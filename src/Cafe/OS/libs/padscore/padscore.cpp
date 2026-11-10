#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/HW/Espresso/PPCCallback.h"
#include "Cafe/OS/libs/padscore/padscore.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"
#include "Cafe/OS/libs/coreinit/coreinit_Alarm.h"
#include "Cafe/OS/libs/coreinit/coreinit_SystemInfo.h"
#include "WindowSystem.h"
#include "input/InputManager.h"

constinit COSStaticData s_padscoreStaticData;

// KPAD

enum class KPAD_ERROR : sint32
{
	NONE = 0,
	NO_SAMPLE_DATA = -1,
	NO_CONTROLLER = -2,
	NOT_INITIALIZED = -5,
};

// WPAD (Wii Controller stuff)

enum WPADRumble
{
	kStopRumble = 0,
	kStartRumble = 1,
};

enum class WPADBatteryLevel : uint8
{
	FULL = 4,
};

enum class WPADLed : uint8
{
	CHAN0 = (1 << 0),
	CHAN1 = (1 << 1),
	CHAN2 = (1 << 2),
	CHAN3 = (1 << 3),
};

namespace padscore 
{
	enum WPADState_t
	{
		kWPADStateMaster = 0,
		kWPADStateShutdown,
		kWPADStateInitializing,
		kWPADStateAcquired,
		kWPADStateReleased,
	};
	KPADUnifiedWpadStatus_t* g_kpad_ringbuffer = nullptr;
	uint32 g_kpad_ringbuffer_length = 0;
	bool g_wpad_callback_by_kpad = false;

	WPADState_t g_wpad_state = kWPADStateMaster;

	struct {
		CafeStaticVar<coreinit::OSAlarm_t> alarm{s_padscoreStaticData};
		bool kpad_initialized = false;

		struct WPADData
		{
			MEMPTR<void> extension_callback;
			MEMPTR<void> connectCallback;
			MEMPTR<void> sampling_callback;
			MEMPTR<void> dpd_callback;
			MEMPTR<void> dpd_control_callback;
			MEMPTR<void> mpls_callback;
			bool dpd_enabled = true;

			struct MplsState
			{
				bool calibrating = false;
				uint64 calibration_start = 0;
				uint32 features = 0;
			} mpls;

			bool disconnectCalled = false;

			BtnRepeat btn_repeat{};
		} controller_data[InputManager::kMaxWPADControllers] = {};

		MEMPTR<void> mpls_workarea;
		int max_controllers = kWPADMaxControllers; // max bt controllers?
	} g_padscore;

	bool IsDpdEnabled(uint32 channel)
	{
		if (channel >= InputManager::kMaxWPADControllers || !g_padscore.controller_data[channel].dpd_enabled)
			return false;
		const auto controller = InputManager::instance().get_wpad_controller(channel);
		return controller && controller->type() == EmulatedController::Wiimote;
	}
}


#pragma region WPAD

void padscoreExport_WPADGetStatus(PPCInterpreter_t* hCPU)
{
	cemuLog_log(LogType::InputAPI, "WPADGetStatus()");
	uint32 status = 1;
	osLib_returnFromFunction(hCPU, status);
}

void padscoreExport_WPADGetBatteryLevel(PPCInterpreter_t* hCPU)
{
	cemuLog_log(LogType::InputAPI, "WPADGetBatteryLevel()");
	osLib_returnFromFunction(hCPU, (uint32)WPADBatteryLevel::FULL);
}

void padscoreExport_WPADProbe(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	ppcDefineParamPtr(type, uint32be, 1);

	cemuLog_log(LogType::InputAPI, "WPADProbe({})", channel);

	if(const auto controller = InputManager::instance().get_wpad_controller(channel))
	{
		if(type)
			*type = controller->get_device_type();

		osLib_returnFromFunction(hCPU, WPAD_ERR_NONE);
	}
	else
	{
		if(type)
			*type = 253;

		osLib_returnFromFunction(hCPU, WPAD_ERR_NO_CONTROLLER);
	}
}

typedef struct
{
	uint32be dpd;
	uint32be speaker;
	uint32be attach;
	uint32be lowBat;
	uint32be nearempty;
	betype<WPADBatteryLevel> batteryLevel;
	betype<WPADLed> led;
	uint8 protocol;
	uint8 firmware;
}WPADInfo_t;

static_assert(sizeof(WPADInfo_t) == 0x18); // unsure

void padscoreExport_WPADGetInfoAsync(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	ppcDefineParamStructPtr(wpadInfo, WPADInfo_t, 1);
	ppcDefineParamMPTR(callbackFunc, 2);
	cemuLog_log(LogType::InputAPI, "WPADGetInfoAsync({}, 0x{:08x}, 0x{:08x})", channel, memory_getVirtualOffsetFromPointer(wpadInfo), callbackFunc);

	if (channel < InputManager::kMaxWPADControllers)
	{
		if (const auto controller = InputManager::instance().get_wpad_controller(channel))
		{
			wpadInfo->dpd = padscore::IsDpdEnabled(channel) ? TRUE : FALSE;
			wpadInfo->speaker = FALSE;
			wpadInfo->attach = FALSE;
			wpadInfo->lowBat = FALSE;
			wpadInfo->nearempty = FALSE;
			wpadInfo->batteryLevel = WPADBatteryLevel::FULL;
			wpadInfo->led = WPADLed::CHAN0;

			if(callbackFunc != MPTR_NULL)
				coreinitAsyncCallback_add(callbackFunc, 2, channel, (uint32)KPAD_ERROR::NONE);

			osLib_returnFromFunction(hCPU, WPAD_ERR_NONE);
			return;
		}
	}
	else
	{
		debugBreakpoint();
	}

	if (callbackFunc != MPTR_NULL)
		coreinitAsyncCallback_add(callbackFunc, 2, channel, (uint32)KPAD_ERROR::NO_CONTROLLER);

	osLib_returnFromFunction(hCPU, WPAD_ERR_NO_CONTROLLER);
}

