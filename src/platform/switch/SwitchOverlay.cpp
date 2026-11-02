#include "platform/switch/SwitchOverlay.h"

#include <switch.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstdio>
#include <fstream>
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
	ToggleShaderNotice,
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
constexpr int kMainMenuItems = 11;
constexpr int kAccessoryMenuItems = 4;
constexpr int kPositionCount = 7;

constexpr std::array<HotkeyInfo, kHotkeyCount> kHotkeys{{
	{"Open quick menu", "menu", kDefaultMenuButtons},
	{"Cycle TV / Pad / Dual", "display", HidNpadButton_ZL | HidNpadButton_ZR | HidNpadButton_Minus},
	{"Toggle fast-forward", "fast_forward", 0},
	{"Toggle frame generation", "frame_generation", 0},
	{"Toggle FPS counter", "fps", 0},
	{"Toggle shader notice", "shader_notice", 0},
	{"Cycle controller type", "controller", 0},
	{"Scan Amiibo", "amiibo", 0},
	{"Exit to launcher", "exit", HidNpadButton_L | HidNpadButton_R | HidNpadButton_ZL | HidNpadButton_ZR},
}};

std::atomic_bool s_visible{false};
std::atomic_bool s_captureUntilRelease{false};
std::atomic<Page> s_page{Page::Main};
std::atomic_int s_selection{0};
std::atomic_int s_fpsToggleRequests{0};
std::atomic_int s_positionRequests{0};
std::atomic_int s_shaderToggleRequests{0};
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
	case HotkeyAction::ToggleShaderNotice:
		s_shaderToggleRequests.fetch_add(1, std::memory_order_release);
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
	if (s_shaderToggleRequests.exchange(0, std::memory_order_acq_rel) & 1)
		config.notification.shader_compiling = !config.notification.shader_compiling;
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
	ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_None, {0.0f, height});
	if (selected && (s_page.load(std::memory_order_relaxed) == Page::ToySlots ||
		s_page.load(std::memory_order_relaxed) == Page::ToyFiles))
		ImGui::SetScrollHereY(0.5f);
}

void RenderMainPage()
{
	auto& config = GetConfig();
	char row[192];
	std::snprintf(row, sizeof(row), "Controller type     <  %s  >", ControllerTypeName(SwitchInput_GetControllerType()));
	SelectableRow(row, 0);
	std::snprintf(row, sizeof(row), "GamePad screen      <  %s  >", GamePadLayoutName(SwitchPlatform_GetGamePadLayout()));
	SelectableRow(row, 1);
	const char* frameGenerationStatus = !SwitchLSFG_IsPrepared() ? "Not prepared" :
		(!SwitchLSFG_IsAvailable() ? "Unavailable" :
		(!SwitchLSFG_IsEnabled() ? "Off" :
		(SwitchLSFG_IsHighRatePassthrough() ? "On (native rate)" : "On")));
#if defined(ENABLE_DEKO3D)
	if (SwitchLSFGDeko_IsPreparing())
		frameGenerationStatus = "Preparing";
#endif
	std::snprintf(row, sizeof(row), "Frame generation    <  %s  >", frameGenerationStatus);
	SelectableRow(row, 2);
	std::snprintf(row, sizeof(row), "FPS counter         <  %s  >", config.overlay.fps ? "On" : "Off");
	SelectableRow(row, 3);
	std::snprintf(row, sizeof(row), "Overlay position    <  %s  >", PositionName(config.overlay.position));
	SelectableRow(row, 4);
	std::snprintf(row, sizeof(row), "Shader notice       <  %s  >", config.notification.shader_compiling ? "On" : "Off");
	SelectableRow(row, 5);
	SelectableRow("Runtime hotkeys                              >", 6);
	SelectableRow("Virtual USB accessories                      >", 7);
	char amiiboStatus[96];
	SwitchAmiibo_GetStatus(amiiboStatus, sizeof(amiiboStatus));
	std::snprintf(row, sizeof(row), "Scan Amiibo         %s", amiiboStatus);
	SelectableRow(row, 8);
	SelectableRow("Exit to launcher", 9);
	SelectableRow("Close quick menu", 10);
	ImGui::Separator();
	CenteredText("D-Pad  Navigate     A  Select     B  Close");
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
		std::string row = std::string(kHotkeys[index].label) + "    <  " + binding + "  >##hotkey" + std::to_string(index);
		SelectableRow(row, static_cast<int>(index));
	}
	SelectableRow("Reset hotkeys to defaults", static_cast<int>(kHotkeyCount));
	SelectableRow("Back", static_cast<int>(kHotkeyCount + 1));
	ImGui::Separator();
	if (s_bindingRejected.load(std::memory_order_acquire))
		CenteredText("That combination is reserved for the quick menu");
	else
		CenteredText("A  Remap     X  Disable     B  Back");
}

