#include <switch.h>

#include "WindowSystem.h"

#include "util/crypto/aes128.h"
#include "config/ActiveSettings.h"
#include "Cemu/Logging/CemuLogging.h"
#include "config/CemuConfig.h"
#include "config/LaunchSettings.h"
#include "config/NetworkSettings.h"

#include "Cafe/CafeSystem.h"
#include "Cafe/TitleList/TitleList.h"
#include "Cafe/TitleList/TitleInfo.h"
#include "Cafe/TitleList/SaveList.h"
#include "Cafe/GraphicPack/GraphicPack2.h"
#include "Cafe/HW/Latte/Core/LatteOverlay.h"

#include "input/InputManager.h"
#include "audio/IAudioAPI.h"
#include "audio/IAudioInputAPI.h"
#include "audio/SwitchAudioAPI.h"
#include "platform/switch/SwitchAmiibo.h"
#include "platform/switch/SwitchSwkbd.h"
#include "platform/switch/SwitchInput.h"
#include "platform/switch/SwitchJit.h"
#include "platform/switch/SwitchLSFG.h"
#include "platform/switch/SwitchMemory.h"
#include "platform/switch/common/SwitchAddressSpace.h"
#include "platform/switch/SwitchOverlay.h"
#include "platform/switch/SwitchPlatform.h"
#include "platform/switch/SwitchToys.h"

#include "platform/switch/common/SwitchStorage.h"
#include "util/MemMapper/MemMapper.h"
#include "util/MemMapper/SwitchGuestMemory.h"
#include "Cemu/FileCache/FileCache.h"
#include "Cemu/ncrypto/ncrypto.h"

#include <thread>
#include <chrono>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string_view>

void PPCTimer_init();
bool SwitchCreateRenderer(int width, int height, std::string_view backend);
void SwitchDestroyRenderer();

namespace
{
	bool s_switchTripleBuffer = true;
	std::atomic_bool s_gameBootBoostActive{false};
	std::atomic_bool s_gameBootBoostHandoffArmed{false};
	std::atomic<uint64_t> s_gameBootBoostDeadlineTick{0};
	std::atomic_int s_surfaceWidth{1280};
	std::atomic_int s_surfaceHeight{720};
	std::atomic_bool s_exitRequested{false};
	std::atomic_bool s_returnToLauncher{true};
	std::atomic<SwitchGamePadLayout> s_gamePadLayout{SwitchGamePadLayout::Disabled};
	std::atomic<SwitchGamePadLayout> s_lastCompositeLayout{SwitchGamePadLayout::Right};
	std::atomic_bool s_gamePadLayoutChanged{false};
	std::string s_rendererBackend{"vk"};

	constexpr uint64_t kGameBootBoostHandoffTimeoutNs = 300'000'000'000ULL;

	void RestoreNormalCpuBoostMode()
	{
		s_gameBootBoostHandoffArmed.store(false, std::memory_order_release);
		s_gameBootBoostDeadlineTick.store(0, std::memory_order_release);

		if (!s_gameBootBoostActive.exchange(false, std::memory_order_acq_rel))
			return;

		appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
	}

	bool IsCompositeLayout(SwitchGamePadLayout layout)
	{
		return layout == SwitchGamePadLayout::Right || layout == SwitchGamePadLayout::Left ||
			layout == SwitchGamePadLayout::Below || layout == SwitchGamePadLayout::Above;
	}
}

void SwitchPlatform_ApplyWindowSurfaceSize(int width, int height)
{
	if (NWindow* window = nwindowGetDefault())
		nwindowSetDimensions(window, static_cast<u32>(width), static_cast<u32>(height));
}

bool SwitchPlatform_ShouldReturnToLauncher()
{
	return s_returnToLauncher.load(std::memory_order_acquire);
}

bool SwitchPlatform_UseTripleBuffer()
{
	return s_switchTripleBuffer;
}

