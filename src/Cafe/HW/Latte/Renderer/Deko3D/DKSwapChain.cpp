// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DKSwapChain.h"
#include "DKContext.h"
#include "DKMemoryTracker.h"
#include "platform/switch/SwitchPlatform.h"
#include "platform/switch/SwitchLSFG.h"
extern "C"
{
#include <switch/display/native_window.h>
}

namespace Deko3D
{
	std::unique_ptr<DKSwapChain> DKSwapChain::Create()
	{
		auto result = std::make_unique<DKSwapChain>();
		return result->Initialize() ? std::move(result) : nullptr;
	}

	bool DKSwapChain::Initialize()
	{
		NWindow* window = nwindowGetDefault();
		m_imageCount = SwitchPlatform_UseTripleBuffer() || SwitchLSFG_IsPrepared() ? 3 : 2;
		if (R_FAILED(nwindowGetDimensions(window, &m_width, &m_height)) || !m_width || !m_height)
			return false;
		dk::ImageLayout layout;
		dk::ImageLayoutMaker{g_dk_context->GetDevice()}
			.setFlags(DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_Usage2DEngine | DkImageFlags_HwCompression)
			.setFormat(DkImageFormat_RGBA8_Unorm)
			.setDimensions(m_width, m_height)
			.initialize(layout);
		const u64 stride = AlignUp(layout.getSize(), layout.getAlignment());
		const u64 size = AlignUp(stride * m_imageCount, DK_MEMBLOCK_ALIGNMENT);
		if (size > std::numeric_limits<u32>::max())
			return false;
		m_memory = dk::MemBlockMaker{g_dk_context->GetDevice(), static_cast<u32>(size)}
					   .setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image)
					   .create();
		if (!m_memory)
			return false;
		MemoryTracker::RegisterMemBlock(m_memory, "swapchain");
		std::array<const DkImage*, NUM_IMAGES> images;
		for (u32 i = 0; i < m_imageCount; ++i)
		{
			m_images[i].initialize(layout, m_memory, static_cast<u32>(stride * i));
			images[i] = &m_images[i];
		}
		m_swapchain = dk::SwapchainMaker{g_dk_context->GetDevice(), window, images.data(), m_imageCount}.create();
		return !!m_swapchain;
	}

	DKSwapChain::~DKSwapChain()
	{
		if (g_dk_context)
			g_dk_context->WaitIdle();
		m_swapchain = nullptr;
		MemoryTracker::UnregisterMemBlock(m_memory);
	}

	int DKSwapChain::Acquire()
	{
		return g_dk_context->GetGraphicsQueue().acquireImage(m_swapchain);
	}

	void DKSwapChain::Present(int slot)
	{
		g_dk_context->GetGraphicsQueue().presentImage(m_swapchain, slot);
	}

	void DKSwapChain::SetVSync(bool enabled)
	{
		if (m_vsync != enabled)
		{
			m_swapchain.setSwapInterval(enabled ? 1 : 0);
			m_vsync = enabled;
		}
	}
} // namespace Deko3D
