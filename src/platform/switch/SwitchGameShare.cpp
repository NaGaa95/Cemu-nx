#include "platform/switch/SwitchGameShare.h"

#if defined(__SWITCH2__)

// (not <switch.h>: its Semaphore clashes with util/helpers/Semaphore.h)
extern "C"
{
#include <switch/types.h>
#include <switch/result.h>
#include <switch/display/native_window.h>
#include <switch/services/acc.h>
#include <switch2/runtime/gameshare.h>
}

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <vector>

#include "Cemu/Logging/CemuLogging.h"
#include "platform/switch/SwitchPlatform.h"

namespace
{
const Result kAppletBadExit = MAKERESULT(Module_Libnx, LibnxError_LibAppletBadExit);
const Result kAppletBadReply = MAKERESULT(Module_Libnx, LibnxError_BadAppletReceiveMessage);
const Result kTimeout = MAKERESULT(Module_Libnx, LibnxError_Timeout);

std::atomic_bool s_requested{false};
std::atomic_bool s_swapRoles{false};
std::atomic_bool s_active{false};
std::atomic_bool s_separate{false};
// The host session and every stpl call that uses it
std::mutex s_hostMutex;
GameShareHost s_host{};
bool s_hostOpen = false;
NWindow s_guestWindow{};
bool s_guestWindowOpen = false;
bool s_guestScreenOpen = false;
bool s_accountReady = false;

// The guest controller, read on the input thread
std::mutex s_inputMutex;
SwitchGameShareInput s_lastInput{};
bool s_haveInput = false;
HidNpadIdType s_guestPadId = HidNpadIdType_No1;
u32 s_guestPadStyle = 0;
int s_sensorProbe = 0;

std::string DescribeFailure(const char* what, Result rc)
{
	return fmt::format("{} failed (0x{:08x}, module {} description {})", what, (unsigned)rc, (unsigned)R_MODULE(rc),
					   (unsigned)R_DESCRIPTION(rc));
}

bool GetHostUser(AccountUid& uid)
{
	if (!s_accountReady)
		s_accountReady = R_SUCCEEDED(accountInitialize(AccountServiceType_Application));
	if (!s_accountReady)
		return false;
	uid = {};
	if (R_SUCCEEDED(accountGetPreselectedUser(&uid)) && accountUidIsValid(&uid))
		return true;
	uid = {};
	if (R_SUCCEEDED(accountGetLastOpenedUser(&uid)) && accountUidIsValid(&uid))
		return true;
	AccountUid users[ACC_USER_LIST_SIZE]{};
	s32 count = 0;
	if (R_SUCCEEDED(accountListAllUsers(users, ACC_USER_LIST_SIZE, &count)) && count > 0 && accountUidIsValid(&users[0]))
	{
		uid = users[0];
		return true;
	}
	return false;
}

// The guest's display ("player1") with the renderer's second swapchain on it: the guest's player view is drawn there
// like on a second monitor. It must exist before matchmaking, which then starts a session with separate views.
bool OpenGuestScreen(std::string& error)
{
	Result rc = nwindowCreateForDisplay(&s_guestWindow, "player1");
	if (R_FAILED(rc))
	{
		error = DescribeFailure("opening the guest's display", rc);
		return false;
	}
	s_guestWindowOpen = true;
	if (!SwitchRenderer_OpenGuestScreen(&s_guestWindow, kSwitchGameShareGuestWidth, kSwitchGameShareGuestHeight))
	{
		error = "the renderer could not present to the guest's display";
		nwindowClose(&s_guestWindow);
		s_guestWindowOpen = false;
		return false;
	}
	s_guestScreenOpen = true;
	return true;
}

void CloseGuestScreen()
{
	s_separate.store(false, std::memory_order_release);
	if (s_guestScreenOpen)
		SwitchRenderer_CloseGuestScreen();
	s_guestScreenOpen = false;
	if (s_guestWindowOpen)
		nwindowClose(&s_guestWindow);
	s_guestWindowOpen = false;
}

void CloseHost()
{
	if (!s_hostOpen)
		return;
	std::scoped_lock lock(s_hostMutex);
	Result rc = gameshareHostClose(&s_host);
	// Timeout: the service has not given the work memory back yet (each call waits about a second)
	for (int attempt = 0; rc == kTimeout && attempt < 5; attempt++)
		rc = gameshareHostClose(&s_host);
	if (R_FAILED(rc))
		cemuLog_log(LogType::Force, "GameShare: {}", DescribeFailure("closing the session", rc));
	s_hostOpen = false;
}
} // namespace