void SwitchPlatform_SetGamePadLayout(SwitchGamePadLayout layout)
{
	if (IsCompositeLayout(layout))
		s_lastCompositeLayout.store(layout, std::memory_order_release);
	const SwitchGamePadLayout previous = s_gamePadLayout.exchange(layout, std::memory_order_acq_rel);
	if (previous != layout)
		s_gamePadLayoutChanged.store(true, std::memory_order_release);
	WindowSystem::GetWindowInfo().pad_open.store(layout != SwitchGamePadLayout::Disabled, std::memory_order_release);
}

SwitchGamePadLayout SwitchPlatform_GetGamePadLayout()
{
	return s_gamePadLayout.load(std::memory_order_acquire);
}

void SwitchPlatform_CycleGamePadMode()
{
	const SwitchGamePadLayout current = SwitchPlatform_GetGamePadLayout();
	if (current == SwitchGamePadLayout::Disabled)
		SwitchPlatform_SetGamePadLayout(SwitchGamePadLayout::PadOnly);
	else if (current == SwitchGamePadLayout::PadOnly)
		SwitchPlatform_SetGamePadLayout(s_lastCompositeLayout.load(std::memory_order_acquire));
	else
		SwitchPlatform_SetGamePadLayout(SwitchGamePadLayout::Disabled);
}

bool SwitchPlatform_TakeGamePadLayoutChange()
{
	return s_gamePadLayoutChanged.exchange(false, std::memory_order_acq_rel);
}

bool SwitchPlatform_IsGamePadOutputActive()
{
	return SwitchPlatform_GetGamePadLayout() != SwitchGamePadLayout::Disabled;
}

bool SwitchPlatform_IsGamePadOnly()
{
	return SwitchPlatform_GetGamePadLayout() == SwitchGamePadLayout::PadOnly;
}

bool SwitchPlatform_IsGamePadCompositeActive()
{
	return IsCompositeLayout(SwitchPlatform_GetGamePadLayout());
}

void SwitchPlatform_GetGamePadViewRegion(bool padView, int fullWidth, int fullHeight,
	int& x, int& y, int& width, int& height)
{
	x = 0;
	y = 0;
	width = fullWidth;
	height = fullHeight;
	switch (SwitchPlatform_GetGamePadLayout())
	{
	case SwitchGamePadLayout::Right:
		width = fullWidth / 2;
		x = padView ? width : 0;
		break;
	case SwitchGamePadLayout::Left:
		width = fullWidth / 2;
		x = padView ? 0 : width;
		break;
	case SwitchGamePadLayout::Below:
		height = fullHeight / 2;
		y = padView ? height : 0;
		break;
	case SwitchGamePadLayout::Above:
		height = fullHeight / 2;
		y = padView ? 0 : height;
		break;
	case SwitchGamePadLayout::PadOnly:
	case SwitchGamePadLayout::Disabled:
		break;
	}
}

void SwitchPlatform_GetGamePadImageRect(bool padView, int fullWidth, int fullHeight,
	int sourceWidth, int sourceHeight, int& x, int& y, int& width, int& height)
{
	SwitchPlatform_GetGamePadViewRegion(padView, fullWidth, fullHeight, x, y, width, height);
	int scaledWidth = width;
	int scaledHeight = sourceHeight * width / std::max(sourceWidth, 1);
	if (scaledHeight > height)
	{
		scaledWidth = sourceWidth * height / std::max(sourceHeight, 1);
		scaledHeight = height;
	}
	x += (width - scaledWidth) / 2;
	y += (height - scaledHeight) / 2;
	width = scaledWidth;
	height = scaledHeight;
}

void SwitchPlatform_BeginGameBootBoost()
{
	s_gameBootBoostHandoffArmed.store(false, std::memory_order_release);
	s_gameBootBoostDeadlineTick.store(0, std::memory_order_release);

	if (s_gameBootBoostActive.exchange(true, std::memory_order_acq_rel))
		return;

	const Result result = appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
	if (R_FAILED(result))
	{
		s_gameBootBoostActive.store(false, std::memory_order_release);
		return;
	}
}

