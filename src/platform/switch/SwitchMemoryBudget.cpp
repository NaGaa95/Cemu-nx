#include "platform/switch/SwitchMemoryBudget.h"

#include <algorithm>
#include <limits>
#include <malloc.h>

extern size_t g_switchNewlibHeapSize;

namespace
{
	constexpr size_t MiB = 1024 * 1024;
	constexpr size_t kHeapAlignment = 2 * MiB;
	constexpr size_t kBudgetAlignment = 32 * MiB;
	constexpr size_t kMandatoryGuestBacking = 0x563F0000ull;
	constexpr size_t kMinimumFlexiblePool = 224 * MiB;
	constexpr size_t kMaximumFlexiblePool = 640 * MiB;
	constexpr size_t kMinimumNewlibHeap = 768 * MiB;
	constexpr size_t kJumpTableChunkSize = 64 * MiB;

	size_t AlignUp(size_t value, size_t alignment)
	{
		if (alignment == 0)
			return value;
		const size_t remainder = value % alignment;
		if (remainder == 0)
			return value;
		const size_t increment = alignment - remainder;
		return value <= std::numeric_limits<size_t>::max() - increment
			? value + increment : value;
	}

	size_t ScaledBudget(size_t divisor, size_t minimum, size_t maximum)
	{
		if (g_switchNewlibHeapSize == 0 || divisor == 0)
			return minimum;
		const size_t rawBudget = g_switchNewlibHeapSize / divisor;
		const size_t roundedInput = rawBudget <= std::numeric_limits<size_t>::max() - kBudgetAlignment / 2
			? rawBudget + kBudgetAlignment / 2 : rawBudget;
		const size_t scaled = roundedInput / kBudgetAlignment * kBudgetAlignment;
		return std::clamp(scaled, minimum, maximum);
	}

	size_t QueryContiguousHeadroom()
	{
		const struct mallinfo allocator = mallinfo();
		const size_t unextended = g_switchNewlibHeapSize >= allocator.arena
			? g_switchNewlibHeapSize - allocator.arena : 0;
		return unextended + std::min(allocator.keepcost, allocator.fordblks);
	}

	size_t GetSafetyReserve()
	{
		return ScaledBudget(12, 96 * MiB, 256 * MiB);
	}

	size_t GetStagingLimit()
	{
		return ScaledBudget(16, 64 * MiB, 256 * MiB);
	}
}

size_t SwitchMemoryBudget_ComputeGuestPoolSize(size_t heapTotal)
{
	if (heapTotal <= kMandatoryGuestBacking + kMinimumNewlibHeap)
		return 0;

	const size_t proportionalFlexiblePool = AlignUp(heapTotal / 14, kHeapAlignment);
	const size_t flexiblePool = std::clamp(proportionalFlexiblePool,
		kMinimumFlexiblePool, kMaximumFlexiblePool);
	size_t desiredPool = kMandatoryGuestBacking + flexiblePool;

	const size_t proportionalNewlibFloor = heapTotal / 3;
	const size_t newlibFloor = std::max(kMinimumNewlibHeap, proportionalNewlibFloor);
	const size_t maximumPool = heapTotal > newlibFloor ? heapTotal - newlibFloor : 0;
	desiredPool = std::min(desiredPool, maximumPool);
	if (desiredPool < kMandatoryGuestBacking)
		return 0;
	return desiredPool;
}

size_t SwitchMemoryBudget_GetMandatoryGuestBackingSize()
{
	return kMandatoryGuestBacking;
}

size_t SwitchMemoryBudget_GetJitArenaSize()
{
	return 64 * MiB;
}

size_t SwitchMemoryBudget_GetBufferCacheSize()
{
	return ScaledBudget(16, 64 * MiB, 164 * MiB);
}

size_t SwitchMemoryBudget_GetJumpTableChunkSize()
{
	// Horizon limits the number of CodeMemory objects available to the process.
	return kJumpTableChunkSize;
}

size_t SwitchMemoryBudget_GetPipelineCacheAllocationLimit()
{
	return SwitchMemoryBudget_GetPipelineCacheBudget(false).allocationLimit;
}

SwitchPipelineCacheBudget SwitchMemoryBudget_GetPipelineCacheBudget(bool startup)
{
	const size_t headroom = QueryContiguousHeadroom();
	const size_t reserve = startup ? 32 * MiB : GetSafetyReserve();
	return {headroom, reserve, headroom > reserve ? headroom - reserve : 0};
}

bool SwitchMemoryBudget_ShouldRecycleStaging(size_t currentSize, size_t growthSize)
{
	if (growthSize == 0)
		return false;
	const size_t limit = GetStagingLimit();
	if (currentSize > limit || growthSize > limit - currentSize)
		return true;

	const size_t headroom = QueryContiguousHeadroom();
	const size_t reserve = GetSafetyReserve();
	return growthSize > headroom || headroom - growthSize < reserve;
}

bool SwitchMemoryBudget_ShouldPrepareTextureAllocation(size_t allocationSize)
{
	if (allocationSize == 0)
		return false;
	const size_t headroom = QueryContiguousHeadroom();
	const size_t reserve = GetSafetyReserve();
	return allocationSize > headroom || headroom - allocationSize < reserve;
}

size_t SwitchMemoryBudget_SelectTextureChunkSize(size_t minimumSize, size_t preferredSize)
{
	constexpr size_t kTextureAlignment = 32 * 1024;
	constexpr size_t kMinimumPooledChunk = 8 * MiB;
	minimumSize = AlignUp(minimumSize, kTextureAlignment);
	preferredSize = AlignUp(std::max(preferredSize, minimumSize), kTextureAlignment);
	const size_t allocationFloor = std::min(preferredSize,
		std::max(minimumSize, kMinimumPooledChunk));

	const size_t headroom = QueryContiguousHeadroom();
	const size_t reserve = GetSafetyReserve();
	size_t selectedSize = preferredSize;
	while (selectedSize > allocationFloor)
	{
		if (selectedSize <= headroom && headroom - selectedSize >= reserve)
			break;
		const size_t smallerSize = (selectedSize / 2) & ~(kTextureAlignment - 1);
		if (smallerSize < allocationFloor)
		{
			selectedSize = allocationFloor;
			break;
		}
		selectedSize = smallerSize;
	}
	return selectedSize;
}
