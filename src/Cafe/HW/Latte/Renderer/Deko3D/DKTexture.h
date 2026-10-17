#pragma once

#include "DKImage.h"
#include "Cafe/HW/Latte/Core/LatteTexture.h"
#include "Cafe/HW/Latte/Core/LatteTextureLoader.h"

namespace Deko3D
{
	struct FormatInfo
	{
		DkImageFormat format = DkImageFormat_None;
		TextureDecoder* decoder = nullptr;
		bool isAlternateFormat = false;
	};

	void GetTextureFormatInfo(Latte::E_GX2SURFFMT format, bool isDepth, Latte::E_DIM dim,
							  sint32 width, sint32 height, FormatInfo* info);
	u32 FormatBytes(DkImageFormat format);
	bool IsCompressed(DkImageFormat format);
	DkImageType ImageType(Latte::E_DIM dim);

	class LatteTextureDK final : public LatteTexture
	{
	  public:
		LatteTextureDK(Latte::E_DIM dim, MPTR physical, MPTR mipPhysical, Latte::E_GX2SURFFMT format,
					   u32 width, u32 height, u32 depth, u32 pitch, u32 mipLevels, u32 swizzle,
					   Latte::E_HWTILEMODE tileMode, bool isDepth);
		void AllocateOnHost() override;
		std::unique_ptr<DKImage> image;
		FormatInfo hostFormat;
		u64 lastWrite = 0;
		u64 lastRead = 0;

	  protected:
		LatteTextureView* CreateView(Latte::E_DIM dim, Latte::E_GX2SURFFMT format,
									 sint32 firstMip, sint32 mipCount, sint32 firstSlice, sint32 sliceCount) override;
	};

	class LatteTextureViewDK final : public LatteTextureView
	{
	  public:
		LatteTextureViewDK(LatteTextureDK* texture, Latte::E_DIM dim, Latte::E_GX2SURFFMT format,
						   sint32 firstMip, sint32 mipCount, sint32 firstSlice, sint32 sliceCount);
		DkImageView MakeView() const;
		u64 Id() const { return m_id; }
		std::unique_ptr<DKImage> renderImage;
		LatteTextureDK* Texture() const
		{
			return static_cast<LatteTextureDK*>(baseTexture);
		}
	private:
		u64 m_id;
	};
} // namespace Deko3D