void SwitchPlatform_ArmGameBootBoostHandoff()
{
	const uint64_t now = armGetSystemTick();
	if (!s_gameBootBoostActive.load(std::memory_order_acquire))
		return;

	s_gameBootBoostDeadlineTick.store(now + armNsToTicks(kGameBootBoostHandoffTimeoutNs), std::memory_order_release);
	s_gameBootBoostHandoffArmed.store(true, std::memory_order_release);
}

void SwitchPlatform_EndGameBootBoost()
{
	RestoreNormalCpuBoostMode();
}

void SwitchPlatform_NotifyGameFrameSubmitted()
{
	if (s_gameBootBoostHandoffArmed.exchange(false, std::memory_order_acq_rel))
		RestoreNormalCpuBoostMode();
}

void SwitchPlatform_PollGameBootState()
{
	const uint64_t now = armGetSystemTick();
	const uint64_t boostDeadline = s_gameBootBoostDeadlineTick.load(std::memory_order_acquire);
	if (s_gameBootBoostActive.load(std::memory_order_acquire) &&
		s_gameBootBoostHandoffArmed.load(std::memory_order_acquire) && boostDeadline != 0 &&
		static_cast<int64_t>(now - boostDeadline) >= 0)
	{
		RestoreNormalCpuBoostMode();
	}
}

namespace WindowSystem
{
	static WindowInfo s_windowInfo{};
	static NWindow* s_nwindow = nullptr;
	static bool s_cafeInitialized = false;
	static bool s_inputInitialized = false;
	static bool s_titleListInitialized = false;
	static bool s_saveListInitialized = false;
	static AppletHookCookie s_appletHookCookie{};
	static bool s_appletHooked = false;

	static constexpr int kHandheldWidth = 1280;
	static constexpr int kHandheldHeight = 720;
	static constexpr int kDockedWidth = 1920;
	static constexpr int kDockedHeight = 1080;

	static void UpdateOperationMode()
	{
		const bool docked = appletGetOperationMode() == AppletOperationMode_Console;
		s_surfaceWidth.store(docked ? kDockedWidth : kHandheldWidth, std::memory_order_release);
		s_surfaceHeight.store(docked ? kDockedHeight : kHandheldHeight, std::memory_order_release);
	}

	static void AppletHook(AppletHookType hook, void*)
	{
		switch (hook)
		{
		case AppletHookType_OnFocusState:
		case AppletHookType_OnResume:
		{
			const bool active = appletGetFocusState() == AppletFocusState_InFocus;
			s_windowInfo.app_active.store(active, std::memory_order_release);
			SwitchAudioAPI::SetApplicationActive(active);
			break;
		}
		case AppletHookType_OnOperationMode:
			UpdateOperationMode();
			break;
		case AppletHookType_OnExitRequest:
			s_exitRequested.store(true, std::memory_order_release);
			s_returnToLauncher.store(false, std::memory_order_release);
			break;
		default:
			break;
		}
	}

	static void SetupLifecycle()
	{
		UpdateOperationMode();
		appletSetFocusHandlingMode(AppletFocusHandlingMode_SuspendHomeSleepNotify);
		appletHook(&s_appletHookCookie, AppletHook, nullptr);
		s_appletHooked = true;
	}

	static void ShutdownLifecycle()
	{
		if (s_appletHooked)
		{
			appletUnhook(&s_appletHookCookie);
			s_appletHooked = false;
		}
	}

	static void SetupWindowInfo()
	{
		SetupLifecycle();
		s_nwindow = nwindowGetDefault();
		const int width = s_surfaceWidth.load(std::memory_order_acquire);
		const int height = s_surfaceHeight.load(std::memory_order_acquire);
		SwitchPlatform_ApplyWindowSurfaceSize(width, height);

		s_windowInfo.width = width;
		s_windowInfo.height = height;
		s_windowInfo.phys_width = width;
		s_windowInfo.phys_height = height;
		s_windowInfo.dpi_scale = 1.0;
		s_windowInfo.app_active = true;
		s_windowInfo.is_fullscreen = true;
		s_windowInfo.pad_open = false;

		auto& canvas = s_windowInfo.canvas_main;
		canvas.backend = WindowHandleInfo::Backend::ViNN;
		canvas.display = nullptr;
		canvas.surface = s_nwindow;
		s_windowInfo.window_main = canvas;
	}