void RenderAccessoryPage()
{
	for (int index = 0; index < 3; ++index)
	{
		const auto type = static_cast<SwitchToyType>(index);
		std::string row = std::string(ToyTypeName(type)) + "    <  " +
			(SwitchToys_IsEnabled(type) ? "Enabled" : "Disabled") + "  >##accessory" + std::to_string(index);
		SelectableRow(row, index, 64.0f);
	}
	SelectableRow("Back", 3, 64.0f);
	ImGui::Separator();
	CenteredText("Enable devices in Launcher > Settings > USB Accessories");
}

void RenderToySlotsPage()
{
	const SwitchToyType type = s_toyType.load(std::memory_order_acquire);
	const int slotCount = SwitchToys_GetSlotCount(type);
	ImGui::BeginChild("toy slots", {0.0f, 500.0f}, false, ImGuiWindowFlags_NoInputs);
	for (int slot = 0; slot < slotCount; ++slot)
	{
		std::string row = SwitchToys_GetSlotTitle(type, slot) + "    <  " +
			Shorten(SwitchToys_GetSlotValue(type, slot), 46) + "  >##slot" + std::to_string(slot);
		SelectableRow(row, slot);
	}
	SelectableRow("Back", slotCount);
	ImGui::EndChild();
	ImGui::Separator();
	const std::string status = Shorten(SwitchToys_GetStatus(), 72);
	CenteredText(status.c_str());
	CenteredText("A  Choose dump     X  Remove     Y  Refresh     B  Back");
}

void RenderToyFilesPage()
{
	const SwitchToyType type = s_toyType.load(std::memory_order_acquire);
	const int fileCount = SwitchToys_GetFileCount(type);
	ImGui::BeginChild("toy files", {0.0f, 500.0f}, false, ImGuiWindowFlags_NoInputs);
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
	else
		CenteredText("A  Load figure     Y  Refresh     B  Back");
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
	int itemCount = kMainMenuItems;
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
		if (selection == 0 && previous)
			CycleControllerType(-1);
		else if (selection == 0 && (next || confirm))
			CycleControllerType(1);
		else if (selection == 1 && previous)
			ChangeGamePadLayout(-1);
		else if (selection == 1 && (next || confirm))
			ChangeGamePadLayout(1);
		else if (selection == 2 && (previous || next || confirm))
			SwitchLSFG_RequestEnabled(!SwitchLSFG_IsEnabled());
		else if (selection == 3 && (previous || next || confirm))
			s_fpsToggleRequests.fetch_add(1, std::memory_order_release);
		else if (selection == 4 && previous)
			s_positionRequests.fetch_sub(1, std::memory_order_release);
		else if (selection == 4 && (next || confirm))
			s_positionRequests.fetch_add(1, std::memory_order_release);
		else if (selection == 5 && (previous || next || confirm))
			s_shaderToggleRequests.fetch_add(1, std::memory_order_release);
		else if (selection == 6 && confirm)
			SetPage(Page::Hotkeys);
		else if (selection == 7 && confirm)
			SetPage(Page::Accessories);
		else if (selection == 8 && confirm)
			s_amiiboScanRequests.fetch_add(1, std::memory_order_release);
		else if (selection == 9 && confirm)
		{
			s_exitRequests.fetch_add(1, std::memory_order_release);
			CloseMenu();
		}
		else if (selection == 10 && confirm)
			CloseMenu();
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

	ImFont* font = ImGui_GetFont(24.0f);
	if (!font)
		return;
	const ImGuiIO& io = ImGui::GetIO();
	ImGui::GetBackgroundDrawList()->AddRectFilled({0.0f, 0.0f}, io.DisplaySize, IM_COL32(0, 0, 0, 120));
	ImGui::SetNextWindowPos({io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f}, ImGuiCond_Always, {0.5f, 0.5f});
	ImGui::SetNextWindowSize({840.0f, 680.0f}, ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(0.94f);

	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {28.0f, 20.0f});
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {10.0f, 8.0f});
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
		const Page page = s_page.load(std::memory_order_acquire);
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
	ImGui::PopStyleVar(3);
}
