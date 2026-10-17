#include "DKTexture.h"

namespace Deko3D
{
	DkImageType ImageType(Latte::E_DIM dim)
	{
		switch (dim)
		{
		case Latte::E_DIM::DIM_1D:
			return DkImageType_1D;
		case Latte::E_DIM::DIM_1D_ARRAY:
			return DkImageType_1DArray;
		case Latte::E_DIM::DIM_3D:
			return DkImageType_3D;
		case Latte::E_DIM::DIM_2D_ARRAY:
		case Latte::E_DIM::DIM_2D_ARRAY_MSAA:
			return DkImageType_2DArray;
		case Latte::E_DIM::DIM_CUBEMAP:
			return DkImageType_CubemapArray;
		default:
			return DkImageType_2D;
		}
	}

	bool IsCompressed(DkImageFormat format)
	{
		return format >= DkImageFormat_RGB_BC1 && format <= DkImageFormat_RG_BC5_Snorm;
	}

	u32 FormatBytes(DkImageFormat format)
	{
		switch (format)
		{
		case DkImageFormat_R8_Unorm:
		case DkImageFormat_R8_Snorm:
		case DkImageFormat_R8_Uint:
		case DkImageFormat_R8_Sint:
			return 1;
		case DkImageFormat_R16_Unorm:
		case DkImageFormat_R16_Snorm:
		case DkImageFormat_R16_Uint:
		case DkImageFormat_R16_Sint:
		case DkImageFormat_R16_Float:
		case DkImageFormat_RG8_Unorm:
		case DkImageFormat_RG8_Snorm:
		case DkImageFormat_RG8_Uint:
		case DkImageFormat_RG8_Sint:
		case DkImageFormat_Z16:
		case DkImageFormat_RGB565_Unorm:
		case DkImageFormat_RGBA4_Unorm:
		case DkImageFormat_RGB5A1_Unorm:
			return 2;
		case DkImageFormat_RGBA16_Unorm:
		case DkImageFormat_RGBA16_Snorm:
		case DkImageFormat_RGBA16_Uint:
		case DkImageFormat_RGBA16_Sint:
		case DkImageFormat_RGBA16_Float:
		case DkImageFormat_RG32_Uint:
		case DkImageFormat_RG32_Sint:
		case DkImageFormat_RG32_Float:
		case DkImageFormat_ZF32_X24S8:
			return 8;
		case DkImageFormat_RGBA32_Uint:
		case DkImageFormat_RGBA32_Sint:
		case DkImageFormat_RGBA32_Float:
			return 16;
		case DkImageFormat_RGB32_Uint:
		case DkImageFormat_RGB32_Sint:
		case DkImageFormat_RGB32_Float:
			return 12;
		case DkImageFormat_RGB_BC1:
		case DkImageFormat_RGBA_BC1:
		case DkImageFormat_RGB_BC1_sRGB:
		case DkImageFormat_RGBA_BC1_sRGB:
		case DkImageFormat_R_BC4_Unorm:
		case DkImageFormat_R_BC4_Snorm:
			return 8;
		case DkImageFormat_RGBA_BC2:
		case DkImageFormat_RGBA_BC2_sRGB:
		case DkImageFormat_RGBA_BC3:
		case DkImageFormat_RGBA_BC3_sRGB:
		case DkImageFormat_RG_BC5_Unorm:
		case DkImageFormat_RG_BC5_Snorm:
			return 16;
		case DkImageFormat_None:
			throw std::runtime_error("Unsupported Deko3D image format");
		default:
			return 4;
		}
	}

	LatteTextureDK::LatteTextureDK(Latte::E_DIM dim, MPTR physical, MPTR mipPhysical,
								   Latte::E_GX2SURFFMT format, u32 width, u32 height, u32 depth, u32 pitch,
								   u32 mipLevels, u32 swizzle, Latte::E_HWTILEMODE tileMode, bool isDepth)
		: LatteTexture(dim, physical, mipPhysical, format, width, height, depth, pitch, mipLevels,
					   swizzle, tileMode, isDepth)
	{
		GetTextureFormatInfo(overwriteInfo.hasFormatOverwrite ? static_cast<Latte::E_GX2SURFFMT>(overwriteInfo.format) : format,
							 isDepth, dim, width, height, &hostFormat);
	}

	void LatteTextureDK::AllocateOnHost()
	{
		if (image)
			return;
		sint32 effectiveWidth, effectiveHeight;
		GetEffectiveSize(effectiveWidth, effectiveHeight, 0);
		const u32 effectiveDepth = std::max(1, overwriteInfo.hasResolutionOverwrite ? overwriteInfo.depth : depth);
		DkImageType type = ImageType(dim);
		if (type == DkImageType_2D && effectiveDepth > 1)
			type = DkImageType_2DArray;
		if (type == DkImageType_1D && effectiveDepth > 1)
			type = DkImageType_1DArray;
		u32 flags = 0;
		if (!IsCompressed(hostFormat.format))
			flags = DkImageFlags_UsageRender;
		image = DKImage::Create(effectiveWidth, effectiveHeight, hostFormat.format, flags,
								type, effectiveDepth, mipLevels);
		if (!image)
			throw std::runtime_error("Deko3D could not allocate a texture");
	}

	LatteTextureView* LatteTextureDK::CreateView(Latte::E_DIM dim, Latte::E_GX2SURFFMT format,
												 sint32 firstMip, sint32 mipCount, sint32 firstSlice, sint32 sliceCount)
	{
		AllocateOnHost();
		return new LatteTextureViewDK(this, dim, format, firstMip, mipCount, firstSlice, sliceCount);
	}

	LatteTextureViewDK::LatteTextureViewDK(LatteTextureDK* texture, Latte::E_DIM dim,
										   Latte::E_GX2SURFFMT format, sint32 firstMip, sint32 mipCount, sint32 firstSlice, sint32 sliceCount)
		: LatteTextureView(texture, firstMip, mipCount, firstSlice, sliceCount, dim, format)
	{
		static std::atomic<u64> nextId{1};
		m_id = nextId.fetch_add(1, std::memory_order_relaxed);
	}

	DkImageView LatteTextureViewDK::MakeView() const
	{
		auto view = Texture()->image->MakeView(firstMip, firstSlice, numSlice);
		view.mipLevelCount = numMip;
		view.type = ImageType(dim);
		if (format != baseTexture->format && !Texture()->hostFormat.isAlternateFormat)
		{
			FormatInfo info;
			GetTextureFormatInfo(format, baseTexture->isDepth, dim, 0, 0, &info);
			view.format = info.format;
		}
		return view;
	}
} // namespace Deko3D
