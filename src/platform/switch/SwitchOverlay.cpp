#include "platform/switch/SwitchOverlay.h"

#include <switch.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

#include <imgui.h>

#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"
#include "imgui/imgui_extension.h"
#include "platform/switch/SwitchAmiibo.h"
#include "platform/switch/SwitchInput.h"
#include "platform/switch/SwitchLSFG.h"
#if defined(ENABLE_DEKO3D)
#include "platform/switch/SwitchLSFGDeko.h"
#endif
#include "platform/switch/SwitchPlatform.h"
#include "platform/switch/SwitchToys.h"

namespace
{
enum class Page
{
	Main,
	Hotkeys,
	Accessories,
	ToySlots,
	ToyFiles,
};

enum class HotkeyAction : size_t
{
	QuickMenu,
	CycleDisplay,
	FastForward,
	ToggleFrameGeneration,
	ToggleFps,
	CycleController,
	ScanAmiibo,
	ExitLauncher,
	Count,
};

struct HotkeyInfo
{
	const char* label;
	const char* key;
	std::uint64_t defaultMask;
};

constexpr std::uint64_t kDefaultMenuButtons = HidNpadButton_L | HidNpadButton_R | HidNpadButton_Plus;
constexpr std::uint64_t kAllowedButtons = HidNpadButton_A | HidNpadButton_B | HidNpadButton_X |
	HidNpadButton_Y | HidNpadButton_L | HidNpadButton_R | HidNpadButton_ZL | HidNpadButton_ZR |
	HidNpadButton_Plus | HidNpadButton_Minus | HidNpadButton_StickL | HidNpadButton_StickR |
	HidNpadButton_Up | HidNpadButton_Down | HidNpadButton_Left | HidNpadButton_Right;
constexpr size_t kHotkeyCount = static_cast<size_t>(HotkeyAction::Count);
enum MainItem
{
	MainResume,
#ifdef __SWITCH2__
	MainCpuBoost,
#endif
	MainController,
	MainSecondaryController,
	MainGamePad,
	MainFrameGeneration,
	MainFps,
	MainPosition,
	MainHotkeys,
	MainAccessories,
	MainAmiibo,
	MainExit,
	MainItemCount,
};
constexpr float kMainRowHeight = 38.0f;
constexpr int kAccessoryMenuItems = 4;
constexpr int kPositionCount = 7;

constexpr std::array<HotkeyInfo, kHotkeyCount> kHotkeys{{
	{"Open quick menu", "menu", kDefaultMenuButtons},
	{"Cycle TV / Pad / Dual", "display", HidNpadButton_ZL | HidNpadButton_ZR | HidNpadButton_Minus},
	{"Toggle fast-forward", "fast_forward", 0},
	{"Toggle frame generation", "frame_generation", 0},
	{"Toggle FPS counter", "fps", 0},
	{"Cycle controller type", "controller", 0},
	{"Scan Amiibo", "amiibo", 0},
	{"Exit to launcher", "exit", HidNpadButton_L | HidNpadButton_R | HidNpadButton_ZL | HidNpadButton_ZR},
}};

std::atomic_bool s_visible{false};
// The menu is laid out for 1080 lines; the Switch 2 scales it up with its docked output (1440p, 4K). Render thread only.
float s_scale = 1.0f;
std::atomic_bool s_captureUntilRelease{false};
std::atomic<Page> s_page{Page::Main};
std::atomic_int s_selection{0};
std::atomic_int s_fpsToggleRequests{0};
std::atomic_int s_positionRequests{0};
std::atomic_int s_fastForwardToggleRequests{0};
std::atomic_int s_amiiboScanRequests{0};
std::atomic_int s_exitRequests{0};
std::array<std::atomic_uint64_t, kHotkeyCount> s_hotkeyMasks{};
std::atomic_int s_bindingAction{-1};
std::atomic_int s_bindingStage{0};
std::atomic_uint64_t s_bindingMask{0};
std::atomic_bool s_bindingRejected{false};
std::atomic<SwitchToyType> s_toyType{SwitchToyType::Skylanders};
std::atomic_int s_toySlot{0};
fs::path s_hotkeyPath;

size_t HotkeyIndex(HotkeyAction action)
{
	return static_cast<size_t>(action);
}

const char* ControllerTypeName(SwitchControllerType type)
{
	switch (type)
	{
	case SwitchControllerType::GamePad:
		return "Wii U GamePad";
	case SwitchControllerType::Pro:
		return "Wii U Pro Controller";
	case SwitchControllerType::Classic:
		return "Wii Classic Controller";
	case SwitchControllerType::Wiimote:
		return "Wii Remote + MotionPlus";
	case SwitchControllerType::WiimoteNunchuk:
		return "Wii Remote + MotionPlus + Nunchuk";
	case SwitchControllerType::Count:
		break;
	}
	return "Wii U GamePad";
}

void CycleControllerType(int direction)
{
	int type = static_cast<int>(SwitchInput_GetControllerType());
	type = (type + direction + static_cast<int>(SwitchControllerType::Count)) % static_cast<int>(SwitchControllerType::Count);
	SwitchInput_SetControllerType(static_cast<SwitchControllerType>(type));
}

void CycleSecondaryControllerType(int direction)
{
	constexpr int count = static_cast<int>(SwitchControllerType::Count);
	int type = static_cast<int>(SwitchInput_GetSecondaryControllerType());
	do
		type = (type + direction + count) % count;
	while (static_cast<SwitchControllerType>(type) == SwitchControllerType::GamePad);
	SwitchInput_SetSecondaryControllerType(static_cast<SwitchControllerType>(type));
}

const char* GamePadLayoutName(SwitchGamePadLayout layout)
{
	switch (layout)
	{
	case SwitchGamePadLayout::Disabled:
		return "TV only";
	case SwitchGamePadLayout::PadOnly:
		return "Pad only";
	case SwitchGamePadLayout::Right:
		return "Pad right";
	case SwitchGamePadLayout::Left:
		return "Pad left";
	case SwitchGamePadLayout::Below:
		return "Pad below";
	case SwitchGamePadLayout::Above:
		return "Pad above";
	}
	return "TV only";
}

void ChangeGamePadLayout(int direction)
{
	constexpr int layoutCount = 6;
	int layout = static_cast<int>(SwitchPlatform_GetGamePadLayout());
	layout = (layout + direction + layoutCount) % layoutCount;
	SwitchPlatform_SetGamePadLayout(static_cast<SwitchGamePadLayout>(layout));
}

const char* PositionName(ScreenPosition position)
{
	switch (position)
	{
	case ScreenPosition::kDisabled:
		return "Disabled";
	case ScreenPosition::kTopLeft:
		return "Top left";
	case ScreenPosition::kTopCenter:
		return "Top center";
	case ScreenPosition::kTopRight:
		return "Top right";
	case ScreenPosition::kBottomLeft:
		return "Bottom left";
	case ScreenPosition::kBottomCenter:
		return "Bottom center";
	case ScreenPosition::kBottomRight:
		return "Bottom right";
	}
	return "Disabled";
}

const char* ToyTypeName(SwitchToyType type)
{
	switch (type)
	{
	case SwitchToyType::Skylanders:
		return "Skylanders Portal";
	case SwitchToyType::Infinity:
		return "Disney Infinity Base";
	case SwitchToyType::Dimensions:
		return "LEGO Dimensions Toypad";
	}
	return "USB accessory";
}

std::string Shorten(std::string value, size_t limit)
{
	if (value.size() <= limit)
		return value;
	value.resize(limit - 3);
	value += "...";
	return value;
}

std::string HotkeyMaskName(std::uint64_t mask)
{
	if (mask == 0)
		return "Disabled";
	struct ButtonName
	{
		std::uint64_t button;
		const char* name;
	};
	constexpr std::array<ButtonName, 16> names{{
		{HidNpadButton_L, "L"}, {HidNpadButton_R, "R"}, {HidNpadButton_ZL, "ZL"},
		{HidNpadButton_ZR, "ZR"}, {HidNpadButton_Plus, "+"}, {HidNpadButton_Minus, "-"},
		{HidNpadButton_A, "A"}, {HidNpadButton_B, "B"}, {HidNpadButton_X, "X"},
		{HidNpadButton_Y, "Y"}, {HidNpadButton_StickL, "L3"}, {HidNpadButton_StickR, "R3"},
		{HidNpadButton_Up, "Up"}, {HidNpadButton_Down, "Down"},
		{HidNpadButton_Left, "Left"}, {HidNpadButton_Right, "Right"},
	}};
	std::string result;
	for (const auto& entry : names)
	{
		if (!(mask & entry.button))
			continue;
		if (!result.empty())
			result += " + ";
		result += entry.name;
	}
	return result.empty() ? "Disabled" : result;
}

void SaveHotkeys()
{
	if (s_hotkeyPath.empty())
		return;
	const fs::path temporary = s_hotkeyPath.string() + ".tmp";
	std::ofstream output(temporary, std::ios::trunc);
	if (!output)
		return;
	for (size_t index = 0; index < kHotkeyCount; ++index)
		output << kHotkeys[index].key << '=' << std::hex << s_hotkeyMasks[index].load(std::memory_order_acquire) << '\n';
	output.close();
	if (!output)
		return;
	std::error_code error;
	fs::remove(s_hotkeyPath, error);
	error.clear();
	fs::rename(temporary, s_hotkeyPath, error);
}

void ResetHotkeys()
{
	for (size_t index = 0; index < kHotkeyCount; ++index)
		s_hotkeyMasks[index].store(kHotkeys[index].defaultMask, std::memory_order_release);
	SaveHotkeys();
}

bool SetHotkey(int action, std::uint64_t mask)
{
	if (action < 0 || action >= static_cast<int>(kHotkeyCount))
		return false;
	mask &= kAllowedButtons;
	if (action == static_cast<int>(HotkeyAction::QuickMenu) && mask == 0)
		mask = kDefaultMenuButtons;
	const std::uint64_t menuMask = s_hotkeyMasks[HotkeyIndex(HotkeyAction::QuickMenu)].load(std::memory_order_acquire);
	if (action != static_cast<int>(HotkeyAction::QuickMenu) && mask != 0 && mask == menuMask)
		return false;
	if (mask != 0)
	{
		for (size_t index = 0; index < kHotkeyCount; ++index)
			if (static_cast<int>(index) != action && s_hotkeyMasks[index].load(std::memory_order_acquire) == mask)
				s_hotkeyMasks[index].store(0, std::memory_order_release);
	}
	s_hotkeyMasks[action].store(mask, std::memory_order_release);
	SaveHotkeys();
	return true;
}

bool HotkeyPressed(HotkeyAction action, std::uint64_t down, std::uint64_t held)
{
	const std::uint64_t mask = s_hotkeyMasks[HotkeyIndex(action)].load(std::memory_order_acquire);
	return mask != 0 && (held & mask) == mask && (down & mask) != 0;
}

void SetPage(Page page)
{
	s_page.store(page, std::memory_order_release);
	s_selection.store(0, std::memory_order_release);
}

void CloseMenu()
{
	s_visible.store(false, std::memory_order_release);
	s_captureUntilRelease.store(true, std::memory_order_release);
}

void ToggleMenu()
{
	const bool visible = !s_visible.load(std::memory_order_relaxed);
	s_visible.store(visible, std::memory_order_release);
	if (visible)
		SetPage(Page::Main);
	else
		s_captureUntilRelease.store(true, std::memory_order_release);
}

void DispatchHotkey(HotkeyAction action)
{
	switch (action)
	{
	case HotkeyAction::QuickMenu:
		ToggleMenu();
		return;
	case HotkeyAction::CycleDisplay:
		SwitchPlatform_CycleGamePadMode();
		break;
	case HotkeyAction::FastForward:
		s_fastForwardToggleRequests.fetch_add(1, std::memory_order_release);
		break;
	case HotkeyAction::ToggleFrameGeneration:
		SwitchLSFG_RequestEnabled(!SwitchLSFG_IsEnabled());
		break;
	case HotkeyAction::ToggleFps:
		s_fpsToggleRequests.fetch_add(1, std::memory_order_release);
		break;
	case HotkeyAction::CycleController:
		CycleControllerType(1);
		break;
	case HotkeyAction::ScanAmiibo:
		s_amiiboScanRequests.fetch_add(1, std::memory_order_release);
		break;
	case HotkeyAction::ExitLauncher:
		s_exitRequests.fetch_add(1, std::memory_order_release);
		break;
	case HotkeyAction::Count:
		return;
	}
	s_captureUntilRelease.store(true, std::memory_order_release);
}

void BeginBinding(int action)
{
	s_bindingRejected.store(false, std::memory_order_release);
	s_bindingMask.store(0, std::memory_order_release);
	s_bindingStage.store(1, std::memory_order_release);
	s_bindingAction.store(action, std::memory_order_release);
}

void UpdateBinding(std::uint64_t held)
{
	const int action = s_bindingAction.load(std::memory_order_acquire);
	if (action < 0)
		return;
	const std::uint64_t buttons = held & kAllowedButtons;
	const int stage = s_bindingStage.load(std::memory_order_relaxed);
	if (stage == 1)
	{
		if (buttons == 0)
			s_bindingStage.store(2, std::memory_order_release);
		return;
	}
	if (stage == 2)
	{
		if (buttons != 0)
		{
			s_bindingMask.store(buttons, std::memory_order_release);
			s_bindingStage.store(3, std::memory_order_release);
		}
		return;
	}
	if (buttons != 0)
	{
		s_bindingMask.fetch_or(buttons, std::memory_order_acq_rel);
		return;
	}
	const bool accepted = SetHotkey(action, s_bindingMask.load(std::memory_order_acquire));
	s_bindingRejected.store(!accepted, std::memory_order_release);
	s_bindingAction.store(-1, std::memory_order_release);
	s_bindingStage.store(0, std::memory_order_release);
	s_captureUntilRelease.store(true, std::memory_order_release);
}

void ApplyOverlaySettings()
{
	auto& config = GetConfig();
	if (s_fpsToggleRequests.exchange(0, std::memory_order_acq_rel) & 1)
	{
		config.overlay.fps = !config.overlay.fps;
		if (config.overlay.fps && config.overlay.position == ScreenPosition::kDisabled)
			config.overlay.position = ScreenPosition::kTopLeft;
	}
	const int positionDelta = s_positionRequests.exchange(0, std::memory_order_acq_rel);
	if (positionDelta != 0)
	{
		const int position = (static_cast<int>(config.overlay.position) +
			positionDelta % kPositionCount + kPositionCount) % kPositionCount;
		config.overlay.position = static_cast<ScreenPosition>(position);
	}
	if (s_fastForwardToggleRequests.exchange(0, std::memory_order_acq_rel) & 1)
		ActiveSettings::SetTimerShiftFactor(ActiveSettings::GetTimerShiftFactor() < 3 ? 3 : 1);
}

void CenteredText(const char* text)
{
	const float width = ImGui::CalcTextSize(text).x;
	ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), (ImGui::GetWindowWidth() - width) * 0.5f));
	ImGui::TextUnformatted(text);
}