void SwitchGameShare_Configure(bool requested, bool swapRoles)
{
	s_requested.store(requested, std::memory_order_release);
	s_swapRoles.store(swapRoles, std::memory_order_release);
}

bool SwitchGameShare_Requested() { return s_requested.load(std::memory_order_acquire); }
bool SwitchGameShare_SwapRoles() { return s_swapRoles.load(std::memory_order_acquire); }
void SwitchGameShare_SetSwapRoles(bool swapRoles) { s_swapRoles.store(swapRoles, std::memory_order_release); }
bool SwitchGameShare_Active() { return s_active.load(std::memory_order_acquire); }
bool SwitchGameShare_SeparateActive() { return s_active.load(std::memory_order_acquire) && s_separate.load(std::memory_order_acquire); }

SwitchGameShareResult SwitchGameShare_Begin(std::string& error)
{
	error.clear();
	if (s_hostOpen)
		return SwitchGameShareResult::Started;
	AccountUid uid{};
	if (!GetHostUser(uid))
	{
		error = "no user account can host the session";
		return SwitchGameShareResult::Failed;
	}
	const bool separate = SwitchRenderer_SupportsGuestScreen();
	if (separate && !OpenGuestScreen(error))
		return SwitchGameShareResult::Failed;
	s_separate.store(separate, std::memory_order_release);

	GameShareHostConfig config;
	gameshareHostConfigDefault(&config, 1);
	config.mode = separate ? StplStreamingMode_SeparateViews : StplStreamingMode_Mirror;
	config.touch_width = kSwitchGameShareGuestWidth;
	config.touch_height = kSwitchGameShareGuestHeight;
	config.audio = {true, 1.0f, 1.0f, 1.0f};
	config.user = uid;
	s_host = GameShareHost{};
	s_hostOpen = true;
	const Result rc = gameshareHostCreate(&s_host, &config);
	// Closing the matchmaking applet with HOME or B can end it abnormally instead of as a cancel, and then
	// gameshareHostClose does not end the matchmaking: end it here
	if (rc == kAppletBadExit || rc == kAppletBadReply)
	{
		std::scoped_lock lock(s_hostMutex);
		stplEndMatchmaking();
		stplReleaseGuestWorkMemory();
	}
	if (rc == GAMESHARE_RESULT_MATCHMAKING_CANCELLED || rc == kAppletBadExit || rc == kAppletBadReply)
	{
		CloseHost();
		CloseGuestScreen();
		return SwitchGameShareResult::Cancelled;
	}
	if (R_FAILED(rc))
	{
		error = DescribeFailure("starting the session", rc);
		CloseHost();
		CloseGuestScreen();
		return SwitchGameShareResult::Failed;
	}
	s_active.store(true, std::memory_order_release);
	return SwitchGameShareResult::Started;
}

void SwitchGameShare_Update()
{
	if (!SwitchGameShare_Active())
		return;
	bool left;
	{
		std::scoped_lock lock(s_hostMutex);
		left = R_SUCCEEDED(gameshareHostUpdate(&s_host)) && gameshareHostGetGuestsLeft(&s_host);
	}
	if (left)
	{
		cemuLog_log(LogType::Force, "GameShare: the guest disconnected");
		std::scoped_lock lock(s_inputMutex);
		s_haveInput = false;
	}
}

