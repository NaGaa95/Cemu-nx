#pragma once
#include "DKCommon.h"

namespace Deko3D::MemoryTracker
{
	void Register(DkGpuAddr address, u64 size, std::string_view label);
	void Unregister(DkGpuAddr address);
	void RegisterMemBlock(DkMemBlock block, std::string_view label);
	void UnregisterMemBlock(DkMemBlock block);
	u64 GetUsage();
} // namespace Deko3D::MemoryTracker
