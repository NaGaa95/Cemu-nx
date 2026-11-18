#include <switch.h>
#include <array>
#include <map>
#include <mutex>
#include <unordered_map>

#include "platform/switch/SwitchJit.h"
#include "Cemu/Logging/CemuLogging.h"
#if defined(__SWITCH2__)
#include "platform/switch/nx2_platform.h"
#endif

namespace
{
	struct JitArena
	{
		Jit jit{};
		uint8_t* rwBase = nullptr;
		uint8_t* rxBase = nullptr;
		size_t size = 0;
		size_t cursor = 0;
		std::map<size_t, size_t> freeBlocks;
	};

	struct JitAllocation
	{
		size_t arenaIndex;
		size_t size;
	};

	constexpr size_t kMaxArenaCount = 2;
	std::array<JitArena, kMaxArenaCount> s_arenas{};
	size_t s_arenaCount = 0;
	size_t s_arenaSize = 0;
	bool s_ready = false;
	std::unordered_map<uintptr_t, JitAllocation> s_allocations;
	std::mutex s_mutex;

	constexpr size_t kCodeAlignment = 16;

	bool AlignCodeSize(size_t size, size_t& alignedSize)
	{
		if (size == 0 || size > SIZE_MAX - (kCodeAlignment - 1))
			return false;
		alignedSize = (size + kCodeAlignment - 1) & ~(kCodeAlignment - 1);
		return true;
	}

	bool CreateArena(size_t size)
	{
		if (s_arenaCount >= kMaxArenaCount)
			return false;

		JitArena& arena = s_arenas[s_arenaCount];
		arena = {};
		Result rc = jitCreate(&arena.jit, size);
#if defined(__SWITCH2__)
		// kaiHEN: the RX view is mapped from the start and each finished block is published (SwitchJit_FlushCode);
		// jitTransitionToExecutable would publish the whole arena.
		if (R_FAILED(rc))
		{
			arena = {};
			return false;
		}
#else
		if (R_FAILED(rc))
		{
			arena = {};
			return false;
		}
		if (arena.jit.type != JitType_CodeMemory)
		{
			jitClose(&arena.jit);
			arena = {};
			return false;
		}
		rc = jitTransitionToExecutable(&arena.jit);
		if (R_FAILED(rc))
		{
			jitClose(&arena.jit);
			arena = {};
			return false;
		}
#endif

		arena.rwBase = static_cast<uint8_t*>(jitGetRwAddr(&arena.jit));
		arena.rxBase = static_cast<uint8_t*>(jitGetRxAddr(&arena.jit));
		if (!arena.rwBase || !arena.rxBase)
		{
			jitClose(&arena.jit);
			arena = {};
			return false;
		}

		arena.size = size;
		arena.cursor = 0;
		arena.freeBlocks.clear();
#if defined(__SWITCH2__)
		// (s_arenas is a static array: the Jit stays at this address until SwitchJit_Shutdown unregisters it)
		nx2JitRegister(&arena.jit);
#endif
		++s_arenaCount;
		return true;
	}

	void* AllocateFromArena(size_t arenaIndex, size_t size)
	{
		JitArena& arena = s_arenas[arenaIndex];
		for (auto it = arena.freeBlocks.begin(); it != arena.freeBlocks.end(); ++it)
		{
			if (it->second < size)
				continue;
			const size_t offset = it->first;
			const size_t blockSize = it->second;
			arena.freeBlocks.erase(it);
			if (blockSize > size)
				arena.freeBlocks.emplace(offset + size, blockSize - size);
			void* address = arena.rwBase + offset;
			s_allocations.emplace(reinterpret_cast<uintptr_t>(address), JitAllocation{arenaIndex, size});
			return address;
		}

		if (arena.cursor > arena.size || size > arena.size - arena.cursor)
			return nullptr;

		void* address = arena.rwBase + arena.cursor;
		arena.cursor += size;
		s_allocations.emplace(reinterpret_cast<uintptr_t>(address), JitAllocation{arenaIndex, size});
		return address;
	}

	bool ContainsAddress(uintptr_t address, const uint8_t* base, size_t size)
	{
		const uintptr_t begin = reinterpret_cast<uintptr_t>(base);
		return address >= begin && address - begin < size;
	}
}

bool SwitchJit_SyscallsAvailable()
{
#if defined(__SWITCH2__)
	// No CodeMemory on the Switch 2 (a syscall outside the mask ends the process): jitCreate goes through kaiHEN.
	return true;
#else
	return envIsSyscallHinted(0x4B) && envIsSyscallHinted(0x4C);
#endif
}

bool SwitchJit_InitCodeArena(size_t size)
{
	std::lock_guard<std::mutex> lock(s_mutex);
	if (s_ready)
		return true;
	if (size == 0 || (size & 0xFFF) != 0)
		return false;
	if (!SwitchJit_SyscallsAvailable())
		return false;

	if (!CreateArena(size))
		return false;
	s_arenaSize = size;
	s_allocations.clear();
	s_ready = true;
	return true;
}

