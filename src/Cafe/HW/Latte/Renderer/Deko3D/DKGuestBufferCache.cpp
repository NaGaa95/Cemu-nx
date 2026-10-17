#include "DKGuestBufferCache.h"
#include "DKCommandBufferManager.h"
#include "DKContext.h"
#include "DKMemoryTracker.h"

namespace Deko3D
{
	DKGuestBufferCache::DKGuestBufferCache(u32 size)
		: m_heap(nullptr, size), m_size(size), m_generations(PAGE_COUNT * 2)
	{
		m_memory = dk::MemBlockMaker{g_dk_context->GetDevice(), size}
			.setFlags(DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuCached).create();
		if (!m_memory)
			throw std::bad_alloc();
		MemoryTracker::RegisterMemBlock(m_memory, "guest buffer snapshots");
	}

	DKGuestBufferCache::~DKGuestBufferCache()
	{
		for (const auto& [address, entry] : m_entries)
			if (entry.offset != ~0u)
				m_heap.freeOffset(entry.offset);
		for (const auto& retired : m_retired)
			m_heap.freeOffset(retired.offset);
		MemoryTracker::UnregisterMemBlock(m_memory);
		DeferMemBlockDestruction(std::move(m_memory));
	}

	void DKGuestBufferCache::Invalidate(u32 address, u32 size)
	{
		if (!size)
			return;
		const u64 endAddress = static_cast<u64>(address) + size;
		std::erase_if(m_gpuSources, [&](const GpuSourceRange& range) {
			if (address >= range.end || endAddress <= range.begin)
				return false;
			return true;
		});
		InvalidateSnapshots(address, size);
	}

	void DKGuestBufferCache::InvalidateSnapshots(u32 address, u32 size)
	{
		if (!size)
			return;
		u32 first = PAGE_COUNT + (address >> PAGE_SHIFT);
		u32 end = PAGE_COUNT + static_cast<u32>((std::min<u64>(static_cast<u64>(address) + size,
			0x1'0000'0000ULL) + (1u << PAGE_SHIFT) - 1) >> PAGE_SHIFT);
		std::fill(m_generations.begin() + first, m_generations.begin() + end, ++m_generation);
		while (first > 1)
		{
			first >>= 1;
			end = (end + 1) >> 1;
			for (u32 index = first; index < end; ++index)
				m_generations[index] = std::max(m_generations[index * 2], m_generations[index * 2 + 1]);
		}
	}

	void DKGuestBufferCache::TrackGpuWrite(u32 address, u32 size, u64 fence, DkBufExtents source, u32 epoch)
	{
		if (!size)
			return;
		Invalidate(address, size);
		const u64 completed = g_dk_command_buffer_mgr->GetCompletedFenceCounter();
		std::erase_if(m_gpuWrites, [completed](const GpuWrite& write) { return write.fence <= completed; });
		std::erase_if(m_gpuSources, [completed](const GpuSourceRange& range) { return range.fence <= completed; });
		const u64 end = static_cast<u64>(address) + size;
		if (source.size >= size)
		{
			m_gpuSources.push_back({address, end, fence, {source.addr, size}, epoch});
		}
		m_gpuWriteBegin = address;
		m_gpuWriteEnd = end;
		for (const auto& write : m_gpuWrites)
		{
			m_gpuWriteBegin = std::min(m_gpuWriteBegin, write.begin);
			m_gpuWriteEnd = std::max(m_gpuWriteEnd, write.end);
		}
		if (!m_gpuWrites.empty())
		{
			auto& write = m_gpuWrites.back();
			if (write.fence == fence && address <= write.end && end >= write.begin)
			{
				write.begin = std::min<u64>(write.begin, address);
				write.end = std::max(write.end, end);
				return;
			}
		}
		m_gpuWrites.push_back({address, end, fence});
	}

	std::optional<DkBufExtents> DKGuestBufferCache::GpuSource(u32 address, u32 size, u32 epoch) const
	{
		if (!size)
			return std::nullopt;
		const u64 completed = g_dk_command_buffer_mgr->GetCompletedFenceCounter();
		const u64 end = static_cast<u64>(address) + size;
		for (const auto& range : m_gpuSources)
			if (range.fence > completed && range.epoch == epoch && address >= range.begin && end <= range.end)
			{
				const u64 source = range.source.addr + address - range.begin;
				if (!(source & 3))
					return DkBufExtents{source, size};
			}
		return std::nullopt;
	}

	void DKGuestBufferCache::InvalidateGpuSource(DkBufExtents source)
	{
		if (!source.size)
			return;
		std::erase_if(m_gpuSources, [&](const GpuSourceRange& range) {
			if (source.addr >= range.source.addr + range.source.size || source.addr + source.size <= range.source.addr)
				return false;
			return true;
		});
	}

