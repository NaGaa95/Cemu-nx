extern "C" {
#include <switch/runtime/pad.h>
#include <switch/services/hid.h>
}

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "input/InputManager.h"
#include "input/api/Controller.h"
#include "input/emulated/VPADController.h"
#include "input/emulated/ProController.h"
#include "input/emulated/ClassicController.h"
#include "input/emulated/WiimoteController.h"
#include "input/motion/MotionHandler.h"
#include "platform/switch/SwitchInput.h"
#include "platform/switch/SwitchOverlay.h"
#include "platform/switch/SwitchPlatform.h"

namespace
{
constexpr uint32 kNoCode = 0xFFFFFFFF;
constexpr size_t kSwitchControllerCount = InputManager::kMaxController;
constexpr float kTau = 6.2831853071795864769f;
// rotation that sweeps the pointer across the whole screen, in radians
constexpr float kPointerYawRange = 0.6981317f;
constexpr float kPointerPitchRange = 0.3926991f;

constexpr std::array<HidNpadIdType, kSwitchControllerCount> kNpadIds = {
	HidNpadIdType_No1,
	HidNpadIdType_No2,
	HidNpadIdType_No3,
	HidNpadIdType_No4,
	HidNpadIdType_No5,
	HidNpadIdType_No6,
	HidNpadIdType_No7,
	HidNpadIdType_No8,
};

class SwitchGamepadController : public ControllerBase
{
  public:
	SwitchGamepadController(size_t playerIndex, HidNpadIdType npadId, bool useHandheld, bool gamePadFeatures, bool enableMotion, bool enablePointer)
		: ControllerBase(fmt::format("switch-{}", playerIndex), fmt::format("Switch Controller {}", playerIndex + 1)),
		  m_npadId(npadId), m_useHandheld(useHandheld), m_hasTouch(gamePadFeatures),
		  m_enableMotion(enableMotion), m_hasPointer(enablePointer)
	{
		u64 mask = BITL(static_cast<u32>(m_npadId));
		if (m_useHandheld)
			mask |= BITL(static_cast<u32>(HidNpadIdType_Handheld));
		padInitializeWithMask(&m_pad, mask);
		padUpdate(&m_pad);
		m_connected = padIsConnected(&m_pad);
		if (m_hasTouch)
			hidInitializeTouchScreen();

		initializeVibration();
		if (m_enableMotion)
			initializeMotion();
	}
	~SwitchGamepadController() override
	{
		sendVibration(0.0f);
		for (size_t i = 0; i < m_motionHandles.size(); ++i)
		{
			if (m_motionReady[i])
				(void)hidStopSixAxisSensor(m_motionHandles[i]);
		}
	}

	std::string_view api_name() const override { return "Switch"; }
	InputAPI::Type api() const override { return InputAPI::SDLController; }
	bool is_connected() override
	{
		std::scoped_lock lock(m_padMutex);
		return m_connected;
	}
	bool has_motion() override
	{
		std::scoped_lock lock(m_padMutex);
		return m_enableMotion && m_hasMotion;
	}
	MotionSample get_motion_sample() override
	{
		std::scoped_lock lock(m_motionMutex);
		return m_motionSample;
	}
	bool has_rumble() override
	{
		std::scoped_lock lock(m_padMutex);
		return std::any_of(m_vibration.begin(), m_vibration.end(), [](const auto& device) { return device.ready; });
	}
	void start_rumble() override { sendVibration(std::clamp(get_settings().rumble, 0.0f, 1.0f)); }
	void stop_rumble() override { sendVibration(0.0f); }
	bool usesGamePadFeatures() const { return m_hasTouch; }
	bool usesMotion() const
	{
		return m_enableMotion;
	}
	bool usesPointer() const
	{
		return m_hasPointer;
	}
	void setRecenterButton(uint32 code)
	{
		std::scoped_lock lock(m_padMutex);
		m_recenterCode = code;
	}

	bool has_position() override
	{
		std::scoped_lock lock(m_padMutex);
		return m_hasPointer ? m_connected : m_touchDown;
	}
	glm::vec2 get_position() override
	{
		std::scoped_lock lock(m_padMutex);
		return m_hasPointer ? m_pointerPos : m_touchPos;
	}
	glm::vec2 get_prev_position() override
	{
		std::scoped_lock lock(m_padMutex);
		return m_hasPointer ? m_pointerPrevPos : m_touchPrevPos;
	}
	PositionVisibility GetPositionVisibility() override
	{
		std::scoped_lock lock(m_padMutex);
		return (m_hasPointer ? m_connected : m_touchDown) ? PositionVisibility::FULL : PositionVisibility::NONE;
	}