void SwitchJit_Shutdown()
{
	std::lock_guard<std::mutex> lock(s_mutex);
	if (!s_ready)
		return;

	for (size_t i = 0; i < s_arenaCount; ++i)
	{
#if defined(__SWITCH2__)
		nx2JitUnregister(&s_arenas[i].jit);
#endif
		jitClose(&s_arenas[i].jit);
		s_arenas[i] = {};
	}
	s_arenaCount = 0;
	s_arenaSize = 0;
	s_ready = false;
	s_allocations.clear();
}

void* SwitchJit_AllocRw(size_t size)
{
	std::lock_guard<std::mutex> lock(s_mutex);
	if (!s_ready)
		return nullptr;
	if (!AlignCodeSize(size, size))
		return nullptr;

	for (size_t i = 0; i < s_arenaCount; ++i)
	{
		if (void* address = AllocateFromArena(i, size))
			return address;
	}

	if (size > s_arenaSize || s_arenaCount >= kMaxArenaCount || !CreateArena(s_arenaSize))
		return nullptr;
	return AllocateFromArena(s_arenaCount - 1, size);
}

bool SwitchJit_FreeRw(void* address)
{
	if (!address)
		return true;
	std::lock_guard<std::mutex> lock(s_mutex);
	if (!s_ready)
		return false;

	uintptr_t ptr = reinterpret_cast<uintptr_t>(address);
	for (size_t i = 0; i < s_arenaCount; ++i)
	{
		const JitArena& arena = s_arenas[i];
		if (ContainsAddress(ptr, arena.rxBase, arena.size))
		{
			ptr = reinterpret_cast<uintptr_t>(arena.rwBase) +
				(ptr - reinterpret_cast<uintptr_t>(arena.rxBase));
			break;
		}
	}

	auto allocation = s_allocations.find(ptr);
	if (allocation == s_allocations.end())
		return false;

	const size_t arenaIndex = allocation->second.arenaIndex;
	JitArena& arena = s_arenas[arenaIndex];
	const uintptr_t rwBegin = reinterpret_cast<uintptr_t>(arena.rwBase);
	size_t begin = ptr - rwBegin;
	const size_t allocationSize = allocation->second.size;
	size_t end = begin + allocationSize;
	s_allocations.erase(allocation);

	auto next = arena.freeBlocks.lower_bound(begin);
	if (next != arena.freeBlocks.begin())
	{
		auto previous = std::prev(next);
		if (previous->first + previous->second == begin)
		{
			begin = previous->first;
			arena.freeBlocks.erase(previous);
		}
	}
	next = arena.freeBlocks.lower_bound(begin);
	if (next != arena.freeBlocks.end() && end == next->first)
	{
		end += next->second;
		arena.freeBlocks.erase(next);
	}
	if (end == arena.cursor)
	{
		arena.cursor = begin;
		return true;
	}
	arena.freeBlocks.emplace(begin, end - begin);
	return true;
}

void SwitchJit_FlushCode(void* rwAddr, size_t size)
{
	if (!rwAddr || size == 0)
		return;
#if defined(__SWITCH2__)
	// Copies the staged bytes to the RX view (published code is never written again).
	nx2JitPublish(rwAddr, size);
#else
	armDCacheClean(rwAddr, size);
	void* rxAddr = SwitchJit_RwToRx(rwAddr);
	if (rxAddr)
		armICacheInvalidate(rxAddr, size);
#endif
}

void* SwitchJit_RwToRx(void* rw)
{
	if (!rw)
		return nullptr;
	std::lock_guard<std::mutex> lock(s_mutex);
	const uintptr_t address = reinterpret_cast<uintptr_t>(rw);
	for (size_t i = 0; i < s_arenaCount; ++i)
	{
		const JitArena& arena = s_arenas[i];
		if (!ContainsAddress(address, arena.rwBase, arena.size))
			continue;
		return arena.rxBase + (address - reinterpret_cast<uintptr_t>(arena.rwBase));
	}
	return nullptr;
}

void SwitchJit_PinThreadToCore(int coreIndex)
{
#if defined(__SWITCH2__)
	if (coreIndex < 0 || coreIndex >= nx2CoreCount() || !(nx2CoreMask() & (UINT64_C(1) << coreIndex)))
		return;
#else
	if (coreIndex < 0 || coreIndex > 3)
		return;
#endif
	const Result result = svcSetThreadCoreMask(CUR_THREAD_HANDLE, coreIndex, 1u << coreIndex);
	if (R_FAILED(result))
		cemuLog_log(LogType::Force, "Switch: could not pin a thread to core {} (rc=0x{:08x})", coreIndex, result);
}
