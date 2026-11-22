#include <switch.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

#include "WindowSystem.h"
#include "Cemu/Logging/CemuLogging.h"
#include "Common/ExceptionHandler/ExceptionHandler.h"
#include "audio/SwitchAudioAPI.h"
#include "config/LaunchSettings.h"
#include "SwitchPlatform.h"
#include "SwitchMemoryBudget.h"
#include "common/SwitchStorage.h"
#include "common/SwitchLaunchPolicy.h"
#include "platform/switch/SwitchThread.h"
#include "platform/switch/SwitchSaltyNX.h"
#include "util/MemMapper/MemMapper.h"
#include "util/MemMapper/SwitchGuestMemory.h"
#if defined(__SWITCH2__)
#include "platform/switch/nx2_platform.h"
#include <sys/stat.h>
#endif

std::atomic_bool g_isGPUInitFinished = false;

void* g_switchGuestPoolBase = nullptr;
size_t g_switchGuestPoolSize = 0;
size_t g_switchNewlibHeapSize = 0;
size_t g_switchHeapTotalSize = 0;
size_t g_switchInheritedHeapSize = 0;
size_t g_switchRecoveredHeapSize = 0;
u64 g_switchProcessMemoryTotal = 0;
u64 g_switchProcessMemoryUsed = 0;
Result g_switchHeapExpansionResult = 0;
bool g_switchHeapExpansionAttempted = false;


namespace
{
	constexpr size_t kHeapAlignment = 0x200000;
	constexpr size_t kApplicationReserve = 96 * 1024 * 1024;
	constexpr const char* kLastCoreErrorPath = "sdmc:/switch/cemu/last-core-error.log";

	void WriteLastCoreError(const char* stage, const char* detail)
	{
		FILE* file = std::fopen(kLastCoreErrorPath, "w");
		if (!file)
			return;
		std::fprintf(file, "stage=%s\n", stage ? stage : "unknown");
		std::fprintf(file, "detail=%s\n", detail ? detail : "unknown");
		std::fprintf(file, "inherited_heap=%zu\n", g_switchInheritedHeapSize);
		std::fprintf(file, "total_heap=%zu\n", g_switchHeapTotalSize);
		std::fprintf(file, "newlib_heap=%zu\n", g_switchNewlibHeapSize);
		std::fprintf(file, "guest_pool=%zu\n", g_switchGuestPoolSize);
		std::fprintf(file, "guest_backing=%s\n", MemMapper::DescribeGuestBacking());
		std::fprintf(file, "heap_expansion_attempted=%d\n", g_switchHeapExpansionAttempted ? 1 : 0);
		std::fprintf(file, "heap_expansion_result=0x%08x\n", static_cast<unsigned>(g_switchHeapExpansionResult));
		MemMapper::SwitchGuestMemory::PrintDiagnostics([](void* context, const char* line) {
			std::fprintf(static_cast<FILE*>(context), "%s\n", line);
		}, file);
		std::fclose(file);
	}

	bool RecoverGuestPoolFromApplicationHeap(char* overrideBase, size_t overrideSize)
	{
		u64 heapRegionAddress = 0;
		if (R_FAILED(svcGetInfo(&heapRegionAddress, InfoType_HeapRegionAddress, CUR_PROCESS_HANDLE, 0)) ||
			R_FAILED(svcGetInfo(&g_switchProcessMemoryTotal, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0)) ||
			R_FAILED(svcGetInfo(&g_switchProcessMemoryUsed, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0)))
			return false;

		const uintptr_t inheritedStart = reinterpret_cast<uintptr_t>(overrideBase);
		if (overrideSize > UINTPTR_MAX - inheritedStart)
			return false;
		const uintptr_t inheritedEnd = inheritedStart + overrideSize;
		if (heapRegionAddress > inheritedStart || inheritedEnd < heapRegionAddress)
			return false;

		const u64 currentHeapSize64 = inheritedEnd - heapRegionAddress;
		if (currentHeapSize64 > SIZE_MAX || g_switchProcessMemoryTotal <= g_switchProcessMemoryUsed + kApplicationReserve)
			return false;

		size_t expandable = static_cast<size_t>(g_switchProcessMemoryTotal -
			g_switchProcessMemoryUsed - kApplicationReserve) & ~(kHeapAlignment - 1);
		if (expandable < SwitchMemoryBudget_GetMandatoryGuestBackingSize() ||
			overrideSize > SIZE_MAX - expandable)
			return false;

		const size_t desiredPool = SwitchMemoryBudget_ComputeGuestPoolSize(overrideSize + expandable);
		if (desiredPool == 0)
			return false;
		size_t expansionSize = (desiredPool + kHeapAlignment - 1) & ~(kHeapAlignment - 1);
		if (expansionSize > expandable)
			expansionSize = expandable;
		if (expansionSize < SwitchMemoryBudget_GetMandatoryGuestBackingSize())
			return false;

		const size_t currentHeapSize = static_cast<size_t>(currentHeapSize64);
		if (currentHeapSize > SIZE_MAX - expansionSize)
			return false;

		void* expandedHeapBase = nullptr;
		g_switchHeapExpansionAttempted = true;
		g_switchHeapExpansionResult = svcSetHeapSize(&expandedHeapBase, currentHeapSize + expansionSize);
		if (R_FAILED(g_switchHeapExpansionResult) ||
			reinterpret_cast<uintptr_t>(expandedHeapBase) != heapRegionAddress)
			return false;

		g_switchGuestPoolBase = reinterpret_cast<void*>(inheritedEnd);
		g_switchGuestPoolSize = expansionSize;
		g_switchRecoveredHeapSize = expansionSize;
		return true;
	}
}

