#pragma once

#include <cstddef>

size_t SwitchMemoryBudget_ComputeGuestPoolSize(size_t heapTotal);
size_t SwitchMemoryBudget_GetMandatoryGuestBackingSize();
size_t SwitchMemoryBudget_GetJitArenaSize();
size_t SwitchMemoryBudget_GetBufferCacheSize();
size_t SwitchMemoryBudget_GetJumpTableChunkSize();
size_t SwitchMemoryBudget_GetPipelineCacheAllocationLimit();

struct SwitchPipelineCacheBudget
{
	size_t headroom;
	size_t reserve;
	size_t allocationLimit;
};

SwitchPipelineCacheBudget SwitchMemoryBudget_GetPipelineCacheBudget(bool startup);

bool SwitchMemoryBudget_ShouldRecycleStaging(size_t currentSize, size_t growthSize);
bool SwitchMemoryBudget_ShouldPrepareTextureAllocation(size_t allocationSize);
size_t SwitchMemoryBudget_SelectTextureChunkSize(size_t minimumSize, size_t preferredSize);