void SelectableRow(const std::string& label, int index, float height = 48.0f)
{
	const bool selected = s_selection.load(std::memory_order_relaxed) == index;
	ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_None, {0.0f, height * s_scale});
	if (selected && (s_page.load(std::memory_order_relaxed) == Page::ToySlots ||
		s_page.load(std::memory_order_relaxed) == Page::ToyFiles))
		ImGui::SetScrollHereY(0.5f);
}

void OptionRow(const char* label, int index, std::string_view value = {},
	std::optional<bool> toggle = std::nullopt, float height = kMainRowHeight)
{
	SelectableRow(label, index, height);
	const float center = (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f;
	float right = ImGui::GetItemRectMax().x - 12.0f * s_scale;
	ImDrawList* draw = ImGui::GetWindowDrawList();
	if (toggle)
	{
		const float trackHeight = 22.0f * s_scale;
		const float width = trackHeight * 9.0f / 5.0f;
		const float inset = 3.0f * s_scale;
		const float radius = trackHeight * 0.5f - inset;
		draw->AddRectFilled({right - width, center - trackHeight * 0.5f}, {right, center + trackHeight * 0.5f},
			*toggle ? IM_COL32(75, 155, 222, 255) : IM_COL32(58, 68, 82, 255), trackHeight * 0.5f);
		const float knob = *toggle ? right - inset - radius : right - width + inset + radius;
		draw->AddCircleFilled({knob, center}, radius, IM_COL32(245, 250, 255, 255));
		right -= width + 12.0f * s_scale;
	}
	if (!value.empty())
	{
		const char* end = value.data() + value.size();
		const ImVec2 size = ImGui::CalcTextSize(value.data(), end);
		const float left = ImGui::GetItemRectMin().x + ImGui::CalcTextSize(label, nullptr, true).x + 16.0f * s_scale;
		if (left >= right)
			return;
		draw->PushClipRect({left, ImGui::GetItemRectMin().y}, {right, ImGui::GetItemRectMax().y}, true);
		draw->AddText({std::max(left, right - size.x), center - size.y * 0.5f},
			IM_COL32(177, 205, 225, 255), value.data(), end);
		draw->PopClipRect();
	}
}

void RenderMainPage()
{
	const auto& config = GetConfig();
	OptionRow("Resume Game", MainResume);
#ifdef __SWITCH2__
	OptionRow("CPU Boost", MainCpuBoost, {}, SwitchPlatform_IsCpuBoostEnabled());
#endif
	OptionRow("Controller type", MainController, ControllerTypeName(SwitchInput_GetControllerType()));
	OptionRow("Other controllers", MainSecondaryController, ControllerTypeName(SwitchInput_GetSecondaryControllerType()));
	OptionRow("GamePad screen", MainGamePad, GamePadLayoutName(SwitchPlatform_GetGamePadLayout()));
	const bool frameGenerationPrepared = SwitchLSFG_IsPrepared();
	bool frameGenerationAvailable = frameGenerationPrepared && SwitchLSFG_IsAvailable();
	const bool frameGenerationEnabled = SwitchLSFG_IsEnabled();
	const char* frameGenerationStatus = frameGenerationAvailable ?
		(frameGenerationEnabled && SwitchLSFG_IsHighRatePassthrough() ? "Native rate" : "") :
		(frameGenerationPrepared ? "Unavailable" : "Not prepared");
#if defined(ENABLE_DEKO3D)
	if (SwitchLSFGDeko_IsPreparing())
	{
		frameGenerationAvailable = false;
		frameGenerationStatus = "Preparing";
	}
#endif
	OptionRow("Frame generation", MainFrameGeneration, frameGenerationStatus,
		frameGenerationAvailable ? std::optional{frameGenerationEnabled} : std::nullopt);
	OptionRow("FPS counter", MainFps, {}, config.overlay.fps);
	OptionRow("Overlay position", MainPosition, PositionName(config.overlay.position));
	OptionRow("Runtime hotkeys", MainHotkeys, ">");
	OptionRow("Virtual USB accessories", MainAccessories, ">");
	char amiiboStatus[96];
	SwitchAmiibo_GetStatus(amiiboStatus, sizeof(amiiboStatus));
	OptionRow("Scan Amiibo", MainAmiibo, amiiboStatus);
	OptionRow("Exit to launcher", MainExit);
}

void RenderHotkeyPage()
{
	for (size_t index = 0; index < kHotkeyCount; ++index)
	{
		std::string binding;
		if (s_bindingAction.load(std::memory_order_acquire) == static_cast<int>(index))
		{
			const int stage = s_bindingStage.load(std::memory_order_acquire);
			binding = stage <= 1 ? "Release all buttons" :
				(stage == 2 ? "Hold new combination" : HotkeyMaskName(s_bindingMask.load(std::memory_order_acquire)));
		}
		else
			binding = HotkeyMaskName(s_hotkeyMasks[index].load(std::memory_order_acquire));
		OptionRow(kHotkeys[index].label, static_cast<int>(index), binding);
	}
	OptionRow("Reset hotkeys to defaults", static_cast<int>(kHotkeyCount));
	OptionRow("Back", static_cast<int>(kHotkeyCount + 1));
	if (s_bindingRejected.load(std::memory_order_acquire))
	{
		ImGui::Separator();
		CenteredText("That combination is reserved for the quick menu");
	}
}

void RenderAccessoryPage()
{
	for (int index = 0; index < 3; ++index)
	{
		const auto type = static_cast<SwitchToyType>(index);
		OptionRow(ToyTypeName(type), index, SwitchToys_IsEnabled(type) ? "Enabled" : "Disabled");
	}
	OptionRow("Back", kAccessoryMenuItems - 1);
	ImGui::Separator();
	CenteredText("Enable devices in Launcher > Settings > USB Accessories");
}

void RenderToySlotsPage()
{
	const SwitchToyType type = s_toyType.load(std::memory_order_acquire);
	const int slotCount = SwitchToys_GetSlotCount(type);
	ImGui::BeginChild("toy slots", {0.0f, 500.0f * s_scale}, false, ImGuiWindowFlags_NoInputs);
	for (int slot = 0; slot < slotCount; ++slot)
	{
		const std::string label = SwitchToys_GetSlotTitle(type, slot) + "##slot" + std::to_string(slot);
		OptionRow(label.c_str(), slot, SwitchToys_GetSlotValue(type, slot), {}, 48.0f);
	}
	SelectableRow("Back", slotCount);
	ImGui::EndChild();
	ImGui::Separator();
	const std::string status = Shorten(SwitchToys_GetStatus(), 72);
	CenteredText(status.c_str());
}

void RenderToyFilesPage()
{
	const SwitchToyType type = s_toyType.load(std::memory_order_acquire);
	const int fileCount = SwitchToys_GetFileCount(type);
	ImGui::BeginChild("toy files", {0.0f, 500.0f * s_scale}, false, ImGuiWindowFlags_NoInputs);
	for (int index = 0; index < fileCount; ++index)
	{
		std::string row = Shorten(SwitchToys_GetFileName(type, index), 64) + "##file" + std::to_string(index);
		SelectableRow(row, index);
	}
	SelectableRow("Back", fileCount);
	ImGui::EndChild();
	ImGui::Separator();
	const std::string status = Shorten(SwitchToys_GetStatus(), 72);
	CenteredText(status.c_str());
	if (fileCount == 0)
		CenteredText("No compatible dumps found. Add files to sdmc:/switch/cemu/toys/");
}
} // namespace