extern "C"
{
	u32 __nx_applet_type = AppletType_Application;

	extern char* fake_heap_start;
	extern char* fake_heap_end;

	void __libnx_initheap(void)
	{
		char* heapBase = nullptr;
		size_t heapTotal = 0;
		if (envHasHeapOverride())
		{
			heapBase = (char*)envGetHeapOverrideAddr();
			heapTotal = envGetHeapOverrideSize();
			if (!heapBase || heapTotal == 0)
				diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_BadGetInfo_Heap));
			g_switchInheritedHeapSize = heapTotal;
		}
		else
		{
			u64 avail = 0, used = 0;
			Result result = svcGetInfo(&avail, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
			if (R_FAILED(result))
				diagAbortWithResult(result);
			result = svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
			if (R_FAILED(result))
				diagAbortWithResult(result);
			if (avail <= used + 0x200000)
				diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_OutOfMemory));
			size_t sz = (avail - used - 0x200000) & ~0x1FFFFFull;
			void* a = nullptr;
			result = svcSetHeapSize(&a, sz);
			if (R_FAILED(result))
				diagAbortWithResult(result);
			if (!a)
				diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
			heapBase = (char*)a;
			heapTotal = sz;
		}

		const size_t targetGuestPool = SwitchMemoryBudget_ComputeGuestPoolSize(heapTotal);
		const bool recoveredGuestPool = targetGuestPool == 0 && envHasHeapOverride() &&
			RecoverGuestPoolFromApplicationHeap(heapBase, heapTotal);
		g_switchHeapTotalSize = heapTotal + g_switchRecoveredHeapSize;
		if (heapBase && targetGuestPool != 0 && targetGuestPool < heapTotal)
		{
			const size_t heapSize = (heapTotal - targetGuestPool) & ~0x1FFFFFull;
			g_switchGuestPoolBase = heapBase + heapSize;
			fake_heap_start = heapBase;
			fake_heap_end = heapBase + heapSize;
			g_switchGuestPoolSize = heapTotal - heapSize;
			g_switchNewlibHeapSize = heapSize;
		}
		else if (recoveredGuestPool)
		{
			fake_heap_start = heapBase;
			fake_heap_end = heapBase + heapTotal;
			g_switchNewlibHeapSize = heapTotal;
		}
		else
		{
			fake_heap_start = heapBase;
			fake_heap_end = heapBase + heapTotal;
			g_switchNewlibHeapSize = heapTotal;
		}
	}
}

void requireConsole() {}

static bool s_romfsMounted = false;
static bool s_hidInitialized = false;
static bool s_socketInitialized = false;
static bool s_storageInitialized = false;

static std::string ReadLaunchGamePath()
{
	FILE* file = std::fopen("sdmc:/switch/cemu/switch.ini", "r");
	if (!file)
		return {};
	char line[2048]{};
	std::string result;
	while (std::fgets(line, sizeof(line), file))
	{
		size_t length = std::strlen(line);
		while (length && (line[length - 1] == '\r' || line[length - 1] == '\n'))
			line[--length] = '\0';
		if (std::strncmp(line, "game=", 5) == 0)
		{
			result.assign(line + 5);
			break;
		}
	}
	std::fclose(file);
	return result;
}