void padscoreExport_WPADRead(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	ppcDefineParamPtr(wpadStatus, WPADStatus_t, 1);
	cemuLog_log(LogType::InputAPI, "WPADRead({}, {:x})", channel, memory_getVirtualOffsetFromPointer(wpadStatus));

	if (channel < InputManager::kMaxWPADControllers)
	{
		if(const auto controller = InputManager::instance().get_wpad_controller(channel) )
		{
			controller->WPADRead(wpadStatus);
		}
	}
	else
	{
		debugBreakpoint();
	}
	osLib_returnFromFunction(hCPU, WPAD_ERR_NO_CONTROLLER);
}

void padscoreExport_WPADSetDataFormat(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	ppcDefineParamU32(fmt, 1);
	cemuLog_log(LogType::InputAPI, "WPADSetDataFormat({}, {})", channel, fmt);

	if (channel < InputManager::kMaxWPADControllers)
	{
		if (const auto controller = InputManager::instance().get_wpad_controller(channel))
			controller->set_data_format((WPADDataFormat)fmt);
	}

	osLib_returnFromFunction(hCPU, 0);
}

void padscoreExport_WPADGetDataFormat(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	cemuLog_log(LogType::InputAPI, "WPADGetDataFormat({})", channel);

	sint32 dataFormat = kDataFormat_CORE;
	if (channel < InputManager::kMaxWPADControllers)
	{
		if (const auto controller = InputManager::instance().get_wpad_controller(channel))
			dataFormat = controller->get_data_format();
	}
	else
	{
		debugBreakpoint();
	}

	osLib_returnFromFunction(hCPU, dataFormat);
}

void padscoreExport_WPADGetInfo(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	ppcDefineParamStructPtr(wpadInfo, WPADInfo_t, 1);
	cemuLog_log(LogType::InputAPI, "WPADGetInfo({}, 0x{:08x})", channel, memory_getVirtualOffsetFromPointer(wpadInfo));

	if (channel < InputManager::kMaxWPADControllers)
	{
		if (const auto controller = InputManager::instance().get_wpad_controller(channel))
		{
			wpadInfo->dpd = padscore::IsDpdEnabled(channel) ? TRUE : FALSE;
			wpadInfo->speaker = FALSE;
			wpadInfo->attach = FALSE;
			wpadInfo->lowBat = FALSE;
			wpadInfo->nearempty = FALSE;
			wpadInfo->batteryLevel = WPADBatteryLevel::FULL;
			wpadInfo->led = WPADLed::CHAN0;

			osLib_returnFromFunction(hCPU, WPAD_ERR_NONE);
			return;
		}
	}
	else
	{
		debugBreakpoint();
	}

	osLib_returnFromFunction(hCPU, WPAD_ERR_NO_CONTROLLER);
}


void padscoreExport_WPADIsMotorEnabled(PPCInterpreter_t* hCPU)
{
	cemuLog_log(LogType::InputAPI, "WPADIsMotorEnabled()");
	osLib_returnFromFunction(hCPU, TRUE);
}

void padscoreExport_WPADControlMotor(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	ppcDefineParamU32(command, 1);
	cemuLog_log(LogType::InputAPI, "WPADControlMotor({}, {})", channel, command);
	
	if (channel < InputManager::kMaxWPADControllers)
	{
		if (const auto controller = InputManager::instance().get_wpad_controller(channel))
		{
			if( command == kStartRumble )
				controller->start_rumble();
			else
				controller->stop_rumble();
		}
	}
	else
	{
		debugBreakpoint();
	}

	osLib_returnFromFunction(hCPU, 0);
}
#pragma endregion



#pragma region KPAD
void padscoreExport_KPADGetUnifiedWpadStatus(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	ppcDefineParamPtr(status, KPADUnifiedWpadStatus_t, 1);
	ppcDefineParamU32(count, 2);

	cemuLog_log(LogType::InputAPI, "KPADGetUnifiedWpadStatus({}, 0x{:08x}, 0x{:x})", channel, MEMPTR<void>(status).GetMPTR(), count);

	if (count == 0)
	{
		osLib_returnFromFunction(hCPU, 0);
		return;
	}
	if (channel < InputManager::kMaxWPADControllers)
	{
		memset(status, 0x00, sizeof(KPADUnifiedWpadStatus_t) * count);

		if (const auto controller = InputManager::instance().get_wpad_controller(channel))
		{
			controller->WPADRead(&status->u.core);
			status->fmt = controller->get_data_format();
		}
		else
		{
			status->u.core.err = WPAD_ERR_NO_CONTROLLER;
		}
	}
	else
	{
		debugBreakpoint();
	}

	osLib_returnFromFunction(hCPU, 0);
}