	ControllerState raw_state() override
	{
		std::scoped_lock lock(m_padMutex);
		m_cachedState = pollStateLocked();
		return m_cachedState;
	}

  private:
	ControllerState pollStateLocked()
	{
		padUpdate(&m_pad);
		m_connected = padIsConnected(&m_pad);
		updateMotion();
		const uint64_t b = padGetButtons(&m_pad);
		if (SwitchOverlay_IsInputCaptured(b))
		{
			const bool clearMouse = m_touchDown || m_mainMouseWasDown;
			m_touchPrevPos = m_touchPos;
			m_touchDown = false;
			if (clearMouse)
			{
				auto& im = InputManager::instance();
				std::scoped_lock lock(im.m_main_mouse.m_mutex);
				im.m_main_mouse.left_down = false;
				im.m_main_mouse.left_down_toggle = false;
			}
			m_mainMouseWasDown = false;
			return {};
		}

		if (m_hasTouch)
		{
			const bool previousTouchDown = m_touchDown;
			const glm::vec2 previousTouchPos = m_touchPos;
			HidTouchScreenState ts{};
			if (hidGetTouchScreenStates(&ts, 1) && ts.count > 0)
			{
				const float nx = std::clamp((float)ts.touches[0].x / 1280.0f, 0.0f, 1.0f);
				const float ny = std::clamp((float)ts.touches[0].y / 720.0f, 0.0f, 1.0f);
				glm::vec2 nextTouch{nx, ny};
				bool touchAccepted = true;
				if (SwitchPlatform_IsGamePadCompositeActive())
				{
					int x, y, width, height;
					SwitchPlatform_GetGamePadImageRect(true, 1280, 720, 854, 480, x, y, width, height);
					const int touchX = ts.touches[0].x;
					const int touchY = ts.touches[0].y;
					touchAccepted = touchX >= x && touchX < x + width && touchY >= y && touchY < y + height;
					if (touchAccepted)
						nextTouch = {(float)(touchX - x) / width, (float)(touchY - y) / height};
				}
				if (touchAccepted)
				{
					m_touchPrevPos = m_touchDown ? m_touchPos : nextTouch;
					m_touchPos = nextTouch;
					m_touchDown = true;
				}
				else
				{
					m_touchPrevPos = m_touchPos;
					m_touchDown = false;
				}
			}
			else
			{
				m_touchPrevPos = m_touchPos;
				m_touchDown = false;
			}
			const bool touchChanged = previousTouchDown != m_touchDown ||
				(m_touchDown && (previousTouchPos.x != m_touchPos.x || previousTouchPos.y != m_touchPos.y));
			if (touchChanged)
			{
				auto& im = InputManager::instance();
				std::scoped_lock lock(im.m_main_mouse.m_mutex);
				if (m_touchDown)
					im.m_main_mouse.position = {(int)(m_touchPos.x * 1280.0f), (int)(m_touchPos.y * 720.0f)};
				im.m_main_mouse.left_down = m_touchDown;
				if (m_touchDown && !m_mainMouseWasDown)
					im.m_main_mouse.left_down_toggle = true;
			}
			m_mainMouseWasDown = m_touchDown;
		}

		ControllerState s{};
		auto set = [&](uint32 code, bool pressed) { if (pressed) s.buttons.SetButtonState(code, true); };
		set(kButton0, b & HidNpadButton_A);
		set(kButton1, b & HidNpadButton_B);
		set(kButton2, b & HidNpadButton_X);
		set(kButton3, b & HidNpadButton_Y);
		set(kButton4, b & (HidNpadButton_L | HidNpadButton_AnySL));
		set(kButton5, b & (HidNpadButton_R | HidNpadButton_AnySR));
		set(kButtonZL, b & HidNpadButton_ZL);
		set(kButtonZR, b & HidNpadButton_ZR);
		set(kButton6, b & HidNpadButton_Plus);
		set(kButton7, b & HidNpadButton_Minus);
		set(kButtonUp, b & HidNpadButton_Up);
		set(kButtonDown, b & HidNpadButton_Down);
		set(kButtonLeft, b & HidNpadButton_Left);
		set(kButtonRight, b & HidNpadButton_Right);
		set(kButton8, b & HidNpadButton_StickL);
		set(kButton9, b & HidNpadButton_StickR);

		constexpr float kInv = 1.0f / 32767.0f;
		const float lx = std::clamp(m_pad.sticks[0].x * kInv, -1.0f, 1.0f);
		const float ly = std::clamp(m_pad.sticks[0].y * kInv, -1.0f, 1.0f);
		const float rx = std::clamp(m_pad.sticks[1].x * kInv, -1.0f, 1.0f);
		const float ry = std::clamp(m_pad.sticks[1].y * kInv, -1.0f, 1.0f);
		s.axis = {lx, ly};
		s.rotation = {rx, ry};
		constexpr float th = 0.15f;
		set(kAxisXP, lx > th);
		set(kAxisXN, lx < -th);
		set(kAxisYP, ly > th);
		set(kAxisYN, ly < -th);
		set(kRotationXP, rx > th);
		set(kRotationXN, rx < -th);
		set(kRotationYP, ry > th);
		set(kRotationYN, ry < -th);
		if (m_hasPointer && m_recenterCode != kNoCode && s.buttons.GetButtonState(m_recenterCode))
			m_pointerPos = m_pointerPrevPos = {0.5f, 0.5f};
		return s;
	}