void SwitchOverlay_Initialize()
{
	for (size_t index = 0; index < kHotkeyCount; ++index)
		s_hotkeyMasks[index].store(kHotkeys[index].defaultMask, std::memory_order_relaxed);
	s_hotkeyPath = ActiveSettings::GetUserDataPath("switch_hotkeys.ini");
	std::ifstream input(s_hotkeyPath);
	std::string line;
	while (std::getline(input, line))
	{
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		const size_t equals = line.find('=');
		if (equals == std::string::npos)
			continue;
		const std::string_view key{line.data(), equals};
		std::uint64_t mask = 0;
		const std::string_view value{line.data() + equals + 1, line.size() - equals - 1};
		auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), mask, 16);
		if (error != std::errc{} || end != value.data() + value.size())
			continue;
		for (size_t index = 0; index < kHotkeyCount; ++index)
			if (key == kHotkeys[index].key)
				s_hotkeyMasks[index].store(mask & kAllowedButtons, std::memory_order_relaxed);
	}
	if (s_hotkeyMasks[HotkeyIndex(HotkeyAction::QuickMenu)].load(std::memory_order_relaxed) == 0)
		s_hotkeyMasks[HotkeyIndex(HotkeyAction::QuickMenu)].store(kDefaultMenuButtons, std::memory_order_relaxed);
}