	static void SetupPaths()
	{
		std::set<fs::path> failedWriteAccess;
		ActiveSettings::SetPaths(
			/*isPortableMode*/ true,
			/*executablePath*/ "sdmc:/switch/cemu/cemu.nro",
			/*userDataPath*/   "sdmc:/switch/cemu",
			/*configPath*/     "sdmc:/switch/cemu",
			/*cachePath*/      "sdmc:/switch/cemu/cache",
			/*dataPath*/       "romfs:/",
			failedWriteAccess);
		std::error_code mkdirEc;
		fs::create_directories(_utf8ToPath("sdmc:/switch/cemu/games"), mkdirEc);
	}

	static void InitializeMlcLayout()
	{
		const fs::path mlc = ActiveSettings::GetMlcPath();
		const fs::path directories[] = {
			mlc / "sys",
			mlc / "usr",
			mlc / "usr/title/00050000",
			mlc / "usr/title/0005000c",
			mlc / "usr/title/0005000e",
			mlc / "usr/save/00050010/1004a000/user/common/db",
			mlc / "usr/save/00050010/1004a100/user/common/db",
			mlc / "usr/save/00050010/1004a200/user/common/db",
			mlc / "sys/title/0005001b/1005c000/content",
		};
		for (const auto& directory : directories)
		{
			std::error_code ec;
			fs::create_directories(directory, ec);
			if (ec)
				throw std::runtime_error("failed to create MLC directory " + _pathToUtf8(directory) + ": " + ec.message());
		}

		const fs::path languagePath = mlc / "sys/title/0005001b/1005c000/content/language.txt";
		std::error_code ec;
		if (!fs::exists(languagePath, ec))
		{
			std::ofstream languageFile(languagePath);
			if (!languageFile.is_open())
				throw std::runtime_error("failed to create " + _pathToUtf8(languagePath));
			const char* languages[] = { "ja", "en", "fr", "de", "it", "es", "zh", "ko", "nl", "pt", "ru", "zh" };
			for (const char* language : languages)
				languageFile << '"' << language << "\",\n";
			languageFile.flush();
			if (!languageFile)
				throw std::runtime_error("failed to write " + _pathToUtf8(languagePath));
		}

		const fs::path countryPath = mlc / "sys/title/0005001b/1005c000/content/country.txt";
		ec.clear();
		if (!fs::exists(countryPath, ec))
		{
			std::ofstream countryFile(countryPath);
			if (!countryFile.is_open())
				throw std::runtime_error("failed to create " + _pathToUtf8(countryPath));
			for (size_t index = 0; index < NCrypto::GetCountryCount(); ++index)
			{
				const char* country = NCrypto::GetCountryAsString(index);
				countryFile << (std::string_view(country) == "NN" ? "NULL" : country) << ",\n";
			}
			countryFile.flush();
			if (!countryFile)
				throw std::runtime_error("failed to write " + _pathToUtf8(countryPath));
		}
	}

	static std::map<std::string, std::string> s_handoff;
	static void LoadHandoff()
	{
		fs::path p = ActiveSettings::GetUserDataPath("switch.ini");
		std::ifstream in(p);
		if (!in.is_open())
			return;
		std::string line;
		while (std::getline(in, line))
		{
			while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
				line.pop_back();
			size_t eq = line.find('=');
			if (eq != std::string::npos)
				s_handoff[line.substr(0, eq)] = line.substr(eq + 1);
		}
		in.close();
		std::error_code ec;
		fs::remove(p, ec);
	}
	static std::string Handoff(const char* key, const char* def = "")
	{
		auto it = s_handoff.find(key);
		return it != s_handoff.end() ? it->second : def;
	}