void SwitchGameShare_End()
{
	s_active.store(false, std::memory_order_release);
	{
		std::scoped_lock lock(s_inputMutex);
		s_haveInput = false;
		s_guestPadStyle = 0;
	}
	CloseHost();
	CloseGuestScreen();
}

bool SwitchGameShare_IsGuestAudio(bool tvStream)
{
	return SwitchGameShare_SeparateActive() && tvStream == SwitchGameShare_SwapRoles();
}

void SwitchGameShare_WriteGuestAudio(const int16_t* frames, uint32_t frameCount, int32_t volume)
{
	if (!SwitchGameShare_SeparateActive())
		return;
	thread_local std::vector<int16_t> scaled;
	if (volume < 100)
	{
		scaled.resize(static_cast<size_t>(frameCount) * 2);
		for (size_t i = 0; i < scaled.size(); ++i)
			scaled[i] = static_cast<int16_t>(frames[i] * std::max(volume, 0) / 100);
		frames = scaled.data();
	}
	std::scoped_lock lock(s_hostMutex);
	if (!(gameshareHostGetGuests(&s_host) & 1))
		return; // no guest to hear it
	u32 freeFrames = 0;
	if (R_FAILED(stplGetGuestAudioFreeFrames(0, &freeFrames)) || freeFrames == 0)
		return; // the guest's queue is full: this block is dropped
	u32 written = 0;
	stplWriteGuestAudio(0, frames, std::min(frameCount, freeFrames), &written);
}

// The guest's controllers, merged; false when none is connected. Called with both locks held.
static bool ReadGuestPads(SwitchGameShareInput& input, bool& timedOut)
{
	bool connected = false;
	for (const HidNpadIdType id : {HidNpadIdType_No1, HidNpadIdType_Handheld, HidNpadIdType_No2})
	{
		StplGuestNpadState pad{};
		const Result rc = stplGetGuestNpadState(0, id, &pad);
		timedOut |= rc == kTimeout;
		if (R_FAILED(rc) || !pad.style || !(pad.state.attributes & HidNpadAttribute_IsConnected))
			continue;
		connected = true;
		s_guestPadId = id;
		s_guestPadStyle = pad.style;
		input.buttons |= pad.state.buttons;
		const int32_t values[4] = {pad.state.analog_stick_l.x, pad.state.analog_stick_l.y, pad.state.analog_stick_r.x,
								   pad.state.analog_stick_r.y};
		for (int i = 0; i < 4; ++i)
			if (std::abs(values[i]) > std::abs(input.stick[i]))
				input.stick[i] = values[i];
	}
	return connected;
}

// Read here rather than from the game's last controller poll: the quick menu works whatever the game reads
uint64_t SwitchGameShare_GetGuestButtons()
{
	static uint64_t lastButtons = 0;
	if (!SwitchGameShare_Active())
		return lastButtons = 0;
	std::scoped_lock inputLock(s_inputMutex);
	std::scoped_lock hostLock(s_hostMutex);
	SwitchGameShareInput input{};
	bool timedOut = false;
	if (ReadGuestPads(input, timedOut))
		lastButtons = input.buttons;
	else if (!timedOut)
		lastButtons = 0;
	return lastButtons;
}