void SwitchOverlay_UpdateInput(std::uint64_t down, std::uint64_t held)
{
	if (s_bindingAction.load(std::memory_order_acquire) >= 0)
	{
		UpdateBinding(held);
		return;
	}
	if (s_captureUntilRelease.load(std::memory_order_relaxed))
	{
		if (held != 0)
			return;
		s_captureUntilRelease.store(false, std::memory_order_release);
	}
	if (HotkeyPressed(HotkeyAction::QuickMenu, down, held))
	{
		DispatchHotkey(HotkeyAction::QuickMenu);
		return;
	}
	if (!s_visible.load(std::memory_order_acquire))
	{
		for (size_t index = 1; index < kHotkeyCount; ++index)
		{
			const auto action = static_cast<HotkeyAction>(index);
			if (HotkeyPressed(action, down, held))
			{
				DispatchHotkey(action);
				break;
			}
		}
		return;
	}

	const Page page = s_page.load(std::memory_order_acquire);
	int selection = s_selection.load(std::memory_order_relaxed);
	int itemCount = MainItemCount;
	if (page == Page::Hotkeys)
		itemCount = static_cast<int>(kHotkeyCount + 2);
	else if (page == Page::Accessories)
		itemCount = kAccessoryMenuItems;
	else if (page == Page::ToySlots)
		itemCount = SwitchToys_GetSlotCount(s_toyType.load(std::memory_order_acquire)) + 1;
	else if (page == Page::ToyFiles)
		itemCount = SwitchToys_GetFileCount(s_toyType.load(std::memory_order_acquire)) + 1;

	if (down & HidNpadButton_Up)
		selection = (selection + itemCount - 1) % itemCount;
	else if (down & HidNpadButton_Down)
		selection = (selection + 1) % itemCount;
	s_selection.store(selection, std::memory_order_relaxed);

	if (down & HidNpadButton_B)
	{
		if (page == Page::Main)
			CloseMenu();
		else if (page == Page::ToyFiles)
			SetPage(Page::ToySlots);
		else
			SetPage(Page::Main);
		return;
	}

	const bool previous = down & HidNpadButton_Left;
	const bool next = down & HidNpadButton_Right;
	const bool confirm = down & HidNpadButton_A;
	if (page == Page::Main)
	{
		const bool adjust = previous || next || confirm;
		const int direction = previous ? -1 : 1;
		if (selection == MainResume && confirm)
			CloseMenu();
#ifdef __SWITCH2__
		else if (selection == MainCpuBoost && adjust)
			SwitchPlatform_ToggleCpuBoost();
#endif
		else if (selection == MainController && adjust)
			CycleControllerType(direction);
		else if (selection == MainSecondaryController && adjust)
			CycleSecondaryControllerType(direction);
		else if (selection == MainGamePad && adjust)
			ChangeGamePadLayout(direction);
		else if (selection == MainFrameGeneration && adjust)
			SwitchLSFG_RequestEnabled(!SwitchLSFG_IsEnabled());
		else if (selection == MainFps && adjust)
			s_fpsToggleRequests.fetch_add(1, std::memory_order_release);
		else if (selection == MainPosition && adjust)
			s_positionRequests.fetch_add(direction, std::memory_order_release);
		else if (selection == MainHotkeys && confirm)
			SetPage(Page::Hotkeys);
		else if (selection == MainAccessories && confirm)
			SetPage(Page::Accessories);
		else if (selection == MainAmiibo && confirm)
			s_amiiboScanRequests.fetch_add(1, std::memory_order_release);
		else if (selection == MainExit && confirm)
		{
			s_exitRequests.fetch_add(1, std::memory_order_release);
			CloseMenu();
		}
		return;
	}
	if (page == Page::Hotkeys)
	{
		if (selection < static_cast<int>(kHotkeyCount) && confirm)
			BeginBinding(selection);
		else if (selection < static_cast<int>(kHotkeyCount) && (down & HidNpadButton_X))
			SetHotkey(selection, 0);
		else if (selection == static_cast<int>(kHotkeyCount) && confirm)
		{
			ResetHotkeys();
			s_bindingRejected.store(false, std::memory_order_release);
		}
		else if (selection == static_cast<int>(kHotkeyCount + 1) && confirm)
			SetPage(Page::Main);
		return;
	}
	if (page == Page::Accessories)
	{
		if (selection < 3 && confirm)
		{
			const auto type = static_cast<SwitchToyType>(selection);
			s_toyType.store(type, std::memory_order_release);
			SwitchToys_RefreshFiles(type);
			SetPage(Page::ToySlots);
		}
		else if (selection == 3 && confirm)
			SetPage(Page::Main);
		return;
	}
	const SwitchToyType type = s_toyType.load(std::memory_order_acquire);
	if (page == Page::ToySlots)
	{
		const int slotCount = SwitchToys_GetSlotCount(type);
		if (selection < slotCount && confirm)
		{
			s_toySlot.store(selection, std::memory_order_release);
			SetPage(Page::ToyFiles);
		}
		else if (selection < slotCount && (down & HidNpadButton_X))
			SwitchToys_ClearSlot(type, selection);
		else if (down & HidNpadButton_Y)
			SwitchToys_RefreshFiles(type);
		else if (selection == slotCount && confirm)
			SetPage(Page::Accessories);
		return;
	}
	if (page == Page::ToyFiles)
	{
		const int fileCount = SwitchToys_GetFileCount(type);
		if (selection < fileCount && confirm)
		{
			SwitchToys_LoadFile(type, s_toySlot.load(std::memory_order_acquire), selection);
			SetPage(Page::ToySlots);
		}
		else if (down & HidNpadButton_Y)
		{
			SwitchToys_RefreshFiles(type);
			s_selection.store(0, std::memory_order_release);
		}
		else if (selection == fileCount && confirm)
			SetPage(Page::ToySlots);
	}
}