void padscoreExport_KPADSetBtnRepeat(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	float delaySec = hCPU->fpr[1].fpr;
	float pulseSec = hCPU->fpr[2].fpr;
	cemuLog_log(LogType::InputAPI, "KPADSetBtnRepeat({}, {}, {})", channel, delaySec, pulseSec);

	if (channel < InputManager::kMaxWPADControllers)
	{
		padscore::g_padscore.controller_data[channel].btn_repeat = { (int)delaySec, (int)pulseSec };
	}

	osLib_returnFromFunction(hCPU, 0);
}

void padscoreExport_KPADSetSamplingCallback(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	ppcDefineParamMPTR(callback, 1);

	cemuLog_log(LogType::InputAPI, "KPADSetSamplingCallback({}, 0x{:x})", channel, callback);

	if (channel >= InputManager::kMaxWPADControllers)
	{
		debugBreakpoint();
		osLib_returnFromFunction(hCPU, MPTR_NULL);
		return;
	}

	const auto old_callback = padscore::g_padscore.controller_data[channel].sampling_callback;
	padscore::g_padscore.controller_data[channel].sampling_callback = callback;
	osLib_returnFromFunction(hCPU, old_callback.GetMPTR());
}

void padscoreExport_WPADControlDpd(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	ppcDefineParamU32(command, 1);
	ppcDefineParamMPTR(callback, 2);

	cemuLog_log(LogType::InputAPI, "WPADControlDpd({}, {}, 0x{:x})", channel, command, callback);

	if (channel < InputManager::kMaxWPADControllers)
	{
		if (const auto controller = InputManager::instance().get_wpad_controller(channel))
		{
			padscore::g_padscore.controller_data[channel].dpd_callback = callback;
			padscore::g_padscore.controller_data[channel].dpd_enabled = command != 0;
			if (callback)
			{
				coreinitAsyncCallback_add(callback, 2, channel, WPAD_ERR_NONE);
			}

			osLib_returnFromFunction(hCPU, WPAD_ERR_NONE);
			return;
		}
	}

	osLib_returnFromFunction(hCPU, WPAD_ERR_NO_CONTROLLER);
}

void padscoreExport_WPADSetExtensionCallback(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	ppcDefineParamMPTR(callback, 1);

	cemuLog_log(LogType::InputAPI, "WPADSetExtensionCallback({}, 0x{:x})", channel, callback);

	if (channel >= InputManager::kMaxWPADControllers)
	{
		debugBreakpoint();
		osLib_returnFromFunction(hCPU, MPTR_NULL);
		return;
	}

	const auto old_callback = padscore::g_padscore.controller_data[channel].extension_callback;
	padscore::g_padscore.controller_data[channel].extension_callback = callback;
	osLib_returnFromFunction(hCPU, old_callback.GetMPTR());
}

void padscoreExport_KPADSetConnectCallback(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	ppcDefineParamMPTR(callback, 1);

	cemuLog_log(LogType::InputAPI, "KPADSetConnectCallback({}, 0x{:x})",channel, callback);

	if (channel >= InputManager::kMaxWPADControllers)
	{
		debugBreakpoint();
		osLib_returnFromFunction(hCPU, MPTR_NULL);
		return;
	}

	const auto old_callback = padscore::g_padscore.controller_data[channel].connectCallback;
	padscore::g_padscore.controller_data[channel].connectCallback = callback;
	osLib_returnFromFunction(hCPU, old_callback.GetMPTR());
}

uint64 g_kpadLastRead[InputManager::kMaxWPADControllers] = {0};
bool g_kpadIsInited = true;

sint32 _KPADRead(uint32 channel, KPADStatus_t* samplingBufs, uint32 length, betype<KPAD_ERROR>* errResult)
{

	if (channel >= InputManager::kMaxWPADControllers)
	{
		debugBreakpoint();
		return 0;
	}
	
	if (g_kpadIsInited == false)
	{
		if (errResult)
			*errResult = KPAD_ERROR::NOT_INITIALIZED;

		return 0;
	}

	const auto controller = InputManager::instance().get_wpad_controller(channel);
	if (!controller)
	{
		if (errResult)
			*errResult = KPAD_ERROR::NO_CONTROLLER;

		return 0;
	}

	// On console new input samples are only received every few ms and calling KPADRead(Ex) clears the internal queue regardless of length value
	// thus calling KPADRead(Ex) again too soon on the same channel will result in no data being returned
	// Games that depend on this: Affordable Space Adventures
	uint64 currentTime = coreinit::OSGetTime();
	uint64 timeDif = currentTime - g_kpadLastRead[channel];
	if(length == 0 || timeDif < coreinit::EspressoTime::ConvertNsToTimerTicks(1000000))
	{
		if (errResult)
			*errResult = KPAD_ERROR::NO_SAMPLE_DATA;
		return 0;
	}
	g_kpadLastRead[channel] = currentTime;

	memset(samplingBufs, 0x00, sizeof(KPADStatus_t));
	samplingBufs->wpadErr = WPAD_ERR_NONE;
	samplingBufs->data_format = controller->get_data_format();
	samplingBufs->devType = controller->get_device_type();
	if(!WindowSystem::InputConfigWindowHasFocus())
	{
		const auto btn_repeat = padscore::g_padscore.controller_data[channel].btn_repeat;
		controller->KPADRead(*samplingBufs, btn_repeat);
		if (!padscore::g_padscore.mpls_workarea)
			memset(&samplingBufs->mpls, 0, sizeof(samplingBufs->mpls));
		if (!padscore::g_padscore.controller_data[channel].dpd_enabled)
		{
			samplingBufs->dpd_valid_fg = 0;
			samplingBufs->pos = {};
			samplingBufs->vec = {};
			samplingBufs->speed = 0.0f;
		}
	}

	if (errResult)
		*errResult = KPAD_ERROR::NONE;

	return 1;
}

