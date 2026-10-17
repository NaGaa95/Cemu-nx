#include "DKTexture.h"
void Deko3D::GetTextureFormatInfo(Latte::E_GX2SURFFMT format, bool isDepth, Latte::E_DIM dim, sint32 width, sint32 height, FormatInfo* formatInfoOut)
{
	formatInfoOut->isAlternateFormat = false;
	if (isDepth)
	{
		switch (format)
		{
		case Latte::E_GX2SURFFMT::D24_S8_UNORM:
			formatInfoOut->format = DkImageFormat_Z24S8;
			formatInfoOut->decoder = TextureDecoder_D24_S8::getInstance();
			break;
		case Latte::E_GX2SURFFMT::D24_S8_FLOAT:
			formatInfoOut->format = DkImageFormat_ZF32_X24S8;
			formatInfoOut->decoder = TextureDecoder_NullData64::getInstance();
			formatInfoOut->isAlternateFormat = true;
			break;
		case Latte::E_GX2SURFFMT::D32_FLOAT:
			formatInfoOut->format = DkImageFormat_ZF32;
			formatInfoOut->decoder = TextureDecoder_R32_FLOAT::getInstance();
			break;
		case Latte::E_GX2SURFFMT::D16_UNORM:
			formatInfoOut->format = DkImageFormat_Z16;
			formatInfoOut->decoder = TextureDecoder_R16_UNORM::getInstance();
			break;
		case Latte::E_GX2SURFFMT::D32_S8_FLOAT:
			formatInfoOut->format = DkImageFormat_ZF32_X24S8;
			formatInfoOut->decoder = TextureDecoder_D32_S8_UINT_X24::getInstance();
			break;
		default:
			cemuLog_log(LogType::Force, "Unsupported depth texture format {:04x}", (uint32)format);
			formatInfoOut->format = DkImageFormat_Z16;
			formatInfoOut->decoder = nullptr;
			formatInfoOut->isAlternateFormat = true;
			break;
		}
	}
	else
	{
		if (format == (Latte::E_GX2SURFFMT::R16_G16_B16_A16_FLOAT | Latte::E_GX2SURFFMT::FMT_BIT_SRGB))
			format = Latte::E_GX2SURFFMT::R16_G16_B16_A16_FLOAT;
		switch (format)
		{
		case Latte::E_GX2SURFFMT::R32_G32_B32_A32_FLOAT:
			formatInfoOut->format = DkImageFormat_RGBA32_Float;
			formatInfoOut->decoder = TextureDecoder_R32_G32_B32_A32_FLOAT::getInstance();
			break;
		case Latte::E_GX2SURFFMT::R32_G32_B32_A32_UINT:
			formatInfoOut->format = DkImageFormat_RGBA32_Uint;
			formatInfoOut->decoder = TextureDecoder_R32_G32_B32_A32_UINT::getInstance();
			break;
		case Latte::E_GX2SURFFMT::R16_G16_B16_A16_FLOAT:
			formatInfoOut->format = DkImageFormat_RGBA16_Float;
			formatInfoOut->decoder = TextureDecoder_R16_G16_B16_A16_FLOAT::getInstance();
			break;
		case Latte::E_GX2SURFFMT::R16_G16_B16_A16_UINT:
			formatInfoOut->format = DkImageFormat_RGBA16_Uint;
			formatInfoOut->decoder = TextureDecoder_R16_G16_B16_A16_UINT::getInstance();
			break;
		case Latte::E_GX2SURFFMT::R16_G16_B16_A16_UNORM:
			formatInfoOut->format = DkImageFormat_RGBA16_Unorm;
			formatInfoOut->decoder = TextureDecoder_R16_G16_B16_A16::getInstance();
			break;
		case Latte::E_GX2SURFFMT::R16_G16_B16_A16_SNORM:
			formatInfoOut->format = DkImageFormat_RGBA16_Snorm;
			formatInfoOut->decoder = TextureDecoder_R16_G16_B16_A16::getInstance();
			break;
		case Latte::E_GX2SURFFMT::R8_G8_B8_A8_UNORM:
			formatInfoOut->format = DkImageFormat_RGBA8_Unorm;
			formatInfoOut->decoder = TextureDecoder_R8_G8_B8_A8::getInstance();
			break;
		case Latte::E_GX2SURFFMT::R8_G8_B8_A8_SNORM:
			formatInfoOut->format = DkImageFormat_RGBA8_Snorm;
			formatInfoOut->decoder = TextureDecoder_R8_G8_B8_A8::getInstance();
			break;
		case Latte::E_GX2SURFFMT::R8_G8_B8_A8_SRGB:
			formatInfoOut->format = DkImageFormat_RGBA8_Unorm_sRGB;
			formatInfoOut->decoder = TextureDecoder_R8_G8_B8_A8::getInstance();
			break;
		case Latte::E_GX2SURFFMT::R8_G8_B8_A8_UINT:
			formatInfoOut->format = DkImageFormat_RGBA8_Uint;
			formatInfoOut->decoder = TextureDecoder_R8_G8_B8_A8::getInstance();
			break;
		case Latte::E_GX2SURFFMT::R8_G8_B8_A8_SINT:
			formatInfoOut->format = DkImageFormat_RGBA8_Sint;
			formatInfoOut->decoder = TextureDecoder_R8_G8_B8_A8::getInstance();
			break;
		case Latte::E_GX2SURFFMT::R32_G32_FLOAT:
			formatInfoOut->format = DkImageFormat_RG32_Float;
			formatInfoOut->decoder = TextureDecoder_R32_G32_FLOAT::getInstance();
			break;
		case Latte::E_GX2SURFFMT::R32_G32_UINT:
			formatInfoOut->format = DkImageFormat_RG32_Uint;
			formatInfoOut->decoder = TextureDecoder_R32_G32_UINT::getInstance();
			break;
		case Latte::E_GX2SURFFMT::R16_G16_UNORM:
			formatInfoOut->format = DkImageFormat_RG16_Unorm;
			formatInfoOut->decoder = TextureDecoder_R16_G16::getInstance();
			break;
		case Latte::E_GX2SURFFMT::R16_G16_FLOAT:
			formatInfoOut->format = DkImageFormat_RG16_Float;
			formatInfoOut->decoder = TextureDecoder_R16_G16_FLOAT::getInstance();
			break;
		case Latte::E_GX2SURFFMT::R8_G8_UNORM:
			formatInfoOut->format = DkImageFormat_RG8_Unorm;
			formatInfoOut->decoder = TextureDecoder_R8_G8::getInstance();
			break;
		case Latte::E_GX2SURFFMT::R8_G8_SNORM:
			formatInfoOut->format = DkImageFormat_RG8_Snorm;
			formatInfoOut->decoder = TextureDecoder_R8_G8::getInstance();
			break;
		case Latte::E_GX2SURFFMT::R4_G4_UNORM:
			formatInfoOut->format = DkImageFormat_RG8_Unorm;
			formatInfoOut->decoder = TextureDecoder_R4G4_UNORM_To_RG8::getInstance();
			formatInfoOut->isAlternateFormat = true;
			break;
		case Latte::E_GX2SURFFMT::R32_FLOAT:
			formatInfoOut->format = DkImageFormat_R32_Float;
			formatInfoOut->decoder = TextureDecoder_R32_FLOAT::getInstance();
			break;
		case Latte::E_GX2SURFFMT::R32_UINT:
			formatInfoOut->format = DkImageFormat_R32_Uint;
			formatInfoOut->decoder = TextureDecoder_R32_UINT::getInstance();
			break;
		case Latte::E_GX2SURFFMT::R16_FLOAT:
			formatInfoOut->format = DkImageFormat_R16_Float;
			formatInfoOut->decoder = TextureDecoder_R16_FLOAT::getInstance();
			break;
		case Latte::E_GX2SURFFMT::R16_UNORM:
			formatInfoOut->format = DkImageFormat_R16_Unorm;
			formatInfoOut->decoder = TextureDecoder_R16_UNORM::getInstance();
			break;
		case Latte::E_GX2SURFFMT::R16_SNORM:
			formatInfoOut->format = DkImageFormat_R16_Snorm;
			formatInfoOut->decoder = TextureDecoder_R16_SNORM::getInstance();
			break;
		case Latte::E_GX2SURFFMT::R16_UINT:
			formatInfoOut->format = DkImageFormat_R16_Uint;
			formatInfoOut->decoder = TextureDecoder_R16_UINT::getInstance();
			break;
		case Latte::E_GX2SURFFMT::R8_UNORM:
			formatInfoOut->format = DkImageFormat_R8_Unorm;
			formatInfoOut->decoder = TextureDecoder_R8::getInstance();
			break;
		case Latte::E_GX2SURFFMT::R8_SNORM:
			formatInfoOut->format = DkImageFormat_R8_Snorm;
			formatInfoOut->decoder = TextureDecoder_R8::getInstance();
			break;
		case Latte::E_GX2SURFFMT::R8_UINT:
			formatInfoOut->format = DkImageFormat_R8_Uint;
			formatInfoOut->decoder = TextureDecoder_R8_UINT::getInstance();
			break;
		case Latte::E_GX2SURFFMT::R5_G6_B5_UNORM:
			formatInfoOut->format = DkImageFormat_RGBA8_Unorm;
			formatInfoOut->decoder = TextureDecoder_R5G6B5_UNORM_To_RGBA8::getInstance();
			formatInfoOut->isAlternateFormat = true;
			break;
		case Latte::E_GX2SURFFMT::R5_G5_B5_A1_UNORM:
			formatInfoOut->format = DkImageFormat_RGBA8_Unorm;
			formatInfoOut->decoder = TextureDecoder_R5_G5_B5_A1_UNORM_swappedRB_To_RGBA8::getInstance();
			formatInfoOut->isAlternateFormat = true;
			break;
		case Latte::E_GX2SURFFMT::A1_B5_G5_R5_UNORM:
			formatInfoOut->format = DkImageFormat_RGBA8_Unorm;
			formatInfoOut->decoder = TextureDecoder_A1_B5_G5_R5_UNORM_vulkan_To_RGBA8::getInstance();
			formatInfoOut->isAlternateFormat = true;
			break;
		case Latte::E_GX2SURFFMT::R11_G11_B10_FLOAT:
			formatInfoOut->format = DkImageFormat_RG11B10_Float;
			formatInfoOut->decoder = TextureDecoder_R11_G11_B10_FLOAT::getInstance();
			break;
		case Latte::E_GX2SURFFMT::R4_G4_B4_A4_UNORM:
			formatInfoOut->format = DkImageFormat_RGBA8_Unorm;
			formatInfoOut->decoder = TextureDecoder_R4G4B4A4_UNORM_To_RGBA8::getInstance();
			formatInfoOut->isAlternateFormat = true;
			break;
		case Latte::E_GX2SURFFMT::R10_G10_B10_A2_UNORM:
			formatInfoOut->format = DkImageFormat_RGB10A2_Unorm;
			formatInfoOut->decoder = TextureDecoder_R10_G10_B10_A2_UNORM::getInstance();
			break;
		case Latte::E_GX2SURFFMT::R10_G10_B10_A2_SNORM:
			formatInfoOut->format = DkImageFormat_RGBA16_Snorm;
			formatInfoOut->decoder = TextureDecoder_R10_G10_B10_A2_SNORM_To_RGBA16::getInstance();
			formatInfoOut->isAlternateFormat = true;
			break;
		case Latte::E_GX2SURFFMT::R10_G10_B10_A2_SRGB:

			formatInfoOut->format = DkImageFormat_RGB10A2_Unorm;
			formatInfoOut->decoder = TextureDecoder_R10_G10_B10_A2_UNORM::getInstance();
			formatInfoOut->isAlternateFormat = true;
			break;
		case Latte::E_GX2SURFFMT::BC1_SRGB:
			formatInfoOut->format = DkImageFormat_RGBA_BC1_sRGB;
			formatInfoOut->decoder = TextureDecoder_BC1::getInstance();
			break;
		case Latte::E_GX2SURFFMT::BC1_UNORM:
			formatInfoOut->format = DkImageFormat_RGBA_BC1;
			formatInfoOut->decoder = TextureDecoder_BC1::getInstance();
			break;
		case Latte::E_GX2SURFFMT::BC2_UNORM:
			formatInfoOut->format = DkImageFormat_RGBA_BC2;
			formatInfoOut->decoder = TextureDecoder_BC2::getInstance();
			break;
		case Latte::E_GX2SURFFMT::BC2_SRGB:
			formatInfoOut->format = DkImageFormat_RGBA_BC2_sRGB;
			formatInfoOut->decoder = TextureDecoder_BC2::getInstance();
			break;
		case Latte::E_GX2SURFFMT::BC3_UNORM:
			formatInfoOut->format = DkImageFormat_RGBA_BC3;
			formatInfoOut->decoder = TextureDecoder_BC3::getInstance();
			break;
		case Latte::E_GX2SURFFMT::BC3_SRGB:
			formatInfoOut->format = DkImageFormat_RGBA_BC3_sRGB;
			formatInfoOut->decoder = TextureDecoder_BC3::getInstance();
			break;
		case Latte::E_GX2SURFFMT::BC4_UNORM:
			formatInfoOut->format = DkImageFormat_R_BC4_Unorm;
			formatInfoOut->decoder = TextureDecoder_BC4::getInstance();
			break;
		case Latte::E_GX2SURFFMT::BC4_SNORM:
			formatInfoOut->format = DkImageFormat_R_BC4_Snorm;
			formatInfoOut->decoder = TextureDecoder_BC4::getInstance();
			break;
		case Latte::E_GX2SURFFMT::BC5_UNORM:
			formatInfoOut->format = DkImageFormat_RG_BC5_Unorm;
			formatInfoOut->decoder = TextureDecoder_BC5::getInstance();
			break;
		case Latte::E_GX2SURFFMT::BC5_SNORM:
			formatInfoOut->format = DkImageFormat_RG_BC5_Snorm;
			formatInfoOut->decoder = TextureDecoder_BC5::getInstance();
			break;
		case Latte::E_GX2SURFFMT::R24_X8_UNORM:
			formatInfoOut->format = DkImageFormat_R32_Float;
			formatInfoOut->decoder = TextureDecoder_R24_X8::getInstance();
			formatInfoOut->isAlternateFormat = true;
			break;
		case Latte::E_GX2SURFFMT::X24_G8_UINT:
			formatInfoOut->format = DkImageFormat_RGBA8_Uint;
			formatInfoOut->decoder = TextureDecoder_X24_G8_UINT::getInstance();
			formatInfoOut->isAlternateFormat = true;
			break;
		case Latte::E_GX2SURFFMT::R32_X8_FLOAT:
			formatInfoOut->format = DkImageFormat_R32_Float;
			formatInfoOut->decoder = TextureDecoder_NullData64::getInstance();
			formatInfoOut->isAlternateFormat = true;
			break;
		default:
			cemuLog_log(LogType::Force, "Unsupported color texture format {:04x}", (uint32)format);
			cemu_assert_debug(false);
		}
	}
}
