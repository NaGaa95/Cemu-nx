#include "VKRGuestBufferCache.h"
#include "VulkanRenderer.h"

VKRGuestBufferCache::VKRGuestBufferCache(VulkanRenderer& renderer, uint32 size, uint32 alignment)
	: m_renderer(renderer), m_heap(nullptr, size), m_size(size),
	  m_alignment(std::max(alignment, renderer.GetNonCoherentAtomSize())),
	  m_base((UNIFORM_RANGE + m_alignment - 1) / m_alignment * m_alignment), m_generations(PAGE_COUNT * 2)
{
	constexpr auto usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
	const VkDeviceSize allocationSize = static_cast<VkDeviceSize>(size) + 2 * m_base;
	auto& manager = *renderer.GetMemoryManager();
	if (!manager.CreateBuffer(allocationSize, usage, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, m_buffer, m_memory) &&
		!manager.CreateBuffer(allocationSize, usage, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, m_buffer, m_memory))
		throw std::bad_alloc();
	void* mapped = nullptr;
	if (vkMapMemory(renderer.GetLogicalDevice(), m_memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
	{
		manager.DeleteBuffer(m_buffer, m_memory);
		throw std::bad_alloc();
	}
	m_mapped = static_cast<uint8*>(mapped);
	for (VkDeviceSize offset : {VkDeviceSize{0}, static_cast<VkDeviceSize>(m_base) + size})
	{
		std::memset(m_mapped + offset, 0, m_base);
		VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
		range.memory = m_memory;
		range.offset = offset;
		range.size = m_base;
		m_flushRanges.push_back(range);
	}
}

VKRGuestBufferCache::~VKRGuestBufferCache()
{
	for (const auto& [address, entry] : m_entries)
		if (entry.offset != ~0u)
			m_heap.freeOffset(entry.offset);
	for (const auto& retired : m_retired)
		m_heap.freeOffset(retired.offset);
	vkUnmapMemory(m_renderer.GetLogicalDevice(), m_memory);
	m_renderer.GetMemoryManager()->DeleteBuffer(m_buffer, m_memory);
}

void VKRGuestBufferCache::Invalidate(uint32 address, uint32 size)
{
	if (!size)
		return;
	uint32 first = PAGE_COUNT + (address >> PAGE_SHIFT);
	uint32 end = PAGE_COUNT + static_cast<uint32>((std::min<uint64>(static_cast<uint64>(address) + size,
		0x1'0000'0000ULL) + (1u << PAGE_SHIFT) - 1) >> PAGE_SHIFT);
	std::fill(m_generations.begin() + first, m_generations.begin() + end, ++m_generation);
	while (first > 1)
	{
		first >>= 1;
		end = (end + 1) >> 1;
		for (uint32 index = first; index < end; ++index)
			m_generations[index] = std::max(m_generations[index * 2], m_generations[index * 2 + 1]);
	}
}

uint64 VKRGuestBufferCache::Generation(uint32 address, uint32 size) const
{
	uint32 first = PAGE_COUNT + (address >> PAGE_SHIFT);
	uint32 end = PAGE_COUNT + static_cast<uint32>((static_cast<uint64>(address) + size + (1u << PAGE_SHIFT) - 1) >> PAGE_SHIFT);
	uint64 result = 0;
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

void VKRGuestBufferCache::Reclaim()
{
	std::erase_if(m_retired, [&](const Retired& retired) {
		if (!m_renderer.HasCommandBufferFinished(retired.fence))
			return false;
		m_heap.freeOffset(retired.offset);
		return true;
	});
}

void VKRGuestBufferCache::Release(uint32 offset, uint64 fence)
{
	if (m_renderer.HasCommandBufferFinished(fence))
		m_heap.freeOffset(offset);
	else
		m_retired.push_back({offset, fence});
}

bool VKRGuestBufferCache::Allocate(uint32 size, uint64 draw, uint32& offset)
{
	Reclaim();
	if (m_heap.allocOffset(size, m_alignment, offset))
		return true;
	for (bool pending : {false, true})
	{
		for (auto it = m_entries.begin(); it != m_entries.end();)
		{
			const auto& entry = it->second;
			if (entry.draw == draw || (!pending && !m_renderer.HasCommandBufferFinished(entry.fence)))
			{
				++it;
				continue;
			}
			if (entry.offset != ~0u)
				Release(entry.offset, entry.fence);
			it = m_entries.erase(it);
			if (m_heap.allocOffset(size, m_alignment, offset))
				return true;
		}
	}
	return false;
}

std::optional<uint32> VKRGuestBufferCache::Get(uint32 address, std::span<const uint8> bytes, uint32 epoch, uint64 draw)
{
	if (!address || bytes.empty())
		return 0;
	if (bytes.size() > m_size || static_cast<uint64>(address) + bytes.size() > 0x1'0000'0000ULL)
		return std::nullopt;
	const uint32 size = static_cast<uint32>(bytes.size());
	auto& entry = m_entries[address];
	entry.draw = draw;
	auto* memory = m_mapped + m_base;
	uint64 generation = entry.checked == m_generation && entry.size >= size ? entry.generation : Generation(address, std::max(size, entry.size));
	bool unchanged = entry.offset != ~0u && entry.size >= size;
	if (unchanged && (entry.epoch != epoch || entry.generation != generation))
	{
		unchanged = std::memcmp(memory + entry.offset, bytes.data(), size) == 0;
		if (unchanged)
			entry.size = size;
	}
	if (!unchanged)
	{
		const uint32 capacity = (size + m_alignment - 1) / m_alignment * m_alignment;
		if (entry.capacity < capacity || !m_renderer.HasCommandBufferFinished(entry.fence))
		{
			uint32 offset;
			if (!Allocate(capacity, draw, offset))
				return std::nullopt;
			if (entry.offset != ~0u)
				Release(entry.offset, entry.fence);
			entry.offset = offset;
			entry.capacity = capacity;
		}
		std::memcpy(memory + entry.offset, bytes.data(), size);
		std::memset(memory + entry.offset + size, 0, capacity - size);
		VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
		range.memory = m_memory;
		range.offset = m_base + entry.offset;
		range.size = capacity;
		m_flushRanges.push_back(range);
		entry.size = size;
		generation = Generation(address, size);
	}
	entry.generation = generation;
	entry.checked = m_generation;
	entry.epoch = epoch;
	entry.fence = m_renderer.GetCurrentCommandBufferId();
	return m_base + entry.offset;
}

void VKRGuestBufferCache::Flush()
{
	if (m_flushRanges.empty())
		return;
	std::sort(m_flushRanges.begin(), m_flushRanges.end(), [](const auto& a, const auto& b) { return a.offset < b.offset; });
	size_t count = 0;
	for (const auto& range : m_flushRanges)
	{
		if (count && m_flushRanges[count - 1].offset + m_flushRanges[count - 1].size >= range.offset)
		{
			auto& previous = m_flushRanges[count - 1];
			previous.size = std::max(previous.offset + previous.size, range.offset + range.size) - previous.offset;
		}
		else
			m_flushRanges[count++] = range;
	}
	if (vkFlushMappedMemoryRanges(m_renderer.GetLogicalDevice(), static_cast<uint32>(count), m_flushRanges.data()) != VK_SUCCESS)
		throw std::runtime_error("Failed to flush Vulkan guest buffer snapshots");
	m_flushRanges.clear();
}