void padscoreExport_KPADReadEx(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	ppcDefineParamPtr(kpadStatus, KPADStatus_t, 1);
	ppcDefineParamU32(length, 2);
	ppcDefineParamPtr(errResult, betype<KPAD_ERROR>, 3);
	cemuLog_log(LogType::InputAPI, "KPADReadEx({}, 0x{:x})", channel, length);

	sint32 samplesRead = _KPADRead(channel, kpadStatus, length, errResult);
	osLib_returnFromFunction(hCPU, samplesRead);
}

void padscoreExport_KPADRead(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	ppcDefineParamPtr(kpadStatus, KPADStatus_t, 1);
	ppcDefineParamU32(length, 2);
	cemuLog_log(LogType::InputAPI, "KPADRead({}, 0x{:x})", channel, length);

	sint32 samplesRead = _KPADRead(channel, kpadStatus, length, nullptr);
	osLib_returnFromFunction(hCPU, samplesRead);
}






#pragma endregion



namespace padscore
{
	auto* GetControllerData(sint32 channel)
	{
		return channel >= 0 && channel < InputManager::kMaxWPADControllers ? &g_padscore.controller_data[channel] : nullptr;
	}

	std::shared_ptr<WPADController> GetController(sint32 channel)
	{
		return channel >= 0 ? InputManager::instance().get_wpad_controller(channel) : nullptr;
	}

	void ReturnFloat(PPCInterpreter_t* hCPU, float value)
	{
		hCPU->fpr[1].fpr = value;
		osLib_returnFromFunction(hCPU, 0);
	}

	void NotifyDpdControl(sint32 channel)
	{
		const auto* data = GetControllerData(channel);
		const MPTR callback = data ? data->dpd_control_callback.GetMPTR() : MPTR_NULL;
		if (callback == MPTR_NULL)
			return;
		coreinitAsyncCallback_add(callback, 2, channel, 0);
		coreinitAsyncCallback_add(callback, 2, channel, 1);
	}

	void export_KPADEnableDPD(PPCInterpreter_t* hCPU)
	{
		ppcDefineParamS32(channel, 0);
		cemuLog_log(LogType::InputAPI, "KPADEnableDPD({})", channel);
		cemu_assert_debug(0 <= channel && channel < InputManager::kMaxWPADControllers);

		if(const auto controller = InputManager::instance().get_wpad_controller(channel))
		{
			if (controller->get_data_format() != kDataFormat_FREESTYLE && controller->get_data_format() != 0x1F)
				g_padscore.controller_data[channel].dpd_enabled = true;
		}
		NotifyDpdControl(channel);

		osLib_returnFromFunction(hCPU, 0);
	}

	void KPADDisableDPD(sint32 channel)
	{
		if (auto* data = GetControllerData(channel))
		{
			data->dpd_enabled = false;
			NotifyDpdControl(channel);
		}
	}

	void KPADSetControlDpdCallback(sint32 channel, void* callback)
	{
		if (auto* data = GetControllerData(channel))
			data->dpd_control_callback = callback;
	}

	uint32 WPADIsDpdEnabled(sint32 channel)
	{
		return channel >= 0 && IsDpdEnabled(channel) ? 1 : 0;
	}

	void export_KPADGetMplsWorkSize(PPCInterpreter_t* hCPU)
	{
		cemuLog_log(LogType::InputAPI, "KPADGetMplsWorkSize()");
		osLib_returnFromFunction(hCPU, 0x5FE0);
	}

	void KPADSetMplsWorkarea(void* workarea)
	{
		g_padscore.mpls_workarea = workarea;
	}

	void KPADSetControlMplsCallback(sint32 channel, void* callback)
	{
		if (channel >= 0 && channel < InputManager::kMaxWPADControllers)
			g_padscore.controller_data[channel].mpls_callback = callback;
	}

	uint32 KPADGetMplsStatus(sint32 channel)
	{
		if (channel < 0 || channel >= InputManager::kMaxWPADControllers)
			return 0;
		const auto controller = InputManager::instance().get_wpad_controller(channel);
		return controller ? controller->get_mpls_mode() : 0;
	}

	void KPADEnableMpls(sint32 channel, uint32 mode)
	{
		if (channel < 0 || channel >= InputManager::kMaxWPADControllers)
			return;
		const auto callback = g_padscore.controller_data[channel].mpls_callback.GetMPTR();
		if (callback)
			coreinitAsyncCallback_add(callback, 2, channel, 0);
		const auto controller = InputManager::instance().get_wpad_controller(channel);
		const bool success = !controller || controller->set_mpls_mode(mode);
		sint32 result = 1;
		if (!success)
		{
			result = -1;
			if (mode == 5)
				result = -2;
			else if (mode == 7)
				result = -3;
		}
		if (callback)
			coreinitAsyncCallback_add(callback, 2, channel, result);
	}

