#pragma once
#include "DKCommon.h"

namespace Deko3D
{
	class DKSwapChain
	{
	  public:
		// On an NWindow (window); null: the default window, whose display pacing it also applies
		static std::unique_ptr<DKSwapChain> Create(void* window = nullptr);
		~DKSwapChain();
		int Acquire();
		void Present(int slot);
		void SetVSync(bool enabled);
		const dk::Image& GetImage(u32 slot) const
		{
			return m_images.at(slot);
		}
		u32 Width() const
		{
			return m_width;
		}
		u32 Height() const
		{
			return m_height;
		}

	  private:
		bool Initialize(void* window);
		static constexpr u32 NUM_IMAGES = 3;
		dk::UniqueMemBlock m_memory;
		std::array<dk::Image, NUM_IMAGES> m_images;
		dk::UniqueSwapchain m_swapchain;
		u32 m_width = 0;
		u32 m_height = 0;
		u32 m_imageCount = NUM_IMAGES;
		std::optional<bool> m_vsync;
	};
} // namespace Deko3D