// The defaults (3 bsd sessions, 9 KiB UDP send and 41 KiB receive buffers) are sized for light use: a socket call waits
// for a free session while others block in poll/select, and games' P2P traffic outgrows the UDP buffers.
static bool SwitchSocketInitialize()
{
	SocketInitConfig config = *socketGetDefaultInitConfig();
	config.num_bsd_sessions = 8;
	config.udp_tx_buf_size = 0x10000;
	config.udp_rx_buf_size = 0x20000;
	if (R_SUCCEEDED(socketInitialize(&config)))
		return true;
	return R_SUCCEEDED(socketInitializeDefault());
}

static bool SwitchPlatformInit()
{
	// Before anything else spawns a thread: new threads inherit this mask.
	SwitchThread_RestoreProcessAffinity();
	saltynx_start();
	s_romfsMounted = R_SUCCEEDED(romfsInit());
	s_hidInitialized = R_SUCCEEDED(hidInitialize());
	if (s_hidInitialized)
		padConfigureInput(8, HidNpadStyleSet_NpadStandard);
	s_socketInitialized = SwitchSocketInitialize();
	SwitchStorage::InitializeFromConfig("sdmc:/switch/cemu/launcher.ini", true, nullptr,
	                                    ReadLaunchGamePath());
	s_storageInitialized = true;
	return s_romfsMounted && s_hidInitialized;
}

static void SwitchPlatformExit()
{
	saltynx_stop();
	MemMapper::SwitchGuestMemory::DiscardEarly("core exited before adoption");
	SwitchAudioAPI::Destroy();
	if (s_storageInitialized)
	{
		SwitchStorage::Shutdown();
		s_storageInitialized = false;
	}
	if (s_socketInitialized)
		socketExit();
	if (s_hidInitialized)
		hidExit();
	if (s_romfsMounted)
		romfsExit();
}

#if defined(__SWITCH2__)
// drako3d releases its process-wide GPU client only in an atexit handler, which _Exit skips, leaving the launcher's GL
// without a GPU. -Wl,--wrap=atexit records that one handler (registered from nvLibInit, the branch target of
// dk::detail::nvdev::initialize) so ExitCore can run it; the C++ static destructors stay skipped.
extern "C" int __real_atexit(void (*function)(void));
extern "C" void CemuDrako3dNvInitialize(void) __asm__("_ZN2dk6detail5nvdev10initializeEv") __attribute__((weak));
static void (*s_drako3dAtexit)(void);

static bool IsDrako3dGpuRegistration(uintptr_t caller)
{
	const uintptr_t entry = reinterpret_cast<uintptr_t>(&CemuDrako3dNvInitialize);
	if (!entry)
		return false;
	const uint32_t instruction = *reinterpret_cast<const uint32_t*>(entry);
	if ((instruction & 0xFC000000u) != 0x14000000u) // b <nvLibInit>
		return false;
	int32_t offset = static_cast<int32_t>(instruction & 0x03FFFFFFu);
	if (offset & 0x02000000)
		offset -= 0x04000000;
	const uintptr_t nvLibInit = entry + static_cast<intptr_t>(offset) * 4;
	return caller >= nvLibInit && caller - nvLibInit < 0x800; // (nvLibInit is 0x660 bytes, its atexit call at +0x4ac)
}

extern "C" int __wrap_atexit(void (*function)(void));
extern "C" int __wrap_atexit(void (*function)(void))
{
	if (IsDrako3dGpuRegistration(reinterpret_cast<uintptr_t>(__builtin_return_address(0))))
		s_drako3dAtexit = function;
	return __real_atexit(function);
}

static void ReleaseDrako3dGpu()
{
	if (!s_drako3dAtexit)
		return;
	void (*release)(void) = s_drako3dAtexit;
	s_drako3dAtexit = nullptr;
	release();
}

[[noreturn]] static void ExitCore(int status)
{
	SwitchPlatformExit();
	cemuLog_shutdown();
	// The launcher runs next in this process: stop leftover threads, free their stacks and drako3d's GPU client.
	nx2ExitCleanupNow();
	ReleaseDrako3dGpu();
	std::_Exit(status);
}

// The Switch 2 launcher (switch_launcher/Makefile.switch2). There are no HOME Menu forwarders on the Switch 2.
static constexpr const char* kLauncherPath = "sdmc:/switch/cemu/cemu-nx2.nro";
#else
[[noreturn]] static void ExitCore(int status)
{
	SwitchPlatformExit();
	cemuLog_shutdown();
	std::_Exit(status);
}