	void KPADDisableMpls(sint32 channel)
	{
		KPADEnableMpls(channel, 0);
	}

	constexpr uint64 kMplsCalibrationMs = 1000;

	enum MplsFeature : uint32
	{
		kMplsZeroPlay = 1 << 0,
		kMplsDirRevise = 1 << 1,
		kMplsAccRevise = 1 << 2,
		kMplsDpdRevise = 1 << 3,
	};

	void KPADStartMplsCalibration(sint32 channel)
	{
		if (auto* data = GetControllerData(channel))
		{
			data->mpls.calibrating = true;
			data->mpls.calibration_start = coreinit::OSGetTime();
		}
	}

	void KPADStopMplsCalibration(sint32 channel)
	{
		if (auto* data = GetControllerData(channel))
			data->mpls.calibrating = false;
	}

	void export_KPADWorkMplsCalibration(PPCInterpreter_t* hCPU)
	{
		ppcDefineParamS32(channel, 0);
		// remaining work from 1 to 0, negative while idle
		float remaining = -1.0f;
		const auto* data = GetControllerData(channel);
		if (data && data->mpls.calibrating)
		{
			const uint64 total = coreinit::EspressoTime::ConvertMsToTimerTicks(kMplsCalibrationMs);
			const uint64 elapsed = coreinit::OSGetTime() - data->mpls.calibration_start;
			remaining = elapsed >= total ? 0.0f : 1.0f - (float)elapsed / (float)total;
		}
		cemuLog_log(LogType::InputAPI, "KPADWorkMplsCalibration({}) -> {}", channel, remaining);
		ReturnFloat(hCPU, remaining);
	}

	void KPADResetMpls(sint32 channel)
	{
		if (const auto controller = GetController(channel))
			controller->reset_mpls();
	}

	void KPADReset()
	{
		for (sint32 i = 0; i < InputManager::kMaxWPADControllers; ++i)
			KPADResetMpls(i);
	}

	void KPADSetMplsAngle(sint32 channel, float rx, float ry, float rz)
	{
		if (const auto controller = GetController(channel))
			controller->set_mpls_angle({rx, ry, rz});
	}

	void KPADSetMplsDirection(sint32 channel, KPADMPDir_t* dir)
	{
		const auto controller = GetController(channel);
		if (!controller || !dir)
			return;
		const auto toVec = [](const beVec3D_t& v) { return glm::vec3{(float)v.x, (float)v.y, (float)v.z}; };
		controller->set_mpls_direction({toVec(dir->X), toVec(dir->Y), toVec(dir->Z)});
	}

	void SetMplsFeature(sint32 channel, uint32 feature, bool enabled)
	{
		if (auto* data = GetControllerData(channel))
			data->mpls.features = enabled ? (data->mpls.features | feature) : (data->mpls.features & ~feature);
	}

	void KPADEnableMplsZeroPlay(sint32 channel)
	{
		SetMplsFeature(channel, kMplsZeroPlay, true);
	}

	void KPADDisableMplsZeroPlay(sint32 channel)
	{
		SetMplsFeature(channel, kMplsZeroPlay, false);
	}

	void KPADEnableMplsDirRevise(sint32 channel)
	{
		SetMplsFeature(channel, kMplsDirRevise, true);
	}

	void KPADDisableMplsDirRevise(sint32 channel)
	{
		SetMplsFeature(channel, kMplsDirRevise, false);
	}

	void KPADEnableMplsAccRevise(sint32 channel)
	{
		SetMplsFeature(channel, kMplsAccRevise, true);
	}

	void KPADDisableMplsAccRevise(sint32 channel)
	{
		SetMplsFeature(channel, kMplsAccRevise, false);
	}

	void KPADEnableMplsDpdRevise(sint32 channel)
	{
		SetMplsFeature(channel, kMplsDpdRevise, true);
	}

	void KPADDisableMplsDpdRevise(sint32 channel)
	{
		SetMplsFeature(channel, kMplsDpdRevise, false);
	}

	// 1 while the controller is at rest
	float GetMplsStability(sint32 channel)
	{
		const auto controller = GetController(channel);
		if (!controller || !controller->has_motion())
			return 1.0f;
		auto sample = controller->get_motion_data();
		glm::vec3 gyro;
		sample.getGyrometer(&gyro[0]);
		return glm::length(gyro) < 0.35f ? 1.0f : 0.0f;
	}

	// negative while the feature is off
	float GetMplsFeatureState(sint32 channel, uint32 feature, float enabledValue)
	{
		const auto* data = GetControllerData(channel);
		return data && (data->mpls.features & feature) ? enabledValue : -1.0f;
	}

	void export_KPADIsEnableMplsZeroPlay(PPCInterpreter_t* hCPU)
	{
		ppcDefineParamS32(channel, 0);
		ReturnFloat(hCPU, GetMplsFeatureState(channel, kMplsZeroPlay, GetMplsStability(channel)));
	}

	void export_KPADIsEnableMplsZeroDrift(PPCInterpreter_t* hCPU)
	{
		ppcDefineParamS32(channel, 0);
		ReturnFloat(hCPU, GetControllerData(channel) ? GetMplsStability(channel) : -1.0f);
	}

	void export_KPADIsEnableMplsDirRevise(PPCInterpreter_t* hCPU)
	{
		ppcDefineParamS32(channel, 0);
		ReturnFloat(hCPU, GetMplsFeatureState(channel, kMplsDirRevise, 0.0f));
	}