	u64 DKGuestBufferCache::PendingWriteFence(u32 address, u32 size) const
	{
		const u64 completed = g_dk_command_buffer_mgr->GetCompletedFenceCounter();
		if (!size || m_gpuWrites.empty() || m_gpuWrites.back().fence <= completed)
			return 0;
		const u64 end = static_cast<u64>(address) + size;
		if (address >= m_gpuWriteEnd || end <= m_gpuWriteBegin)
			return 0;
		u64 fence = 0;
		for (const auto& write : m_gpuWrites)
			if (write.fence > completed && address < write.end && end > write.begin)
				fence = std::max(fence, write.fence);
		return fence;
	}

	u64 DKGuestBufferCache::Generation(u32 address, u32 size) const
	{
		u32 first = PAGE_COUNT + (address >> PAGE_SHIFT);
		u32 end = PAGE_COUNT + static_cast<u32>((static_cast<u64>(address) + size +
			(1u << PAGE_SHIFT) - 1) >> PAGE_SHIFT);
		u64 result = 0;
		while (first < end)
		{
			if (first & 1)
				result = std::max(result, m_generations[first++]);
			if (end & 1)
				result = std::max(result, m_generations[--end]);
			first >>= 1;
			end >>= 1;
		}
		return result;
	}

	void DKGuestBufferCache::Reclaim()
	{
		const u64 completed = g_dk_command_buffer_mgr->GetCompletedFenceCounter();
		if (completed == m_reclaimedFence)
		{
			return;
		}
		m_reclaimedFence = completed;
		std::erase_if(m_retired, [&](const Retired& retired) {
			if (retired.fence > completed)
				return false;
			m_heap.freeOffset(retired.offset);
			return true;
		});
	}

	void DKGuestBufferCache::Release(u32 offset, u64 fence)
	{
		if (fence <= g_dk_command_buffer_mgr->GetCompletedFenceCounter())
			m_heap.freeOffset(offset);
		else
			m_retired.push_back({offset, fence});
	}

	bool DKGuestBufferCache::Allocate(u32 size, u64 draw, u32& offset)
	{
		Reclaim();
		if (m_heap.allocOffset(size, DK_UNIFORM_BUF_ALIGNMENT, offset))
			return true;
		const u64 completed = g_dk_command_buffer_mgr->GetCompletedFenceCounter();
		for (bool pending : {false, true})
		{
			for (auto it = m_entries.begin(); it != m_entries.end();)
			{
				const auto& entry = it->second;
				if (entry.draw == draw || (!pending && entry.fence > completed))
				{
					++it;
					continue;
				}
				if (entry.offset != ~0u)
					Release(entry.offset, entry.fence);
				it = m_entries.erase(it);
				if (m_heap.allocOffset(size, DK_UNIFORM_BUF_ALIGNMENT, offset))
					return true;
			}
		}
		return false;
	}

	std::optional<DkBufExtents> DKGuestBufferCache::Get(u32 address, std::span<const u8> bytes,
		u32 epoch, u64 draw, bool& uploaded)
	{
		uploaded = false;
		if (bytes.empty() || bytes.size() > m_size ||
			static_cast<u64>(address) + bytes.size() > 0x1'0000'0000ULL)
			return std::nullopt;
		const u32 size = static_cast<u32>(bytes.size());
		auto& entry = m_entries[address];
		entry.draw = draw;
		auto* memory = static_cast<u8*>(m_memory.getCpuAddr());
		u64 generation = entry.checked == m_generation && entry.size >= size ? entry.generation :
			Generation(address, std::max(size, entry.size));
		bool unchanged = entry.offset != ~0u && entry.size >= size;
		if (unchanged && (entry.epoch != epoch || entry.generation != generation))
		{
			unchanged = std::memcmp(memory + entry.offset, bytes.data(), size) == 0;
			if (unchanged)
				entry.size = size;
		}
		if (!unchanged)
		{
			const u32 capacity = AlignUp(size, DK_UNIFORM_BUF_ALIGNMENT);
			if (entry.capacity < capacity || entry.fence > g_dk_command_buffer_mgr->GetCompletedFenceCounter())
			{
				u32 offset;
				if (!Allocate(capacity, draw, offset))
					return std::nullopt;
				if (entry.offset != ~0u)
					Release(entry.offset, entry.fence);
				entry.offset = offset;
				entry.capacity = capacity;
			}
			std::memcpy(memory + entry.offset, bytes.data(), size);
			std::memset(memory + entry.offset + size, 0, capacity - size);
			dkMemBlockFlushCpuCache(m_memory, entry.offset, capacity);
			entry.size = size;
			generation = Generation(address, size);
			uploaded = true;
		}
		entry.generation = generation;
		entry.checked = m_generation;
		entry.epoch = epoch;
		entry.fence = g_dk_command_buffer_mgr->GetCurrentFenceCounter();
		return DkBufExtents{m_memory.getGpuAddr() + entry.offset, size};
	}
}