	static void CommonInit()
	{
		AES128_init();
		PPCTimer_init();

		SetupPaths();
		ActiveSettings::Init();
		cemuLog_log(LogType::Force, "Switch: stack region {}", SwitchMemory_DescribeStackRegion());

		const unsigned addressBits = SwitchAddressSpaceBits();
		cemuLog_log(LogType::Force, "Switch: process address space {} bits", addressBits);
		const auto addressError = SwitchLaunch::AddressSpaceError(addressBits);
		if (addressError != SwitchLaunch::Error::None)
			throw SwitchLaunch::Failure(addressError, "Cemu NX requires a 39-bit forwarder");

		GetConfigHandle().SetFilename(ActiveSettings::GetConfigPath("settings.xml").generic_wstring());
		GetConfigHandle().Load();
		InitializeMlcLayout();
		if (NetworkConfig::XMLExists())
			n_config.Load();

		LoadHandoff();
#if defined(ENABLE_DEKO3D)
		s_rendererBackend = "deko3d";
		GetConfig().graphic_api = kDeko3D;
#else
		const std::string requestedRenderer = Handoff("renderer", "vk");
		s_rendererBackend = requestedRenderer == "gl" || requestedRenderer == "zink" ?
			requestedRenderer : "vk";
		GetConfig().graphic_api = s_rendererBackend == "vk" ? kVulkan : kOpenGL;
#endif
		const bool importGuestMemory = s_rendererBackend == "vk" || s_rendererBackend == "deko3d";
		MemMapper::SetGuestMemoryImportRequired(importGuestMemory);
		// Older global/per-game settings must not disable the mandatory path.
		GetConfig().vk_host_memory_import = importGuestMemory;
		const std::string lsfgEnabled = Handoff("lsfg_enabled", "false");
		const std::string lsfgPerformance = Handoff("lsfg_performance", "true");
		SwitchLSFG_Configure(
			(s_rendererBackend == "vk" || s_rendererBackend == "deko3d") && (lsfgEnabled == "true" || lsfgEnabled == "1"),
			std::strtof(Handoff("lsfg_flow_scale", "0.25").c_str(), nullptr),
			lsfgPerformance != "false" && lsfgPerformance != "0");
		if (int shift = atoi(Handoff("timer_shift", "3").c_str()); shift >= 0 && shift <= 7)
			ActiveSettings::SetTimerShiftFactor((uint8)shift);
		s_switchTripleBuffer = (Handoff("triple_buffer", "1") != "0");
		const std::string cpuMode = Handoff("cpu_mode");
		if (cpuMode == "0")
			ActiveSettings::SetCPUModeOverride(CPUMode::SinglecoreInterpreter);
		else if (cpuMode == "1")
			ActiveSettings::SetCPUModeOverride(CPUMode::SinglecoreRecompiler);
		else if (cpuMode == "3")
			ActiveSettings::SetCPUModeOverride(CPUMode::MulticoreRecompiler);

		const std::string gamePadLayout = Handoff("gamepad_layout", "off");
		SwitchPlatform_SetGamePadLayout(
			gamePadLayout == "pad" ? SwitchGamePadLayout::PadOnly :
			gamePadLayout == "right" ? SwitchGamePadLayout::Right :
			gamePadLayout == "left" ? SwitchGamePadLayout::Left :
			gamePadLayout == "below" ? SwitchGamePadLayout::Below :
			gamePadLayout == "above" ? SwitchGamePadLayout::Above : SwitchGamePadLayout::Disabled);

		auto applyUsbSetting = [](const char* key, auto& value) {
			const std::string setting = Handoff(key);
			if (!setting.empty())
				value = setting == "true" || setting == "1";
		};
		applyUsbSetting("usb_skylanders", GetConfig().emulated_usb_devices.emulate_skylander_portal);
		applyUsbSetting("usb_infinity", GetConfig().emulated_usb_devices.emulate_infinity_base);
		applyUsbSetting("usb_dimensions", GetConfig().emulated_usb_devices.emulate_dimensions_toypad);

		SwitchToys_Initialize();
		SwitchOverlay_Initialize();

		LatteOverlay_init();

		GetConfig().audio_api = IAudioAPI::SwitchAudio;
		GetConfig().tv_device = L"default";
		GetConfig().tv_channels = kStereo;
		if (!GetConfig().pad_device.empty())
			GetConfig().pad_device = L"default";
		GetConfig().pad_channels = kStereo;
		GetConfig().portal_device.clear();

		IAudioAPI::InitializeStatic();
		IAudioInputAPI::InitializeStatic();
		GraphicPack2::LoadAll();
		InputManager::instance().load();
		s_inputInitialized = true;
		SwitchInput_Setup();

		CafeSystem::Initialize();
		s_cafeInitialized = true;

		CafeTitleList::Initialize(ActiveSettings::GetUserDataPath("title_list_cache.xml"));
		s_titleListInitialized = true;
		fs::path mlcPath = ActiveSettings::GetMlcPath();

		CafeSaveList::Initialize();
		s_saveListInitialized = true;
		if (!mlcPath.empty())
			CafeSaveList::SetMLCPath(mlcPath);
	}