	void export_KPADIsEnableMplsAccRevise(PPCInterpreter_t* hCPU)
	{
		ppcDefineParamS32(channel, 0);
		ReturnFloat(hCPU, GetMplsFeatureState(channel, kMplsAccRevise, 0.0f));
	}

	void export_KPADIsEnableMplsDpdRevise(PPCInterpreter_t* hCPU)
	{
		ppcDefineParamS32(channel, 0);
		ReturnFloat(hCPU, GetMplsFeatureState(channel, kMplsDpdRevise, 0.0f));
	}

	void KPADInitEx(KPADUnifiedWpadStatus_t ring_buffer[], uint32 length)
	{
		if (g_padscore.kpad_initialized)
			return;

		for (uint32 i = 0; i < InputManager::kMaxWPADControllers; i++)
		{
			g_padscore.controller_data[i].dpd_enabled = true;


		}

		g_kpad_ringbuffer = ring_buffer;
		g_kpad_ringbuffer_length = length;
		g_padscore.kpad_initialized = true;
	}

	void export_KPADInit(PPCInterpreter_t* hCPU)
	{
		cemuLog_log(LogType::InputAPI, "KPADInit()");
		KPADInitEx(nullptr, 0);
		osLib_returnFromFunction(hCPU, 0);
	}

	void export_KPADInitEx(PPCInterpreter_t* hCPU)
	{
		ppcDefineParamMEMPTR(ring_buffer, KPADUnifiedWpadStatus_t, 0);
		ppcDefineParamU32(length, 1);
		cemuLog_log(LogType::InputAPI, "KPADInitEx(0x{:08x}, 0x{:x})", ring_buffer.GetMPTR(), length);
		KPADInitEx(ring_buffer.GetPtr(), length);
		osLib_returnFromFunction(hCPU, 0);
	}

	void WPADSetCallbackByKPAD(bool state)
	{
		g_wpad_callback_by_kpad = state;
	}

	void export_WPADSetCallbackByKPAD(PPCInterpreter_t* hCPU)
	{
		ppcDefineParamU32(state, 0);
		cemuLog_log(LogType::InputAPI, "WPADSetCallbackByKPAD({})", state);
		WPADSetCallbackByKPAD(state);
		osLib_returnFromFunction(hCPU, 0);
	}

	void export_KPADGetMaxControllers(PPCInterpreter_t* hCPU)
	{
		cemuLog_log(LogType::InputAPI, "KPADGetMaxControllers()");
		sint32 max_controllers = g_padscore.max_controllers;
		osLib_returnFromFunction(hCPU, max_controllers);
	}

	bool WPADIsUsedCallbackByKPAD()
	{
		return g_wpad_callback_by_kpad == TRUE;
	}

	void* WPADSetSamplingCallback(sint32 channel, void* callback)
	{
		cemu_assert_debug(0 <= channel && channel < kKPADMaxControllers);

		const auto result = g_padscore.controller_data[channel].sampling_callback;
		g_padscore.controller_data[channel].sampling_callback = callback;
		return result.GetPtr();
	}

	void export_KPADSetMaxControllers(PPCInterpreter_t* hCPU)
	{
		ppcDefineParamU32(new_max_count, 0);
		cemuLog_log(LogType::InputAPI, "KPADSetMaxControllers({})", new_max_count);

		if (new_max_count != kKPADMaxControllers && new_max_count != kWPADMaxControllers)
		{
			debugBreakpoint();
			osLib_returnFromFunction(hCPU, -4);
			return;
		}


		const uint32 max_controllers = g_padscore.max_controllers;
		if (max_controllers == new_max_count)
		{
			osLib_returnFromFunction(hCPU, 0);
			return;
		}

		WPADSetCallbackByKPAD(FALSE);
		for (sint32 i = 0; i < kKPADMaxControllers; i++)
		{
			//WPADSetSamplingCallback(i, 0); TODO
		}

		g_padscore.max_controllers = new_max_count;
		
		WPADSetCallbackByKPAD(true);
		osLib_returnFromFunction(hCPU, new_max_count);
	}

	void export_WPADInit()
	{
		if (g_wpad_state != kWPADStateShutdown)
		{
			if (g_wpad_state == kWPADStateInitializing || g_wpad_state == kWPADStateAcquired)
				return;
		}
		g_wpad_state = kWPADStateInitializing;
	}


	enum class WPADStatus : sint32
	{
		Success = 0,
		NoController = -1,
		Busy = -2,
	};

	WPADStatus WPADIsMplsAttached(sint32 index, uint32be* attached, void* callback)
	{
		if (index < 0 || index >= kKPADMaxControllers || !attached)
			return WPADStatus::NoController;

		const auto controller = InputManager::instance().get_wpad_controller(index);
		*attached = controller && controller->is_mpls_attached();

		if (callback)
			PPCCoreCallback(MEMPTR(callback), index, controller ? WPADStatus::Success : WPADStatus::NoController);

		return WPADStatus::Success;
	}

	struct WPADAcc
	{
		sint32be x;
		sint32be y;
		sint32be z;
	};
	void WPADGetAccGravityUnit(sint32 index, uint32 type, WPADAcc* acc)
	{
		if (index >= kKPADMaxControllers)
			return;

		// default values for wiimote usually are around 99
		acc->x = 99;
		acc->y = 99;
		acc->z = 99;
	}


#pragma endregion