	struct VibrationDevice
	{
		HidNpadIdType id{};
		HidNpadStyleTag style{};
		std::array<HidVibrationDeviceHandle, 2> handles{};
		s32 count = 0;
		bool ready = false;
	};

	void initializeVibrationDevice(size_t index, HidNpadIdType id, HidNpadStyleTag style, s32 count)
	{
		auto& device = m_vibration[index];
		if (device.ready)
			return;
		device.id = id;
		device.style = style;
		device.count = count;
		device.ready = R_SUCCEEDED(hidInitializeVibrationDevices(device.handles.data(), count, id, style));
	}

	void initializeVibration()
	{
		const u32 style = padGetStyleSet(&m_pad);
		if (m_useHandheld && padIsHandheld(&m_pad))
			initializeVibrationDevice(0, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld, 2);
		else if (style & HidNpadStyleTag_NpadFullKey)
			initializeVibrationDevice(1, m_npadId, HidNpadStyleTag_NpadFullKey, 2);
		else if (style & HidNpadStyleTag_NpadJoyDual)
			initializeVibrationDevice(2, m_npadId, HidNpadStyleTag_NpadJoyDual, 2);
		else if (style & HidNpadStyleTag_NpadJoyLeft)
			initializeVibrationDevice(3, m_npadId, HidNpadStyleTag_NpadJoyLeft, 1);
		else if (style & HidNpadStyleTag_NpadJoyRight)
			initializeVibrationDevice(4, m_npadId, HidNpadStyleTag_NpadJoyRight, 1);
	}

	void initializeMotion()
	{
		if (!m_enableMotion)
			return;
		auto initializeSingle = [&](size_t index, HidNpadIdType id, HidNpadStyleTag style) {
			if (m_motionReady[index])
				return;
			HidSixAxisSensorHandle handle{};
			if (R_SUCCEEDED(hidGetSixAxisSensorHandles(&handle, 1, id, style)) &&
				R_SUCCEEDED(hidStartSixAxisSensor(handle)))
			{
				m_motionHandles[index] = handle;
				m_motionReady[index] = true;
				m_hasMotion = true;
			}
		};

		const u32 style = padGetStyleSet(&m_pad);
		if (m_useHandheld && padIsHandheld(&m_pad))
			initializeSingle(0, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld);
		else if (style & HidNpadStyleTag_NpadFullKey)
			initializeSingle(1, m_npadId, HidNpadStyleTag_NpadFullKey);
		else if (style & HidNpadStyleTag_NpadJoyDual)
		{
			std::array<HidSixAxisSensorHandle, 2> handles{};
			if (R_SUCCEEDED(hidGetSixAxisSensorHandles(handles.data(), 2,
					m_npadId, HidNpadStyleTag_NpadJoyDual)))
			{
				const u32 attributes = padGetAttributes(&m_pad);
				const size_t preferred = (attributes & HidNpadAttribute_IsRightConnected) ? 1 : 0;
				const size_t index = preferred + 2;
				if (!m_motionReady[index] && R_SUCCEEDED(hidStartSixAxisSensor(handles[preferred])))
				{
					m_motionHandles[index] = handles[preferred];
					m_motionReady[index] = true;
					m_hasMotion = true;
				}
			}
		}
		else if (style & HidNpadStyleTag_NpadJoyLeft)
			initializeSingle(4, m_npadId, HidNpadStyleTag_NpadJoyLeft);
		else if (style & HidNpadStyleTag_NpadJoyRight)
			initializeSingle(5, m_npadId, HidNpadStyleTag_NpadJoyRight);
	}