bool SwitchOverlay_TakeAmiiboScanRequest()
{
	return s_amiiboScanRequests.exchange(0, std::memory_order_acq_rel) != 0;
}

bool SwitchOverlay_TakeExitRequest()
{
	return s_exitRequests.exchange(0, std::memory_order_acq_rel) != 0;
}

bool SwitchOverlay_IsInputCaptured(std::uint64_t held)
{
	if (s_visible.load(std::memory_order_acquire) || s_captureUntilRelease.load(std::memory_order_acquire) ||
		s_bindingAction.load(std::memory_order_acquire) >= 0)
		return true;
	for (size_t index = 0; index < kHotkeyCount; ++index)
	{
		const std::uint64_t mask = s_hotkeyMasks[index].load(std::memory_order_acquire);
		if (mask != 0 && (held & mask) == mask)
			return true;
	}
	return false;
}

void SwitchOverlay_Render()
{
	ApplyOverlaySettings();
	if (!s_visible.load(std::memory_order_acquire))
		return;

	const ImGuiIO& io = ImGui::GetIO();
#if defined(__SWITCH2__)
	s_scale = std::max(1.0f, io.DisplaySize.y / 1080.0f);
#endif
	ImFont* font = ImGui_GetFont(24.0f * s_scale);
	if (!font)
		return;
	ImGui::GetBackgroundDrawList()->AddRectFilled({0.0f, 0.0f}, io.DisplaySize, IM_COL32(0, 0, 0, 120));
	ImGui::SetNextWindowPos({io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f}, ImGuiCond_Always, {0.5f, 0.5f});
	const Page page = s_page.load(std::memory_order_acquire);
	const float rowStep = kMainRowHeight + 8.0f;
	float width = 740.0f;
	float height = 635.0f;
	switch (page)
	{
	case Page::Main:
		width = 570.0f;
		height = 76.0f + MainItemCount * rowStep;
		break;
	case Page::Hotkeys:
		width = 650.0f;
		height = 76.0f + static_cast<float>(kHotkeyCount + 2) * rowStep;
		if (s_bindingRejected.load(std::memory_order_acquire))
			height += 44.0f;
		break;
	case Page::Accessories:
		width = 690.0f;
		height = 120.0f + kAccessoryMenuItems * rowStep;
		break;
	default:
		break;
	}
	ImGui::SetNextWindowSize({width * s_scale, height * s_scale}, ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(0.94f);

	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14.0f * s_scale);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {28.0f * s_scale, 20.0f * s_scale});
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {10.0f * s_scale, 8.0f * s_scale});
	ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, {0.0f, 0.5f});
	ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(12, 20, 34, 245));
	ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(94, 201, 255, 255));
	ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(31, 82, 112, 235));
	ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(31, 82, 112, 235));
	ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(31, 82, 112, 235));
	ImGui::PushFont(font);

	constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
		ImGuiWindowFlags_NoInputs;
	if (ImGui::Begin("Cemu quick menu", nullptr, flags))
	{
		if (page == Page::Main)
			CenteredText("Cemu Quick Menu");
		else if (page == Page::Hotkeys)
			CenteredText("Runtime Hotkeys");
		else if (page == Page::Accessories)
			CenteredText("Virtual USB Accessories");
		else if (page == Page::ToySlots)
			CenteredText(ToyTypeName(s_toyType.load(std::memory_order_acquire)));
		else
		{
			const std::string title = "Load into " + SwitchToys_GetSlotTitle(
				s_toyType.load(std::memory_order_acquire), s_toySlot.load(std::memory_order_acquire));
			CenteredText(title.c_str());
		}
		ImGui::Separator();
		if (page == Page::Main)
			RenderMainPage();
		else if (page == Page::Hotkeys)
			RenderHotkeyPage();
		else if (page == Page::Accessories)
			RenderAccessoryPage();
		else if (page == Page::ToySlots)
			RenderToySlotsPage();
		else
			RenderToyFilesPage();
	}
	ImGui::End();
	ImGui::PopFont();
	ImGui::PopStyleColor(5);
	ImGui::PopStyleVar(4);
}