bool SwitchGameShare_GetGuestInput(SwitchGameShareInput& out)
{
	if (!SwitchGameShare_Active())
		return false;
	std::scoped_lock inputLock(s_inputMutex);
	std::scoped_lock hostLock(s_hostMutex);
	SwitchGameShareInput input{};
	bool timedOut = false;
	const bool connected = ReadGuestPads(input, timedOut);
	// The service was writing a sample: keep the last one until the next read
	if (timedOut && !connected)
	{
		out = s_lastInput;
		return s_haveInput;
	}
	if (!connected)
	{
		s_haveInput = false;
		return false;
	}

	HidTouchScreenState touch{};
	if (R_SUCCEEDED(stplGetGuestTouchScreenState(0, &touch)) && touch.count > 0)
	{
		input.touchDown = true;
		input.touchX = static_cast<int32_t>(touch.touches[0].x);
		input.touchY = static_cast<int32_t>(touch.touches[0].y);
	}

	// The six-axis sensor that matches the guest's controller; the one that answers is tried first next time
	struct SensorProbe
	{
		HidNpadIdType pad;
		StplSixAxisSensor sensor;
	};
	static constexpr SensorProbe kProbes[] = {
		{HidNpadIdType_Handheld, StplSixAxisSensor_Handheld},
		{HidNpadIdType_No1, StplSixAxisSensor_FullKey},
		{HidNpadIdType_No1, StplSixAxisSensor_JoyDualLeft},
		{HidNpadIdType_No1, StplSixAxisSensor_JoyDualRight},
		{HidNpadIdType_No1, StplSixAxisSensor_JoyLeft},
		{HidNpadIdType_No1, StplSixAxisSensor_JoyRight},
		{HidNpadIdType_No2, StplSixAxisSensor_FullKey},
	};
	constexpr int kProbeCount = static_cast<int>(sizeof(kProbes) / sizeof(kProbes[0]));
	for (int attempt = 0; attempt < kProbeCount; ++attempt)
	{
		const int index = (s_sensorProbe + attempt) % kProbeCount;
		HidSixAxisSensorState state{};
		if (R_FAILED(stplGetGuestSixAxisSensorState(0, kProbes[index].pad, kProbes[index].sensor, &state)) ||
			!(state.attributes & HidSixAxisSensorAttribute_IsConnected) || state.sampling_number == 0)
			continue;
		s_sensorProbe = index;
		input.hasMotion = true;
		input.motionSampling = state.sampling_number;
		input.gyro[0] = state.angular_velocity.x;
		input.gyro[1] = state.angular_velocity.y;
		input.gyro[2] = state.angular_velocity.z;
		input.accel[0] = state.acceleration.x;
		input.accel[1] = state.acceleration.y;
		input.accel[2] = state.acceleration.z;
		break;
	}
	s_lastInput = input;
	s_haveInput = true;
	out = input;
	return true;
}

void SwitchGameShare_SetGuestRumble(float amplitude)
{
	if (!SwitchGameShare_Active())
		return;
	HidNpadIdType padId;
	u32 styles;
	{
		std::scoped_lock lock(s_inputMutex);
		padId = s_guestPadId;
		styles = s_guestPadStyle;
	}
	if (styles == 0)
		return;
	const u32 style = styles & (~styles + 1);
	StplVibrationDeviceHandle handles[2];
	s32 count = 0;
	std::scoped_lock lock(s_hostMutex);
	if (R_FAILED(stplGetGuestVibrationDeviceHandles(handles, 2, &count, 0, padId, style)))
		return;
	const HidVibrationValue value{amplitude, 160.0f, amplitude, 320.0f};
	for (s32 i = 0; i < count; ++i)
		stplSendGuestVibrationValue(handles[i], &value);
}

#else

void SwitchGameShare_Configure(bool, bool) {}
bool SwitchGameShare_Requested() { return false; }
bool SwitchGameShare_SwapRoles() { return false; }
void SwitchGameShare_SetSwapRoles(bool) {}
uint64_t SwitchGameShare_GetGuestButtons() { return 0; }
SwitchGameShareResult SwitchGameShare_Begin(std::string& error)
{
	error = "GameShare needs a Nintendo Switch 2";
	return SwitchGameShareResult::Failed;
}
void SwitchGameShare_Update() {}
void SwitchGameShare_End() {}
bool SwitchGameShare_Active() { return false; }
bool SwitchGameShare_SeparateActive() { return false; }
bool SwitchGameShare_IsGuestAudio(bool) { return false; }
void SwitchGameShare_WriteGuestAudio(const int16_t*, uint32_t, int32_t) {}
bool SwitchGameShare_GetGuestInput(SwitchGameShareInput&) { return false; }
void SwitchGameShare_SetGuestRumble(float) {}

#endif
