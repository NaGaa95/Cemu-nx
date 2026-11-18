#include "platform/switch/SwitchThread.h"

#include <switch.h>

#include "Cemu/Logging/CemuLogging.h"
#if defined(__SWITCH2__)
#include "platform/switch/nx2_platform.h"
#endif

namespace
{
	constexpr int32_t kIdealCoreDontCare = -1;
	constexpr uint32_t kAppCoreMask = 0b0111u;
#if defined(__SWITCH2__)
	// Switch 2: six cores. 0-2 run the Espresso schedulers, the audio mixer core 3, background helpers cores 3-5.
	constexpr int kCoreLimit = 64;
	uint32_t AllCoreMask()
	{
		return static_cast<uint32_t>(nx2CoreMask());
	}
	uint32_t BackgroundCoreMask()
	{
		const uint32_t mask = AllCoreMask() & ~kAppCoreMask;
		return mask ? mask : AllCoreMask();
	}
	int32_t HelperCore()
	{
		const uint32_t helpers = AllCoreMask() & ~kAppCoreMask;
		return helpers ? __builtin_ctz(helpers) : -1;
	}
	// The lowest priority the process may use: the host title under kaiHEN refuses the Switch's 63.
	int32_t BackgroundPriority()
	{
		uint64_t mask = 0;
		if (R_SUCCEEDED(svcGetInfo(&mask, InfoType_PriorityMask, CUR_PROCESS_HANDLE, 0)) && mask != 0)
			return 63 - __builtin_clzll(mask);
		return 63;
	}
#else
	constexpr int kCoreLimit = 4;
	constexpr uint32_t kAllCoreMask = 0b1111u;
	constexpr uint32_t kHelperCoreMask = 0b1000u;
	constexpr int32_t kHelperCore = 3;
	constexpr int32_t BackgroundPriority()
	{
		return 63;
	}
#endif
} // namespace

void SwitchThread_RestoreProcessAffinity()
{
	uint64_t mask = 0;
	if (R_FAILED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)))
	{
		cemuLog_log(LogType::Force, "Switch: could not read the process core mask");
		return;
	}
	const Result rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, kIdealCoreDontCare, static_cast<u32>(mask));
	if (R_FAILED(rc))
		cemuLog_log(LogType::Force, "Switch: could not widen the main thread to core mask 0b{:04b} (rc=0x{:08x})",
				    static_cast<u32>(mask), rc);
#if defined(__SWITCH2__)
	else
		cemuLog_log(LogType::Force, "Switch 2: {} of 6 CPU cores granted, main thread mask 0b{:06b}",
				    SwitchThread_AllowedCoreCount(), static_cast<u32>(mask));
#else
	else
		cemuLog_log(LogType::Force, "Switch: {} of 4 CPU cores granted, main thread mask 0b{:04b}",
				    SwitchThread_AllowedCoreCount(), static_cast<u32>(mask));
#endif
}

int SwitchThread_AllowedCoreCount()
{
	uint64_t mask = 0;
	if (R_FAILED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)))
		return 3;
	int cores = 0;
	for (int core = 0; core < kCoreLimit; core++)
		if (mask & (UINT64_C(1) << core))
			cores++;
	return cores ? cores : 3;
}

void SwitchThread_PinToAppCores(const char* what)
{
	Result rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, kIdealCoreDontCare, kAppCoreMask);
	if (R_SUCCEEDED(rc))
		return;
	rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, 0, kAppCoreMask);
	if (R_FAILED(rc))
		cemuLog_log(LogType::Force, "Switch: could not restrict {} to cores 0/1/2 (rc=0x{:08x})", what, rc);
}

bool SwitchThread_PinToHelperCore(const char* what, int32_t priority)
{
#if defined(__SWITCH2__)
	const int32_t helperCore = HelperCore();
	const Result pinResult = helperCore >= 0 ?
		svcSetThreadCoreMask(CUR_THREAD_HANDLE, helperCore, UINT32_C(1) << helperCore) : KERNELRESULT(InvalidCoreId);
	if (R_SUCCEEDED(pinResult))
	{
		const bool prioritized = R_SUCCEEDED(svcSetThreadPriority(CUR_THREAD_HANDLE, static_cast<u32>(priority)));
		if (prioritized)
			cemuLog_log(LogType::Force, "Switch 2: {} runs on core {} at priority {}", what, helperCore, priority);
		else
			cemuLog_log(LogType::Force, "Switch 2: {} runs on core {} at the inherited priority", what, helperCore);
		return true;
	}
#else
	const Result pinResult = svcSetThreadCoreMask(CUR_THREAD_HANDLE, kHelperCore, kHelperCoreMask);
	if (R_SUCCEEDED(pinResult))
	{
		if (R_SUCCEEDED(svcSetThreadPriority(CUR_THREAD_HANDLE, static_cast<u32>(priority))))
			cemuLog_log(LogType::Force, "Switch: {} runs on core 3 at priority {}", what, priority);
		else
			cemuLog_log(LogType::Force, "Switch: {} runs on core 3 at the inherited priority", what);
		return true;
	}
#endif

	SwitchThread_PinToAppCores(what);
	if (R_FAILED(svcSetThreadPriority(CUR_THREAD_HANDLE, static_cast<u32>(priority))))
		cemuLog_log(LogType::Force, "Switch: could not raise {} to priority {}", what, priority);
	else
		cemuLog_log(LogType::Force, "Switch: {} stays on cores 0/1/2 at priority {} ({} of 4 cores granted)",
					    what, priority, SwitchThread_AllowedCoreCount());
	return false;
}

void SwitchThread_AllowHelperCore(const char* what)
{
#if defined(__SWITCH2__)
	if (R_FAILED(svcSetThreadCoreMask(CUR_THREAD_HANDLE, kIdealCoreDontCare, BackgroundCoreMask())))
		SwitchThread_PinToAppCores(what);
#else
	if (R_FAILED(svcSetThreadCoreMask(CUR_THREAD_HANDLE, kIdealCoreDontCare, kAllCoreMask)))
		SwitchThread_PinToAppCores(what);
#endif

	if (R_FAILED(svcSetThreadPriority(CUR_THREAD_HANDLE, static_cast<u32>(BackgroundPriority()))))
		cemuLog_log(LogType::Force, "Switch: could not lower {} to background priority", what);
}