	static void CommonShutdown()
	{
		SwitchPlatform_EndGameBootBoost();
		SwitchAmiibo_Shutdown();
		if (s_cafeInitialized)
		{
			CafeSystem::Shutdown();
			s_cafeInitialized = false;
		}
		SwitchDestroyRenderer();
		if (s_saveListInitialized)
		{
			CafeSaveList::Shutdown();
			s_saveListInitialized = false;
		}
		if (s_titleListInitialized)
		{
			CafeTitleList::Shutdown();
			s_titleListInitialized = false;
		}
		if (s_inputInitialized)
		{
			InputManager::instance().Shutdown();
			SwitchInput_Shutdown();
			s_inputInitialized = false;
		}
		FileCache::ShutdownAsyncWriter();
		SwitchJit_Shutdown();
		MemMapper::Shutdown();
	}

	static void AddInstalledTitleComponents(TitleId titleId)
	{
		const TitleId baseTitleId = TitleIdParser::MakeBaseTitleId(titleId);
		const uint64 low = baseTitleId & 0xFFFFFFFFULL;
		const TitleId candidates[] = {
			baseTitleId,
			(baseTitleId & ~(0xFFULL << 32)) | (0x0EULL << 32),
			(baseTitleId & ~(0xFFULL << 32)) | (0x0CULL << 32),
		};
		const fs::path titleRoot = ActiveSettings::GetMlcPath() / "usr/title";
		for (TitleId candidate : candidates)
		{
			const fs::path path = titleRoot /
				fmt::format("{:08x}", static_cast<uint32>(candidate >> 32)) /
				fmt::format("{:08x}", static_cast<uint32>(low));
			std::error_code ec;
			if (fs::is_directory(path, ec))
				CafeTitleList::AddTitleFromPath(path);
		}
	}

	// ums0:, ums1: ... - the mount aliases libusbhsfs hands out.
	static bool IsUsbStoragePath(const std::string& path)
	{
		const size_t colon = path.find(':');
		if (colon < 4 || colon == std::string::npos)
			return false;
		if (std::tolower((unsigned char)path[0]) != 'u' ||
			std::tolower((unsigned char)path[1]) != 'm' ||
			std::tolower((unsigned char)path[2]) != 's')
			return false;
		for (size_t i = 3; i < colon; i++)
			if (!std::isdigit((unsigned char)path[i]))
				return false;
		return true;
	}

