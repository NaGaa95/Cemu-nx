// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Cafe/HW/Latte/Renderer/Deko3D/DKContext.h"

#include "DKCommon.h"
#include "Cafe/HW/Latte/Renderer/Deko3D/DKMemoryTracker.h"

extern "C"
{
	uint32_t __nx_nv_transfermem_size = 32 * 1024 * 1024;
}

namespace Deko3D
{
	std::unique_ptr<DKContext> g_dk_context;

	DKContext::~DKContext() = default;

	std::unique_ptr<DKContext> DKContext::Create()
	{
		auto context = std::unique_ptr<DKContext>(new DKContext());
		if (!context->Initialize())
			return nullptr;
		return context;
	}

	bool DKContext::Initialize()
	{
		m_device = dk::DeviceMaker{}
					   .setCbDebug(&DKContext::DebugCallback)
					   .setFlags(DkDeviceFlags_DepthZeroToOne | DkDeviceFlags_OriginUpperLeft)
					   .create();
		if (!m_device)
		{
			cemuLog_log(LogType::Force, "deko3d: failed to create device");
			return false;
		}

		m_queue = dk::QueueMaker{m_device}
					  .setFlags(DkQueueFlags_Graphics | DkQueueFlags_Compute)
					  .setPerWarpScratchMemorySize(PER_WARP_SCRATCH_MEMORY_SIZE)
					  .create();
		if (!m_queue)
		{
			cemuLog_log(LogType::Force, "deko3d: failed to create queue");
			return false;
		}

		return true;
	}

	void DKContext::WaitIdle()
	{
		if (m_queue)
			m_queue.waitIdle();
	}

	void DKContext::DebugCallback(void*, const char* context, DkResult result,
								  const char* message)
	{
		if (result != DkResult_Success)
			cemuLog_log(LogType::Force, "deko3d error [{}] (result {}): {}", context, static_cast<int>(result),
						message);
	}
} // namespace Deko3D
