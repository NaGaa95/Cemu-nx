#pragma once

#include "DKCommon.h"

namespace Deko3D
{
	class DKImage
	{
	  public:
		static std::unique_ptr<DKImage> Create(u32 width, u32 height, DkImageFormat format,
											   u32 flags, DkImageType type = DkImageType_2D, u32 depth = 1, u32 mipLevels = 1);
		~DKImage();
		DkImageView MakeView(u32 mip = 0, u32 layer = 0, u32 layers = 1) const;
		DkImageView MakeCopyView(u32 mip = 0, u32 layer = 0, u32 layers = 1) const;
		const dk::Image& GetImage() const
		{
			return m_image;
		}
		const DkImageDescriptor& GetDescriptor() const
		{
			return m_descriptor;
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
		dk::UniqueMemBlock m_memory;
		dk::ImageLayout m_layout;
		dk::Image m_image;
		std::unique_ptr<dk::Image> m_copyImage;
		DkImageDescriptor m_descriptor{};
		u32 m_width = 0;
		u32 m_height = 0;
		DkImageType m_type = DkImageType_2D;
	};
} // namespace Deko3D