	int getActiveMotionHandle() const
	{
		const u32 style = padGetStyleSet(&m_pad);
		if (m_useHandheld && padIsHandheld(&m_pad) && m_motionReady[0])
			return 0;
		if ((style & HidNpadStyleTag_NpadFullKey) && m_motionReady[1])
			return 1;
		if (style & HidNpadStyleTag_NpadJoyDual)
		{
			const u32 attributes = padGetAttributes(&m_pad);
			// Prefer the right Joy-Con for aiming.
			if ((attributes & HidNpadAttribute_IsRightConnected) && m_motionReady[3])
				return 3;
			if ((attributes & HidNpadAttribute_IsLeftConnected) && m_motionReady[2])
				return 2;
		}
		if ((style & HidNpadStyleTag_NpadJoyLeft) && m_motionReady[4])
			return 4;
		if ((style & HidNpadStyleTag_NpadJoyRight) && m_motionReady[5])
			return 5;
		return -1;
	}

	void updateMotion()
	{
		if (!m_enableMotion)
			return;
		int handleIndex = getActiveMotionHandle();
		const auto now = std::chrono::steady_clock::now();
		if (handleIndex < 0 && now >= m_nextMotionProbe)
		{
			m_nextMotionProbe = now + std::chrono::seconds(1);
			initializeMotion();
			handleIndex = getActiveMotionHandle();
		}
		if (handleIndex < 0)
			return;
		if (m_activeMotionHandle != handleIndex)
		{
			for (size_t i = 0; i < m_motionHandles.size(); ++i)
			{
				if (i != static_cast<size_t>(handleIndex) && m_motionReady[i])
				{
					(void)hidStopSixAxisSensor(m_motionHandles[i]);
					m_motionReady[i] = false;
					m_motionHasSample[i] = false;
				}
			}
			m_activeMotionHandle = handleIndex;
		}

		HidSixAxisSensorState state{};
		if (hidGetSixAxisSensorStates(m_motionHandles[handleIndex], &state, 1) != 1 ||
			!(state.attributes & HidSixAxisSensorAttribute_IsConnected))
		{
			(void)hidStopSixAxisSensor(m_motionHandles[handleIndex]);
			m_motionReady[handleIndex] = false;
			m_motionHasSample[handleIndex] = false;
			m_activeMotionHandle = -1;
			m_nextMotionProbe = now + std::chrono::seconds(1);
			return;
		}

		const size_t index = static_cast<size_t>(handleIndex);
		if (m_motionHasSample[index] && state.sampling_number == m_motionSamplingNumber[index])
			return;

		float deltaTime = 1.0f / 60.0f;
		if (m_motionHasSample[index])
			deltaTime = std::chrono::duration<float>(now - m_motionSampleTime[index]).count();
		deltaTime = std::clamp(deltaTime, 0.001f, 0.1f);

		m_motionHasSample[index] = true;
		m_motionSamplingNumber[index] = state.sampling_number;
		m_motionSampleTime[index] = now;

		const auto& gyro = state.angular_velocity;
		const auto& accel = state.acceleration;
		if (!std::isfinite(gyro.x) || !std::isfinite(gyro.y) || !std::isfinite(gyro.z) ||
			!std::isfinite(accel.x) || !std::isfinite(accel.y) || !std::isfinite(accel.z))
			return;

		// Convert HOS sensor coordinates and revolutions/sec to Cemu's SDL convention.
		m_motionHandlers[index].processMotionSample(deltaTime,
			gyro.x * kTau, -gyro.z * kTau, gyro.y * kTau,
			accel.x, -accel.z, accel.y);
		const MotionSample sample = m_motionHandlers[index].getMotionSample();
		{
			std::scoped_lock lock(m_motionMutex);
			m_motionSample = sample;
		}
		if (m_hasPointer)
			updatePointer(sample, deltaTime);
	}