	static void WaitForUsbStorage(const std::string& usbId, const std::string& path)
	{
		constexpr int kMaxAttempts = 100;   // 100 x 100ms = 10 seconds
		constexpr uint64_t kPollNs = 100'000'000ULL;
		for (int attempt = 0; attempt < kMaxAttempts; attempt++)
		{
			if (!usbId.empty())
			{
				if (!SwitchStorage::ResolveUsbPath(usbId).empty())
				{
					cemuLog_log(LogType::Force, "USB storage ready after {}ms", attempt * 100);
					return;
				}
			}
			else if (!SwitchStorage::GetUsbSnapshot().locations.empty())
			{
				cemuLog_log(LogType::Force, "USB storage ready after {}ms", attempt * 100);
				return;
			}
			svcSleepThread(kPollNs);
		}
		cemuLog_log(LogType::Force, "USB storage did not appear within 10s (id '{}', path '{}'), the game will probably not be found", usbId, path);
	}

	static bool ResolveTitleId(TitleId titleId, TitleId& baseTitleId)
	{
		AddInstalledTitleComponents(titleId);
		return CafeTitleList::FindBaseTitleId(titleId, baseTitleId);
	}

	static fs::path ResolveNusFolder(const fs::path& path)
	{
		std::error_code ec;
		if (!fs::is_directory(path, ec))
			return path;
		const fs::path tmd = path / "title.tmd";
		return fs::is_regular_file(tmd, ec) ? tmd : path;
	}

	static bool ResolveTitlePath(const fs::path& rawPath, TitleId& baseTitleId)
	{
		const fs::path path = ResolveNusFolder(rawPath);
		TitleInfo title{path};
		if (!title.IsValid())
			return false;
		CafeTitleList::AddTitleFromPath(path);
		AddInstalledTitleComponents(title.GetAppTitleId());
		CafeTitleList::StoreCacheFile();
		return CafeTitleList::FindBaseTitleId(title.GetAppTitleId(), baseTitleId);
	}

	static bool LaunchPreparedTitle()
	{
		if (!SwitchCreateRenderer(s_surfaceWidth.load(std::memory_order_acquire),
		                          s_surfaceHeight.load(std::memory_order_acquire),
		                          s_rendererBackend))
			throw std::runtime_error("failed to initialize graphics renderer: " + s_rendererBackend);
		CafeSystem::LaunchForegroundTitle();
		return true;
	}

	static bool BootTitle()
	{
		auto loadFile = LaunchSettings::GetLoadFile();
		if (loadFile.has_value())
		{
			fs::path path = loadFile.value();
			std::string ext = _pathToUtf8(path.extension());
			std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
			if (ext == ".rpx" || ext == ".elf")
			{
				auto r = CafeSystem::PrepareForegroundTitleFromStandaloneRPX(path);
				if (r == CafeSystem::PREPARE_STATUS_CODE::SUCCESS)
					return LaunchPreparedTitle();
				return false;
			}
			TitleId baseTitleId;
			if (ResolveTitlePath(path, baseTitleId) &&
				CafeSystem::PrepareForegroundTitle(baseTitleId) == CafeSystem::PREPARE_STATUS_CODE::SUCCESS)
				return LaunchPreparedTitle();
			return false;
		}

		if (auto chosen = LaunchSettings::GetLoadTitleID(); chosen.has_value())
		{
			TitleId baseTitleId;
			if (ResolveTitleId(chosen.value(), baseTitleId))
			{
				auto result = CafeSystem::PrepareForegroundTitle(baseTitleId);
				if (result == CafeSystem::PREPARE_STATUS_CODE::SUCCESS)
					return LaunchPreparedTitle();
				return false;
			}
			return false;
		}

		std::string line = Handoff("game");
		const std::string usbId = Handoff("game_usb_id");
		const bool gameIsOnUsb = !usbId.empty() || IsUsbStoragePath(line);
		if (gameIsOnUsb)
		{
			WaitForUsbStorage(usbId, line);
		}
		if (!usbId.empty())
		{
			std::string root = SwitchStorage::ResolveUsbPath(usbId);
			std::string relative = Handoff("game_usb_relative");
			while (!relative.empty() && (relative.front() == '/' || relative.front() == '\\'))
				relative.erase(relative.begin());
			if (!root.empty())
			{
				if (root.back() != '/')
					root += '/';
				line = root + relative;
			}
		}
		if (!line.empty())
		{
			TitleId baseTitleId = 0;
			bool resolved = false;
			if (line.rfind("id:", 0) == 0)
			{
				TitleId chosenId = 0;
				const std::string_view idText{line.data() + 3, line.size() - 3};
				auto [end, error] = std::from_chars(idText.data(), idText.data() + idText.size(), chosenId, 16);
				if (error == std::errc{} && end == idText.data() + idText.size())
					resolved = ResolveTitleId(chosenId, baseTitleId);
			}
			else
				resolved = ResolveTitlePath(_utf8ToPath(line), baseTitleId);
			if (resolved)
			{
				auto result = CafeSystem::PrepareForegroundTitle(baseTitleId);
				if (result == CafeSystem::PREPARE_STATUS_CODE::SUCCESS)
					return LaunchPreparedTitle();
				return false;
			}
			return false;
		}

		return false;
	}

