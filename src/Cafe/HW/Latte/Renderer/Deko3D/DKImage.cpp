#include "DKImage.h"
#include "DKCommandBufferManager.h"
#include "DKContext.h"
#include "DKMemoryTracker.h"

namespace Deko3D
{
	std::unique_ptr<DKImage> DKImage::Create(u32 width, u32 height, DkImageFormat format,
											 u32 flags, DkImageType type, u32 depth, u32 mipLevels)
	{
		auto result = std::make_unique<DKImage>();
		const bool compressed = format >= DkImageFormat_RGB_BC1 && format <= DkImageFormat_RGBA_BC7_Unorm_sRGB;
		const u32 rows = compressed ? (height + 3) / 4 : height;
		const u32 gobs = (rows + rows / 2 + 7) / 8;
		u32 tile = gobs >= 16 ? 4 : gobs >= 8 ? 3
								: gobs >= 4	  ? 2
								: gobs >= 2	  ? 1
											  : 0;
		while (tile && rows <= (8u << (tile - 1)))
			--tile;
		auto maker = dk::ImageLayoutMaker{g_dk_context->GetDevice()}
			.setFlags(flags | DkImageFlags_CustomTileSize)
			.setType(type)
			.setFormat(format)
			.setTileSize(static_cast<DkTileSize>(tile))
			.setDimensions(width, type == DkImageType_1DArray ? depth : height,
						   type == DkImageType_CubemapArray ? depth / 6 : depth)
			.setMipLevels(mipLevels);
		maker.initialize(result->m_layout);
		const u64 size = AlignUp(result->m_layout.getSize(),
								 std::max<u64>(result->m_layout.getAlignment(), DK_MEMBLOCK_ALIGNMENT));
		if (!size || size > std::numeric_limits<u32>::max())
			return nullptr;
		result->m_memory = dk::MemBlockMaker{g_dk_context->GetDevice(), static_cast<u32>(size)}
							   .setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image | DkMemBlockFlags_ZeroFillInit)
							   .create();
		if (!result->m_memory)
			return nullptr;
		MemoryTracker::RegisterMemBlock(result->m_memory, "image");
		result->m_image.initialize(result->m_layout, result->m_memory, 0);
		if (type == DkImageType_1D || type == DkImageType_1DArray)
		{
			dk::ImageLayout copyLayout;
			maker.setType(type == DkImageType_1D ? DkImageType_2D : DkImageType_2DArray)
				.setDimensions(width, 1, depth)
				.setTileSize(DkTileSize_OneGob)
				.initialize(copyLayout);
			result->m_copyImage = std::make_unique<dk::Image>();
			result->m_copyImage->initialize(copyLayout, result->m_memory, 0);
		}
		result->m_width = width;
		result->m_height = height;
		result->m_type = type;
		const dk::ImageView view{result->m_image};
		dkImageDescriptorInitialize(&result->m_descriptor, &view, false, false);
		return result;
	}

	DKImage::~DKImage()
	{
		MemoryTracker::UnregisterMemBlock(m_memory);
		DeferMemBlockDestruction(std::move(m_memory));
	}

	DkImageView DKImage::MakeView(u32 mip, u32 layer, u32 layers) const
	{
		DkImageView view;
		dkImageViewDefaults(&view, &m_image);
		view.mipLevelOffset = mip;
		view.mipLevelCount = 1;
		view.layerOffset = m_type == DkImageType_3D ? 0 : layer;
		view.layerCount = m_type == DkImageType_3D ? 0 : layers;
		return view;
	}

	DkImageView DKImage::MakeCopyView(u32 mip, u32 layer, u32 layers) const
	{
		auto view = MakeView(mip, layer, layers);
		if (m_copyImage)
			view.pImage = m_copyImage.get();
		return view;
	}
} // namespace Deko3D
