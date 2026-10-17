#pragma once

#include "VulkanAPI.h"
#include "util/ChunkedHeap/ChunkedHeap.h"
#include <optional>
#include <span>
#include <unordered_map>

class VulkanRenderer;

class VKRGuestBufferCache
{
public:
	VKRGuestBufferCache(VulkanRenderer& renderer, uint32 size, uint32 alignment);
	~VKRGuestBufferCache();
	VkBuffer GetBuffer() const { return m_buffer; }
	uint32 Capacity() const { return m_size; }
	void Invalidate(uint32 address, uint32 size);
	void Reclaim();
	std::optional<uint32> Get(uint32 address, std::span<const uint8> bytes, uint32 epoch, uint64 draw);
	void Flush();

private:
	struct Entry
	{
		uint32 offset = ~0u, size = 0, capacity = 0, epoch = ~0u;
		uint64 generation = 0, checked = 0, fence = 0, draw = 0;
	};
	struct Retired { uint32 offset; uint64 fence; };
	uint64 Generation(uint32 address, uint32 size) const;
	bool Allocate(uint32 size, uint64 draw, uint32& offset);
	void Release(uint32 offset, uint64 fence);

	static constexpr uint32 PAGE_SHIFT = 16;
	static constexpr uint32 PAGE_COUNT = 1u << (32 - PAGE_SHIFT);
	static constexpr uint32 UNIFORM_RANGE = 64 * 1024;
	VulkanRenderer& m_renderer;
	VHeap m_heap;
	uint32 m_size, m_alignment, m_base;
	VkBuffer m_buffer = VK_NULL_HANDLE;
	VkDeviceMemory m_memory = VK_NULL_HANDLE;
	uint8* m_mapped = nullptr;
	uint64 m_generation = 1;
	std::vector<uint64> m_generations;
	std::unordered_map<uint32, Entry> m_entries;
	std::vector<Retired> m_retired;
	std::vector<VkMappedMemoryRange> m_flushRanges;
};