	static void RunLoop()
	{
		PadState pad;
		padInitializeDefault(&pad);
		appletSetMediaPlaybackState(true);
		for (;;)
		{
			if (!appletMainLoop())
			{
				s_returnToLauncher.store(false, std::memory_order_release);
				break;
			}
			if (s_exitRequested.load(std::memory_order_acquire))
				break;
			SwitchPlatform_PollGameBootState();
			SwitchAmiibo_Update();
			if (SwitchSwkbd_IsAppletActive())
			{
				svcSleepThread(16'000'000ULL);
				continue;
			}
			padUpdate(&pad);
			const u64 down = padGetButtonsDown(&pad);
			const u64 held = padGetButtons(&pad);
			SwitchOverlay_UpdateInput(down, held);
			if (SwitchOverlay_TakeAmiiboScanRequest())
				SwitchAmiibo_StartScan();
			if (SwitchOverlay_TakeExitRequest())
			{
				s_returnToLauncher.store(true, std::memory_order_release);
				break;
			}
			svcSleepThread(16'000'000ULL);
		}
		appletSetMediaPlaybackState(false);
	}

	void Create()
	{
		s_exitRequested.store(false, std::memory_order_release);
		s_returnToLauncher.store(true, std::memory_order_release);
		try
		{
			SetupWindowInfo();
			SwitchPlatform_BeginGameBootBoost();
			CommonInit();
			if (!BootTitle())
				throw std::runtime_error("no launchable title was found");
			RunLoop();
		}
		catch (...)
		{
			CommonShutdown();
			ShutdownLifecycle();
			throw;
		}
		CommonShutdown();
		ShutdownLifecycle();
	}

	WindowInfo& GetWindowInfo() { return s_windowInfo; }

	void ShowErrorDialog(std::string_view, std::string_view, std::optional<ErrorCategory>) {}

	void GetWindowSize(int& w, int& h) { w = s_surfaceWidth.load(); h = s_surfaceHeight.load(); }
	void GetPadWindowSize(int& w, int& h)
	{
		int x, y;
		SwitchPlatform_GetGamePadViewRegion(true, s_surfaceWidth.load(), s_surfaceHeight.load(), x, y, w, h);
	}
	void GetWindowPhysSize(int& w, int& h) { w = s_surfaceWidth.load(); h = s_surfaceHeight.load(); }
	void GetPadWindowPhysSize(int& w, int& h) { GetPadWindowSize(w, h); }
	double GetWindowDPIScale() { return 1.0; }
	double GetPadDPIScale() { return 1.0; }
	bool IsPadWindowOpen() { return SwitchPlatform_IsGamePadOutputActive(); }
	bool IsKeyDown(uint32) { return false; }
	bool IsKeyDown(PlatformKeyCodes) { return false; }
	std::string GetKeyCodeName(uint32) { return ""; }
	bool InputConfigWindowHasFocus() { return false; }
	void NotifyGameLoaded() {}
	void NotifyGameExited() {}
	void RefreshGameList() {}
	bool IsFullScreen() { return true; }
	void UpdateWindowTitles(bool, bool, double) {}
	void CaptureInput(const ControllerState&, const ControllerState&) {}
} // namespace WindowSystem