	void updatePointer(MotionSample sample, float deltaTime)
	{
		glm::vec3 gyro, acc;
		sample.getGyrometer(&gyro[0]);
		sample.getAccelerometer(&acc[0]);

		// track gravity only while the controller is not being swung
		const float accLength = glm::length(acc);
		if (accLength > 0.5f && accLength < 1.5f)
			m_pointerDown = glm::mix(m_pointerDown, acc / accLength, std::min(1.0f, deltaTime * 4.0f));
		const float downLength = glm::length(m_pointerDown);
		const glm::vec3 down = downLength > 0.001f ? m_pointerDown / downLength : glm::vec3{0.0f, 1.0f, 0.0f};
		glm::vec3 right{down.y, -down.x, 0.0f};
		const float rightLength = glm::length(right);
		right = rightLength > 0.2f ? right / rightLength : glm::vec3{1.0f, 0.0f, 0.0f};

		float yaw = glm::dot(gyro, down);
		float pitch = glm::dot(gyro, right);
		if (std::abs(yaw) < 0.02f)
			yaw = 0.0f;
		if (std::abs(pitch) < 0.02f)
			pitch = 0.0f;

		m_pointerPrevPos = m_pointerPos;
		m_pointerPos.x = std::clamp(m_pointerPos.x + yaw * deltaTime / kPointerYawRange, 0.0f, 1.0f);
		m_pointerPos.y = std::clamp(m_pointerPos.y - pitch * deltaTime / kPointerPitchRange, 0.0f, 1.0f);
	}

	void sendVibration(float amp)
	{
		std::scoped_lock lock(m_padMutex);
		initializeVibration();
		const u32 style = padGetStyleSet(&m_pad);
		VibrationDevice* active = nullptr;
		if (m_useHandheld && padIsHandheld(&m_pad) && m_vibration[0].ready)
			active = &m_vibration[0];
		else
		{
			for (size_t i = 1; i < m_vibration.size(); ++i)
			{
				if (m_vibration[i].ready && (style & m_vibration[i].style))
				{
					active = &m_vibration[i];
					break;
				}
			}
		}
		if (!active)
			return;

		std::array<HidVibrationValue, 2> values{};
		for (s32 i = 0; i < active->count; ++i)
			values[i] = HidVibrationValue{amp, 160.0f, amp, 320.0f};
		hidSendVibrationValues(active->handles.data(), values.data(), active->count);
	}