	void TickFunction(PPCInterpreter_t* hCPU)
	{
		auto& instance = InputManager::instance();
		// test for connected/disconnected controllers
		for (auto i = 0; i < InputManager::kMaxWPADControllers; ++i)
		{
			if (g_padscore.controller_data[i].connectCallback) 
			{
				if(!g_padscore.controller_data[i].disconnectCalled)
				{
					g_padscore.controller_data[i].disconnectCalled = true;
					cemuLog_log(LogType::InputAPI, "Calling WPADConnectCallback({}, {})", i, WPAD_ERR_NO_CONTROLLER);
					PPCCoreCallback(g_padscore.controller_data[i].connectCallback, i, WPAD_ERR_NO_CONTROLLER);
					continue;
				}

				if (const auto controller = instance.get_wpad_controller(i))
				{
					if (controller->m_status == WPADController::ConnectCallbackStatus::ReportDisconnect || controller->was_home_button_down()) // fixed!
					{
						controller->m_status = WPADController::ConnectCallbackStatus::ReportConnect;

						cemuLog_log(LogType::InputAPI, "Calling WPADConnectCallback({}, {})", i, WPAD_ERR_NO_CONTROLLER);
						PPCCoreCallback(g_padscore.controller_data[i].connectCallback, i, WPAD_ERR_NO_CONTROLLER);
						
					}
					else if (controller->m_status == WPADController::ConnectCallbackStatus::ReportConnect) 
					{
						controller->m_status = WPADController::ConnectCallbackStatus::None;
						cemuLog_log(LogType::InputAPI, "Calling WPADConnectCallback({}, {})", i, WPAD_ERR_NONE);
						PPCCoreCallback(g_padscore.controller_data[i].connectCallback, i, WPAD_ERR_NONE);
					}
				}
			}
		}

		// test for connected/disconnected extensions
		for (auto i = 0; i < InputManager::kMaxWPADControllers; ++i)
		{
			if (g_padscore.controller_data[i].extension_callback) 
			{
				if (const auto controller = instance.get_wpad_controller(i))
				{
					if (controller->m_extension_status == WPADController::ConnectCallbackStatus::ReportConnect)
					{
						controller->m_extension_status = WPADController::ConnectCallbackStatus::None;
						cemuLog_log(LogType::InputAPI, "Calling WPADextensionCallback({})", i);
						PPCCoreCallback(g_padscore.controller_data[i].extension_callback, i, controller->get_device_type());
					}
				}
			}
		}

		// call sampling callback
		for (auto i = 0; i < InputManager::kMaxWPADControllers; ++i)
		{
			if (g_padscore.controller_data[i].sampling_callback)
			{
				if (const auto controller = instance.get_wpad_controller(i))
				{
					cemuLog_log(LogType::InputAPI, "Calling WPADsamplingCallback({})", i);
					PPCCoreCallback(g_padscore.controller_data[i].sampling_callback, i);
				}
			}
		}

		osLib_returnFromFunction(hCPU, 0);
	}
	void start()
	{
		OSCreateAlarm(&g_padscore.alarm);
		const uint64 start_tick = coreinit::OSGetTime();
		const uint64 period_tick = coreinit::EspressoTime::GetTimerClock() / 200; // every 5ms
		MPTR handler = PPCInterpreter_makeCallableExportDepr(TickFunction);
		OSSetPeriodicAlarm(&g_padscore.alarm, start_tick, period_tick, handler);
	}

	class : public COSModule
	{
		public:
		std::string_view GetName() override
		{
			return "padscore";
		}

		void RPLUnmapped() override
		{
			coreinit::OSCancelAlarm(g_padscore.alarm.GetPtr());
			for (auto& data : g_padscore.controller_data)
			{
				data.mpls_callback = nullptr;
				data.dpd_control_callback = nullptr;
				data.dpd_enabled = true;
				data.mpls = {};
			}
			g_padscore.mpls_workarea = nullptr;
			s_padscoreStaticData.Deallocate();
		}

		void rpl_entry(uint32 moduleHandle, coreinit::RplEntryReason reason) override
		{
			if (reason == coreinit::RplEntryReason::Loaded)
				start();
		}

