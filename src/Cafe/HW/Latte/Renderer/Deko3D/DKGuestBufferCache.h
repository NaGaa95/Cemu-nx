#pragma once

#include "DKCommon.h"
#include "util/ChunkedHeap/ChunkedHeap.h"
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace Deko3D
{
	class DKGuestBufferCache
	{
	public:
		explicit DKGuestBufferCache(u32 size);
		~DKGuestBufferCache();
		void Invalidate(u32 address, u32 size);
		void InvalidateSnapshots(u32 address, u32 size);
		void TrackGpuWrite(u32 address, u32 size, u64 fence, DkBufExtents source = {}, u32 epoch = 0);
		u64 PendingWriteFence(u32 address, u32 size) const;
		std::optional<DkBufExtents> GpuSource(u32 address, u32 size, u32 epoch) const;
		void InvalidateGpuSource(DkBufExtents source);
		void Reclaim();
		u32 Capacity() const { return m_size; }
		bool HasPendingAllocations() const { return !m_retired.empty(); }
		std::optional<DkBufExtents> Get(u32 address, std::span<const u8> bytes,
			u32 epoch, u64 draw, bool& uploaded);

	private:
		struct Entry
		{
			u32 offset = ~0u;
			u32 size = 0;
			u32 capacity = 0;
			u32 epoch = ~0u;
			u64 generation = 0;
			u64 checked = 0;
			u64 fence = 0;
			u64 draw = 0;
		};
		struct Retired
		{
			u32 offset;
			u64 fence;
		};
		struct GpuWrite
		{
			u64 begin, end, fence;
		};
		struct GpuSourceRange
		{
			u64 begin, end, fence;
			DkBufExtents source;
			u32 epoch;
		};
		u64 Generation(u32 address, u32 size) const;
		bool Allocate(u32 size, u64 draw, u32& offset);
		void Release(u32 offset, u64 fence);

		static constexpr u32 PAGE_SHIFT = 16;
		static constexpr u32 PAGE_COUNT = 1u << (32 - PAGE_SHIFT);
		dk::UniqueMemBlock m_memory;
		VHeap m_heap;
		u32 m_size;
		u64 m_generation = 1;
		u64 m_reclaimedFence = 0;
		std::vector<u64> m_generations;
		std::unordered_map<u32, Entry> m_entries;
		std::vector<Retired> m_retired;
		std::vector<GpuWrite> m_gpuWrites;
		std::vector<GpuSourceRange> m_gpuSources;
		u64 m_gpuWriteBegin = 0, m_gpuWriteEnd = 0;
	};
}
