#include "platform/switch/SwitchMemory.h"

#include <switch.h>

#include "Cemu/Logging/CemuLogging.h"

#include <atomic>
#include <stdexcept>
#include <cstdio>
#include <cstdlib>

namespace
{
	enum class CacheOperation { Clean, Flush, Invalidate };

#if defined(__SWITCH2__)
	// Switch 2: cache maintenance by VA at EL0 (no kernel cache SVCs under kaiHEN). dc ivac is privileged, so an
	// invalidate is a clean+invalidate: harmless, since FlushForGpuWrite already wrote the lines back.
	void synchronizeCache(CacheOperation operation, const void* address, size_t size)
	{
		if (size == 0)
			return;
		if (operation == CacheOperation::Clean)
			armDCacheClean(const_cast<void*>(address), size);
		else
			armDCacheFlush(const_cast<void*>(address), size);
	}
#else
	void synchronizeCache(CacheOperation operation, const void* address, size_t size)
	{
		if (size == 0)
			return;
		const Handle process = envGetOwnProcessHandle();
		const bool canStore = process != INVALID_HANDLE && envIsSyscallHinted(0x5E);
		const bool canInvalidate = process != INVALID_HANDLE && envIsSyscallHinted(0x5D);
		const bool canFlush = envIsSyscallHinted(0x2B);
		const char* name = operation == CacheOperation::Clean ? "clean" :
			operation == CacheOperation::Flush ? "flush" : "invalidate";
		const u64 start = armGetSystemTick();
		bool retrying = false;
		Result rc;
		for (;;)
		{
			if (operation == CacheOperation::Invalidate)
				rc = canInvalidate ? svcInvalidateProcessDataCache(process, reinterpret_cast<uintptr_t>(address), size) : KERNELRESULT(NotImplemented);
			else if (operation == CacheOperation::Clean && canStore)
				rc = svcStoreProcessDataCache(process, reinterpret_cast<uintptr_t>(address), size);
			else
				rc = canFlush ? svcFlushDataCache(const_cast<void*>(address), size) : KERNELRESULT(NotImplemented);
			if (R_SUCCEEDED(rc))
				return;
			if (R_VALUE(rc) != KERNELRESULT(InvalidMemoryState) ||
				armGetSystemTick() - start >= armNsToTicks(5'000'000'000ull))
				break;
			if (!retrying)
			{
				retrying = true;
				static std::atomic<unsigned> reports{0};
				if (reports.fetch_add(1, std::memory_order_relaxed) < 8)
					cemuLog_log(LogType::Force, "Host memory import: retrying cache {} at {}+0x{:x}, rc=0x{:08x}; {}",
						name, address, size, rc, SwitchMemory_DescribeMapping(address));
			}
			svcSleepThread(1'000'000);
		}
		cemuLog_log(LogType::Force, "Host memory import: cache {} failed at {}+0x{:x}, rc=0x{:08x}; {}",
			name, address, size, rc, SwitchMemory_DescribeMapping(address));
		throw std::runtime_error("Imported MEM2 cache synchronization failed");
	}
#endif
}

bool SwitchMemory_CanSynchronizeForGpu()
{
#if defined(__SWITCH2__)
	return true;
#else
	return envGetOwnProcessHandle() != INVALID_HANDLE && envIsSyscallHinted(0x5D) && envIsSyscallHinted(0x2B);
#endif
}

void SwitchMemory_CleanForGpu(const void* address, size_t size)
{
	synchronizeCache(CacheOperation::Clean, address, size);
}

void SwitchMemory_FlushForGpuWrite(const void* address, size_t size)
{
	synchronizeCache(CacheOperation::Flush, address, size);
}

void SwitchMemory_InvalidateFromGpu(const void* address, size_t size)
{
	synchronizeCache(CacheOperation::Invalidate, address, size);
}

std::string SwitchMemory_DescribeStackRegion()
{
	u64 base = 0, size = 0;
	char text[192];
	if (R_FAILED(svcGetInfo(&base, InfoType_StackRegionAddress, CUR_PROCESS_HANDLE, 0)) ||
		R_FAILED(svcGetInfo(&size, InfoType_StackRegionSize, CUR_PROCESS_HANDLE, 0)))
		return "unavailable";

	const u64 end = base + size;
	u64 largest = 0, largestAt = 0;
	for (u64 address = base; address < end;)
	{
		MemoryInfo info{};
		u32 pageInfo = 0;
		if (R_FAILED(svcQueryMemory(&info, &pageInfo, address)))
			break;
		u64 blockEnd = info.addr + info.size;
		if (blockEnd > end)
			blockEnd = end;
		if ((info.type & 0xFF) == MemType_Unmapped)
		{
			const u64 start = info.addr < base ? base : info.addr;
			if (blockEnd > start && blockEnd - start > largest)
			{
				largest = blockEnd - start;
				largestAt = start;
			}
		}
		if (blockEnd <= address)
			break;
		address = blockEnd;
	}
	std::snprintf(text, sizeof(text), "0x%llx+%lluMB, largest free span %lluMB at 0x%llx",
			(unsigned long long)base, (unsigned long long)(size >> 20),
			(unsigned long long)(largest >> 20), (unsigned long long)largestAt);
	return text;
}

std::string SwitchMemory_DescribeMapping(const void* address)
{
	MemoryInfo info{};
	u32 pageInfo = 0;
	char text[192];
	const Result rc = svcQueryMemory(&info, &pageInfo, reinterpret_cast<u64>(address));
	if (R_FAILED(rc))
		std::snprintf(text, sizeof(text), "svcQueryMemory failed (rc=0x%08x)", static_cast<unsigned>(rc));
	else
		std::snprintf(text, sizeof(text), "type 0x%02x perm 0x%x attr 0x%x device refs %u ipc refs %u, block 0x%llx+0x%llx",
			static_cast<unsigned>(info.type), static_cast<unsigned>(info.perm), static_cast<unsigned>(info.attr),
			static_cast<unsigned>(info.device_refcount), static_cast<unsigned>(info.ipc_refcount),
			static_cast<unsigned long long>(info.addr),
			static_cast<unsigned long long>(info.size));
	return text;
}
