#include "DKMemoryTracker.h"

namespace Deko3D::MemoryTracker
{
	namespace
	{
		std::mutex s_mutex;
		std::unordered_map<DkGpuAddr, u64> s_allocations;
		u64 s_usage = 0;
	} // namespace

	void Register(DkGpuAddr address, u64 size, std::string_view)
	{
		if (address == DK_GPU_ADDR_INVALID || !size)
			return;
		std::lock_guard lock(s_mutex);
		auto [it, inserted] = s_allocations.try_emplace(address, size);
		if (inserted)
			s_usage += size;
	}

	void Unregister(DkGpuAddr address)
	{
		std::lock_guard lock(s_mutex);
		const auto it = s_allocations.find(address);
		if (it == s_allocations.end())
			return;
		s_usage -= it->second;
		s_allocations.erase(it);
	}

	void RegisterMemBlock(DkMemBlock block, std::string_view label)
	{
		if (block)
			Register(dkMemBlockGetGpuAddr(block), dkMemBlockGetSize(block), label);
	}

	void UnregisterMemBlock(DkMemBlock block)
	{
		if (block)
			Unregister(dkMemBlockGetGpuAddr(block));
	}

	u64 GetUsage()
	{
		std::lock_guard lock(s_mutex);
		return s_usage;
	}
} // namespace Deko3D::MemoryTracker