	PadState m_pad{};
	HidNpadIdType m_npadId;
	bool m_useHandheld = false;
	bool m_hasTouch = false;
	bool m_enableMotion = false;
	bool m_hasPointer = false;
	std::mutex m_padMutex;
	ControllerState m_cachedState{};
	bool m_connected = false;
	bool m_touchDown = false;
	glm::vec2 m_touchPos{};
	glm::vec2 m_touchPrevPos{};
	glm::vec2 m_pointerPos{0.5f, 0.5f};
	glm::vec2 m_pointerPrevPos{0.5f, 0.5f};
	glm::vec3 m_pointerDown{0.0f, 1.0f, 0.0f};
	uint32 m_recenterCode = kButton9;
	bool m_mainMouseWasDown = false;
	std::array<VibrationDevice, 5> m_vibration{};
	std::array<HidSixAxisSensorHandle, 6> m_motionHandles{};
	std::array<bool, 6> m_motionReady{};
	std::array<bool, 6> m_motionHasSample{};
	std::array<u64, 6> m_motionSamplingNumber{};
	std::array<std::chrono::steady_clock::time_point, 6> m_motionSampleTime{};
	std::array<WiiUMotionHandler, 6> m_motionHandlers{};
	std::chrono::steady_clock::time_point m_nextMotionProbe{};
	int m_activeMotionHandle = -1;
	std::mutex m_motionMutex;
	MotionSample m_motionSample{};
	bool m_hasMotion = false;
};

struct InputConfig
{
	std::string type = "GamePad";
	std::string secondaryType = "Pro";
	bool rumble = true;
	int deadzone = 15;
	int players = 1;
	std::unordered_map<std::string, std::string> map;
};

static std::string trimStr(const std::string& s)
{
	size_t a = s.find_first_not_of(" \t\r\n");
	size_t b = s.find_last_not_of(" \t\r\n");
	return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

static InputConfig loadInputConfig(const char* path)
{
	InputConfig cfg;
	FILE* f = fopen(path, "r");
	if (!f)
		return cfg;
	char line[256];
	std::string section;
	while (fgets(line, sizeof(line), f))
	{
		std::string t = trimStr(line);
		if (t.empty() || t[0] == '#' || t[0] == ';')
			continue;
		if (t.front() == '[' && t.back() == ']') { section = t.substr(1, t.size() - 2); continue; }
		size_t eq = t.find('=');
		if (eq == std::string::npos)
			continue;
		std::string k = trimStr(t.substr(0, eq)), v = trimStr(t.substr(eq + 1));
		if (section == "controller")
		{
			if (k == "type") cfg.type = v;
			else if (k == "secondary_type")
				cfg.secondaryType = v;
			else if (k == "rumble") cfg.rumble = (v == "on" || v == "true" || v == "1");
			else if (k == "deadzone") cfg.deadzone = std::clamp(std::atoi(v.c_str()), 0, 50);
			else if (k == "players") cfg.players = std::clamp(std::atoi(v.c_str()), 1, static_cast<int>(kSwitchControllerCount));
		}
		else if (section == "map")
		{
			for (auto& c : v) c = (char)toupper((unsigned char)c);
			cfg.map[k] = v;
		}
	}
	fclose(f);
	return cfg;
}

static uint32 physCode(const std::string& tok)
{
	static const std::unordered_map<std::string, uint32> m = {
		{"A", kButton0}, {"B", kButton1}, {"X", kButton2}, {"Y", kButton3},
		{"L", kButton4}, {"R", kButton5}, {"ZL", kButtonZL}, {"ZR", kButtonZR},
		{"PLUS", kButton6}, {"MINUS", kButton7},
		{"DUP", kButtonUp}, {"DDOWN", kButtonDown}, {"DLEFT", kButtonLeft}, {"DRIGHT", kButtonRight},
		{"LCLICK", kButton8}, {"RCLICK", kButton9},
	};
	auto it = m.find(tok);
	return it == m.end() ? kNoCode : it->second;
}

InputConfig s_config;
std::array<std::shared_ptr<SwitchGamepadController>, kSwitchControllerCount> s_controllers;
std::atomic<SwitchControllerType> s_controllerType{SwitchControllerType::GamePad};
std::atomic<SwitchControllerType> s_secondaryControllerType{SwitchControllerType::Pro};

SwitchControllerType ParseControllerType(const std::string& type, SwitchControllerType fallback)
{
	if (type == "GamePad")
		return SwitchControllerType::GamePad;
	if (type == "Pro")
		return SwitchControllerType::Pro;
	if (type == "Classic")
		return SwitchControllerType::Classic;
	if (type == "Wiimote")
		return SwitchControllerType::Wiimote;
	if (type == "WiimoteNunchuk")
		return SwitchControllerType::WiimoteNunchuk;
	return fallback;
}

EmulatedController::Type ToEmulatedType(SwitchControllerType type)
{
	switch (type)
	{
	case SwitchControllerType::Pro:
		return EmulatedController::Type::Pro;
	case SwitchControllerType::Classic:
		return EmulatedController::Type::Classic;
	case SwitchControllerType::Wiimote:
	case SwitchControllerType::WiimoteNunchuk:
		return EmulatedController::Type::Wiimote;
	case SwitchControllerType::GamePad:
	default:
		return EmulatedController::Type::VPAD;
	}
}

bool ConfigureMappings(const EmulatedControllerPtr& emu, EmulatedController::Type emuType,
	const std::shared_ptr<SwitchGamepadController>& physical)
{
	if (!emu || !physical)
		return false;
	emu->add_controller(physical);

	auto mapBtn = [&](uint64 emuId, const char* name, const char* defTok) {
		auto it = s_config.map.find(name);
		uint32 code = physCode(it != s_config.map.end() ? it->second : defTok);
		if (code != kNoCode)
			emu->set_mapping(emuId, physical, code);
	};

#define MAP_COMMON(T)                              \
	mapBtn(T::kButtonId_A, "A", "A");              \
	mapBtn(T::kButtonId_B, "B", "B");              \
	mapBtn(T::kButtonId_X, "X", "X");              \
	mapBtn(T::kButtonId_Y, "Y", "Y");              \
	mapBtn(T::kButtonId_L, "L", "L");              \
	mapBtn(T::kButtonId_R, "R", "R");              \
	mapBtn(T::kButtonId_ZL, "ZL", "ZL");           \
	mapBtn(T::kButtonId_ZR, "ZR", "ZR");           \
	mapBtn(T::kButtonId_Plus, "Plus", "PLUS");     \
	mapBtn(T::kButtonId_Minus, "Minus", "MINUS");  \
	mapBtn(T::kButtonId_Up, "Up", "DUP");          \
	mapBtn(T::kButtonId_Down, "Down", "DDOWN");    \
	mapBtn(T::kButtonId_Left, "Left", "DLEFT");    \
	mapBtn(T::kButtonId_Right, "Right", "DRIGHT")

	if (emuType == EmulatedController::Type::VPAD)
	{
		using T = VPADController;
		MAP_COMMON(T);
		mapBtn(T::kButtonId_StickL, "StickL", "LCLICK");
		mapBtn(T::kButtonId_StickR, "StickR", "RCLICK");
		emu->set_mapping(T::kButtonId_StickL_Up, physical, kAxisYP);
		emu->set_mapping(T::kButtonId_StickL_Down, physical, kAxisYN);
		emu->set_mapping(T::kButtonId_StickL_Left, physical, kAxisXN);
		emu->set_mapping(T::kButtonId_StickL_Right, physical, kAxisXP);
		emu->set_mapping(T::kButtonId_StickR_Up, physical, kRotationYP);
		emu->set_mapping(T::kButtonId_StickR_Down, physical, kRotationYN);
		emu->set_mapping(T::kButtonId_StickR_Left, physical, kRotationXN);
		emu->set_mapping(T::kButtonId_StickR_Right, physical, kRotationXP);
	}
	else if (emuType == EmulatedController::Type::Pro)
	{
		using T = ProController;
		MAP_COMMON(T);
		mapBtn(T::kButtonId_StickL, "StickL", "LCLICK");
		mapBtn(T::kButtonId_StickR, "StickR", "RCLICK");
		emu->set_mapping(T::kButtonId_StickL_Up, physical, kAxisYP);
		emu->set_mapping(T::kButtonId_StickL_Down, physical, kAxisYN);
		emu->set_mapping(T::kButtonId_StickL_Left, physical, kAxisXN);
		emu->set_mapping(T::kButtonId_StickL_Right, physical, kAxisXP);
		emu->set_mapping(T::kButtonId_StickR_Up, physical, kRotationYP);
		emu->set_mapping(T::kButtonId_StickR_Down, physical, kRotationYN);
		emu->set_mapping(T::kButtonId_StickR_Left, physical, kRotationXN);
		emu->set_mapping(T::kButtonId_StickR_Right, physical, kRotationXP);
	}
	else if (emuType == EmulatedController::Type::Wiimote)
	{
		using T = WiimoteController;
		mapBtn(T::kButtonId_A, "A", "A");
		mapBtn(T::kButtonId_B, "B", "B");
		mapBtn(T::kButtonId_1, "X", "X");
		mapBtn(T::kButtonId_2, "Y", "Y");
		mapBtn(T::kButtonId_Plus, "Plus", "PLUS");
		mapBtn(T::kButtonId_Minus, "Minus", "MINUS");
		mapBtn(T::kButtonId_Up, "Up", "DUP");
		mapBtn(T::kButtonId_Down, "Down", "DDOWN");
		mapBtn(T::kButtonId_Left, "Left", "DLEFT");
		mapBtn(T::kButtonId_Right, "Right", "DRIGHT");
		mapBtn(T::kButtonId_Nunchuck_C, "L", "L");
		mapBtn(T::kButtonId_Nunchuck_Z, "ZL", "ZL");
		emu->set_mapping(T::kButtonId_Nunchuck_Up, physical, kAxisYP);
		emu->set_mapping(T::kButtonId_Nunchuck_Down, physical, kAxisYN);
		emu->set_mapping(T::kButtonId_Nunchuck_Left, physical, kAxisXN);
		emu->set_mapping(T::kButtonId_Nunchuck_Right, physical, kAxisXP);
	}
	else
	{
		using T = ClassicController;
		MAP_COMMON(T);
		emu->set_mapping(T::kButtonId_StickL_Up, physical, kAxisYP);
		emu->set_mapping(T::kButtonId_StickL_Down, physical, kAxisYN);
		emu->set_mapping(T::kButtonId_StickL_Left, physical, kAxisXN);
		emu->set_mapping(T::kButtonId_StickL_Right, physical, kAxisXP);
		emu->set_mapping(T::kButtonId_StickR_Up, physical, kRotationYP);
		emu->set_mapping(T::kButtonId_StickR_Down, physical, kRotationYN);
		emu->set_mapping(T::kButtonId_StickR_Left, physical, kRotationXN);
		emu->set_mapping(T::kButtonId_StickR_Right, physical, kRotationXP);
	}
#undef MAP_COMMON
	return true;
}

void EnsurePhysicalController(size_t index, EmulatedController::Type type)
{
	const bool gamePadFeatures = type == EmulatedController::Type::VPAD;
	const bool pointer = type == EmulatedController::Type::Wiimote;
	const bool motion = gamePadFeatures || pointer;
	if (s_controllers[index] && s_controllers[index]->usesGamePadFeatures() == gamePadFeatures &&
		s_controllers[index]->usesMotion() == motion && s_controllers[index]->usesPointer() == pointer)
		return;
	s_controllers[index].reset();
	s_controllers[index] = std::make_shared<SwitchGamepadController>(
		index, kNpadIds[index], index == 0, gamePadFeatures, motion, pointer);
	const auto recenter = s_config.map.find("Recenter");
	s_controllers[index]->setRecenterButton(physCode(recenter != s_config.map.end() ? recenter->second : "RCLICK"));
	auto settings = s_controllers[index]->get_settings();
	settings.axis.deadzone = settings.rotation.deadzone = s_config.deadzone / 100.0f;
	settings.rumble = s_config.rumble ? 1.0f : 0.0f;
	settings.motion = motion;
	s_controllers[index]->set_settings(settings);
}

bool ConfigureControllers(SwitchControllerType type)
{
	auto& inputManager = InputManager::instance();
	for (size_t i = 0; i < kSwitchControllerCount; ++i)
	{
		if (auto controller = inputManager.get_controller(i))
			controller->stop_rumble();
		inputManager.delete_controller(i);
	}

	const EmulatedController::Type playerOneType = ToEmulatedType(type);
	const size_t requestedPlayers = static_cast<size_t>(s_config.players);
	const size_t maxPlayers = playerOneType == EmulatedController::Type::VPAD
		? kSwitchControllerCount
		: InputManager::kMaxWPADControllers;
	const size_t configuredPlayers = std::min(requestedPlayers, maxPlayers);
	const auto secondaryType = s_secondaryControllerType.load(std::memory_order_acquire);
	const auto secondaryEmulatedType = ToEmulatedType(secondaryType);
	for (size_t i = 0; i < configuredPlayers; ++i)
		EnsurePhysicalController(i, i == 0 ? playerOneType : secondaryEmulatedType);
	for (size_t i = configuredPlayers; i < s_controllers.size(); ++i)
		s_controllers[i].reset();

	for (size_t i = 0; i < configuredPlayers; ++i)
	{
		const auto controllerType = i == 0 ? type : secondaryType;
		const auto emulatedType = i == 0 ? playerOneType : secondaryEmulatedType;
		auto emu = inputManager.set_controller(i, emulatedType);
		if (!ConfigureMappings(emu, emulatedType, s_controllers[i]))
			return false;
		if (emulatedType == EmulatedController::Type::Wiimote)
		{
			std::static_pointer_cast<WiimoteController>(emu)->set_device_type(
				controllerType == SwitchControllerType::WiimoteNunchuk ? kWAPDevMPLSFreeStyle : kWAPDevMPLS);
		}
	}

	s_controllerType.store(type, std::memory_order_release);
	return true;
}
} // namespace

void SwitchInput_Setup()
{
	s_config = loadInputConfig("sdmc:/switch/cemu/input.ini");
	auto secondaryType = ParseControllerType(s_config.secondaryType, SwitchControllerType::Pro);
	if (secondaryType == SwitchControllerType::GamePad)
		secondaryType = SwitchControllerType::Pro;
	s_secondaryControllerType.store(secondaryType, std::memory_order_release);
	ConfigureControllers(ParseControllerType(s_config.type, SwitchControllerType::GamePad));
}

void SwitchInput_Shutdown()
{
	for (auto& controller : s_controllers)
		controller.reset();
}

bool SwitchInput_SetControllerType(SwitchControllerType type)
{
	if (type < SwitchControllerType::GamePad || type >= SwitchControllerType::Count)
		return false;
	if (type == SwitchInput_GetControllerType())
		return true;
	return ConfigureControllers(type);
}

SwitchControllerType SwitchInput_GetControllerType()
{
	return s_controllerType.load(std::memory_order_acquire);
}

bool SwitchInput_SetSecondaryControllerType(SwitchControllerType type)
{
	if (type <= SwitchControllerType::GamePad || type >= SwitchControllerType::Count)
		return false;
	if (type == SwitchInput_GetSecondaryControllerType())
		return true;
	s_secondaryControllerType.store(type, std::memory_order_release);
	return ConfigureControllers(SwitchInput_GetControllerType());
}

SwitchControllerType SwitchInput_GetSecondaryControllerType()
{
	return s_secondaryControllerType.load(std::memory_order_acquire);
}