		void RPLMapped() override
		{
			s_padscoreStaticData.Allocate();
			cafeExportRegister("padscore", KPADSetMplsWorkarea, LogType::InputAPI);
			cafeExportRegister("padscore", KPADSetControlMplsCallback, LogType::InputAPI);
			cafeExportRegister("padscore", KPADGetMplsStatus, LogType::InputAPI);
			cafeExportRegister("padscore", KPADEnableMpls, LogType::InputAPI);
			cafeExportRegister("padscore", KPADDisableMpls, LogType::InputAPI);
			cafeExportRegister("padscore", KPADStartMplsCalibration, LogType::InputAPI);
			cafeExportRegister("padscore", KPADStopMplsCalibration, LogType::InputAPI);
			cafeExportRegister("padscore", KPADResetMpls, LogType::InputAPI);
			cafeExportRegister("padscore", KPADReset, LogType::InputAPI);
			cafeExportRegister("padscore", KPADSetMplsAngle, LogType::InputAPI);
			cafeExportRegister("padscore", KPADSetMplsDirection, LogType::InputAPI);
			cafeExportRegister("padscore", KPADEnableMplsZeroPlay, LogType::InputAPI);
			cafeExportRegister("padscore", KPADDisableMplsZeroPlay, LogType::InputAPI);
			cafeExportRegister("padscore", KPADEnableMplsDirRevise, LogType::InputAPI);
			cafeExportRegister("padscore", KPADDisableMplsDirRevise, LogType::InputAPI);
			cafeExportRegister("padscore", KPADEnableMplsAccRevise, LogType::InputAPI);
			cafeExportRegister("padscore", KPADDisableMplsAccRevise, LogType::InputAPI);
			cafeExportRegister("padscore", KPADEnableMplsDpdRevise, LogType::InputAPI);
			cafeExportRegister("padscore", KPADDisableMplsDpdRevise, LogType::InputAPI);
			cafeExportRegister("padscore", KPADDisableDPD, LogType::InputAPI);
			cafeExportRegister("padscore", KPADSetControlDpdCallback, LogType::InputAPI);
			cafeExportRegister("padscore", WPADIsDpdEnabled, LogType::InputAPI);
			cafeExportRegister("padscore", WPADIsMplsAttached, LogType::InputAPI);
			cafeExportRegister("padscore", WPADGetAccGravityUnit, LogType::InputAPI);

			// wpad
			//osLib_addFunction("padscore", "WPADInit", padscore::export_WPADInit);

			// kpad
			osLib_addFunction("padscore", "KPADSetMaxControllers", padscore::export_KPADSetMaxControllers);
			osLib_addFunction("padscore", "KPADGetMaxControllers", padscore::export_KPADGetMaxControllers);
			osLib_addFunction("padscore", "KPADEnableDPD", padscore::export_KPADEnableDPD);
			osLib_addFunction("padscore", "KPADGetMplsWorkSize", padscore::export_KPADGetMplsWorkSize);
			osLib_addFunction("padscore", "KPADWorkMplsCalibration", padscore::export_KPADWorkMplsCalibration);
			osLib_addFunction("padscore", "KPADIsEnableMplsZeroPlay", padscore::export_KPADIsEnableMplsZeroPlay);
			osLib_addFunction("padscore", "KPADIsEnableMplsZeroDrift", padscore::export_KPADIsEnableMplsZeroDrift);
			osLib_addFunction("padscore", "KPADIsEnableMplsDirRevise", padscore::export_KPADIsEnableMplsDirRevise);
			osLib_addFunction("padscore", "KPADIsEnableMplsAccRevise", padscore::export_KPADIsEnableMplsAccRevise);
			osLib_addFunction("padscore", "KPADIsEnableMplsDpdRevise", padscore::export_KPADIsEnableMplsDpdRevise);
			osLib_addFunction("padscore", "KPADInit", padscore::export_KPADInit);
			osLib_addFunction("padscore", "KPADInitEx", padscore::export_KPADInitEx);

			osLib_addFunction("padscore", "KPADSetConnectCallback", padscoreExport_KPADSetConnectCallback);
			osLib_addFunction("padscore", "KPADReadEx", padscoreExport_KPADReadEx);
			osLib_addFunction("padscore", "KPADRead", padscoreExport_KPADRead);
			osLib_addFunction("padscore", "KPADGetUnifiedWpadStatus", padscoreExport_KPADGetUnifiedWpadStatus);
			osLib_addFunction("padscore", "KPADSetSamplingCallback", padscoreExport_KPADSetSamplingCallback);
			osLib_addFunction("padscore", "KPADSetBtnRepeat", padscoreExport_KPADSetBtnRepeat);

			osLib_addFunction("padscore", "WPADGetBatteryLevel", padscoreExport_WPADGetBatteryLevel);
			osLib_addFunction("padscore", "WPADControlMotor", padscoreExport_WPADControlMotor);
			osLib_addFunction("padscore", "WPADIsMotorEnabled", padscoreExport_WPADIsMotorEnabled);
			osLib_addFunction("padscore", "WPADGetStatus", padscoreExport_WPADGetStatus);
			osLib_addFunction("padscore", "WPADProbe", padscoreExport_WPADProbe);
			osLib_addFunction("padscore", "WPADGetInfoAsync", padscoreExport_WPADGetInfoAsync);
			osLib_addFunction("padscore", "WPADGetInfo", padscoreExport_WPADGetInfo);
			osLib_addFunction("padscore", "WPADSetConnectCallback", padscoreExport_KPADSetConnectCallback);
			osLib_addFunction("padscore", "WPADSetDataFormat", padscoreExport_WPADSetDataFormat);
			osLib_addFunction("padscore", "WPADGetDataFormat", padscoreExport_WPADGetDataFormat);
			osLib_addFunction("padscore", "WPADRead", padscoreExport_WPADRead);
			osLib_addFunction("padscore", "WPADSetExtensionCallback", padscoreExport_WPADSetExtensionCallback);
			osLib_addFunction("padscore", "WPADSetSamplingCallback", padscoreExport_KPADSetSamplingCallback);
			osLib_addFunction("padscore", "WPADControlDpd", padscoreExport_WPADControlDpd);

			osLib_addFunction("padscore", "WPADSetCallbackByKPAD", padscore::export_WPADSetCallbackByKPAD);
		};

	}s_COSCoreinitModule;

	COSModule* GetModule()
	{
		return &s_COSCoreinitModule;
	}

}