// Matches EXIT_DETECTION_STR in switch_launcher/fwd/main.c.
static constexpr const char* kForwarderExitSentinel = "__CEMU_FORWARDER_EXIT__";
static constexpr const char* kLauncherPath = "sdmc:/switch/cemu/cemu.nro";
#endif

#if defined(__SWITCH2__)
static void ReturnToLauncher()
{
	// No forwarder restart on the Switch 2: appletRestartProgram would relaunch the game kaiHEN runs in, and there
	// are no HOME Menu forwarders. Chainload the launcher in place when it is installed.
	struct stat info{};
	if (envHasNextLoad() && stat(kLauncherPath, &info) == 0)
		envSetNextLoad(kLauncherPath, kLauncherPath);
}
#else
static std::string ForwarderLauncherPath()
{
	u64 programId = 0;
	if (R_FAILED(svcGetInfo(&programId, InfoType_ProgramId, CUR_PROCESS_HANDLE, 0)) || programId == 0)
		return {};

	char configPath[128]{};
	std::snprintf(configPath, sizeof(configPath), "sdmc:/switch/cemu/forwarders/%016llx.cfg",
	              (unsigned long long)programId);
	FILE* config = std::fopen(configPath, "rb");
	if (!config)
		return {};

	// <nro path>\0<arguments>\0, so the first string is the launcher.
	char target[FS_MAX_PATH]{};
	const size_t length = std::fread(target, 1, sizeof(target) - 1, config);
	std::fclose(config);
	target[length] = '\0';
	return target;
}

static void ReturnToLauncher()
{
	const std::string forwarderPath = ForwarderLauncherPath();
	if (!forwarderPath.empty())
	{
		cemuLog_log(LogType::Force, "Switch: restarting the forwarder to return to {}", forwarderPath);
		cemuLog_waitForFlush();

		// Does not return on success.
		const Result restartResult = appletRestartProgram(nullptr, 0);
		const Result launchResult = appletRequestLaunchApplication(0, nullptr);
		if (R_SUCCEEDED(launchResult) && envHasNextLoad())
		{
			envSetNextLoad(forwarderPath.c_str(), kForwarderExitSentinel);
			return;
		}
		cemuLog_log(LogType::Force,
		            "Switch: no clean restart (0x{:08x}/0x{:08x}), chainloading in place",
		            restartResult, launchResult);
	}

	if (envHasNextLoad())
		envSetNextLoad(kLauncherPath, kLauncherPath);
}
#endif

int main(int argc, char* argv[])
{
#if defined(__SWITCH2__)
	nx2DisplayInit();
#endif
	const bool platformInitialized = SwitchPlatformInit();
	if (!platformInitialized)
	{
		WriteLastCoreError("platform", "platform initialization failed");
		ExitCore(EXIT_FAILURE);
	}

	// A disengaged optional means that parsing succeeded and normal emulation
	// should continue.  A value is an explicit early-exit status (for example,
	// --help, --version, or a command-line parse error).
	if (const std::optional<int> commandLineResult = LaunchSettings::HandleCommandline(argc, argv);
		commandLineResult.has_value())
	{
		if (*commandLineResult == EXIT_SUCCESS)
			std::remove(kLastCoreErrorPath);
		else
			WriteLastCoreError("platform", "invalid command line");
		ExitCore(*commandLineResult);
	}
	std::remove(kLastCoreErrorPath);

	std::remove(SwitchLaunch::ErrorPath);
	int status = EXIT_SUCCESS;
	try
	{
		ExceptionHandler_Init();
		WindowSystem::Create();
	}
	catch (const SwitchLaunch::Failure& exception)
	{
		WriteLastCoreError("host-memory", exception.what());
		SwitchLaunch::WriteError(exception.error);
		status = EXIT_FAILURE;
	}
	catch (const std::exception& exception)
	{
		WriteLastCoreError("window", exception.what());
		SwitchLaunch::WriteError(SwitchLaunch::Error::LaunchFailed);
		status = EXIT_FAILURE;
	}
	catch (...)
	{
		WriteLastCoreError("window", "unknown exception");
		SwitchLaunch::WriteError(SwitchLaunch::Error::LaunchFailed);
		status = EXIT_FAILURE;
	}

	if (SwitchPlatform_ShouldReturnToLauncher())
		ReturnToLauncher();
	ExitCore(status);
}
