#include "DKRendererInternal.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"

using namespace Deko3D;

namespace
{
	u32 AdjustSwizzle(Latte::E_GX2SURFFMT format, u32 component)
	{
		using F = Latte::E_GX2SURFFMT;
		switch (format)
		{
		case F::R8_UNORM:
		case F::R8_SNORM:
		case F::BC4_UNORM:
		case F::BC4_SNORM:
			return component < 4 ? 0 : component;
		case F::A1_B5_G5_R5_UNORM:
		case F::A2_B10_G10_R10_UNORM:
			return component < 4 ? 3 - component : component;
		case F::BC5_UNORM:
		case F::BC5_SNORM:
			return component == 3 ? 1 : component;
		case F::X24_G8_UINT:
			return component < 4 ? 3 : component;
		case F::R4_G4_UNORM:
			return component < 2 ? 1 - component : component;
		default:
			return component;
		}
	}

	std::array<u32, 16> TextureKey(const LatteDecompilerShader& shader, const LatteTextureViewDK* texture, u32 unit, u32 word4)
	{
		std::array<u32, 16> key{};
		key[3] = static_cast<u32>(shader.textureUnitDim[unit]);
		if (!texture)
			return key;
		key[0] = static_cast<u32>(texture->Id());
		key[1] = static_cast<u32>(texture->Id() >> 32);
		key[2] = word4 & 0x0fff0000;
		key[4] = shader.textureUsesDepthCompare[unit];
		const u32 assignment = shader.textureUnitSamplerAssignment[unit];
		if (assignment == LATTE_DECOMPILER_SAMPLER_NONE)
			return key;
		const u32 index = assignment + LatteDecompiler_getTextureSamplerBaseIndex(shader.shaderType);
		std::memcpy(key.data() + 5, LatteGPUState.contextRegister + Latte::REGADDR::SQ_TEX_SAMPLER_WORD0_0 + index * 3, 3 * sizeof(u32));
		const auto& ctx = LatteGPUState.contextNew;
		const auto border = ctx.SQ_TEX_SAMPLER[index].WORD0.get_BORDER_COLOR_TYPE();
		using Border = Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_BORDER_COLOR_TYPE;
		if (border != Border::TRANSPARENT_BLACK && border != Border::OPAQUE_BLACK && border != Border::OPAQUE_WHITE)
		{
			const auto* colors = shader.shaderType == LatteConst::ShaderType::Vertex ? ctx.TD_VS_SAMPLER_BORDER_COLOR :
				shader.shaderType == LatteConst::ShaderType::Pixel ? ctx.TD_PS_SAMPLER_BORDER_COLOR : ctx.TD_GS_SAMPLER_BORDER_COLOR;
			key[8] = std::bit_cast<u32>(colors[assignment].red.get_channelValue());
			key[9] = std::bit_cast<u32>(colors[assignment].green.get_channelValue());
			key[10] = std::bit_cast<u32>(colors[assignment].blue.get_channelValue());
			key[11] = std::bit_cast<u32>(colors[assignment].alpha.get_channelValue());
		}
		const auto& overwrite = texture->baseTexture->overwriteInfo;
		key[12] = overwrite.hasLodBias ? static_cast<u32>(overwrite.lodBias) : 0;
		key[13] = overwrite.hasRelativeLodBias ? static_cast<u32>(overwrite.relativeLodBias) : 0;
		key[14] = static_cast<u32>(overwrite.anisotropicLevel);
		key[15] = 1 | (static_cast<u32>(overwrite.hasLodBias) << 1) | (static_cast<u32>(overwrite.hasRelativeLodBias) << 2);
		return key;
	}
} // namespace

void Deko3DRenderer::State::BindTextures(LatteDecompilerShader* shader, StageState& stage)
{
	const u32 bases[]{LATTE_CEMU_VS_TEX_UNIT_BASE, LATTE_CEMU_PS_TEX_UNIT_BASE, LATTE_CEMU_GS_TEX_UNIT_BASE};
	const u32 registers[]{Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS, Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS,
						  Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_GS};
	const u32 stageIndex = StageIndex(shader->shaderType);
	stage.textureCount = 0;
	for (u32 i = 0; i < shader->textureUnitListCount; ++i)
	{
		const u32 unit = shader->textureUnitList[i];
		const sint32 binding = shader->resourceMapping.textureUnitToBindingPoint[unit];
		if (binding < 0 || stage.program->bindings.textures[binding] < 0)
			continue;
		auto* texture = textures[bases[stageIndex] + unit];
		const auto dim = shader->textureUnitDim[unit];
		if (texture && ((dim == Latte::E_DIM::DIM_1D && texture->dim != Latte::E_DIM::DIM_1D) ||
						(dim == Latte::E_DIM::DIM_2D && texture->dim != Latte::E_DIM::DIM_2D && texture->dim != Latte::E_DIM::DIM_2D_MSAA)))
		{
			texture = nullptr;
		}
		DkImageView volumeView{};
		const bool volume = texture && texture->Texture()->Is3DTexture() && dim != Latte::E_DIM::DIM_3D;
		if (texture)
		{
			TextureBarrier(*texture->Texture(), false);
			if (volume)
				volumeView = RenderView(*texture);
		}
		const u32 word4 = LatteGPUState.contextRegister[registers[stageIndex] + unit * 7 + 4];
		const auto key = TextureKey(*shader, texture, unit, word4);
		auto& cached = stage.textureBindings[unit];
		if (!cached.Matches(key, samplerGeneration))
		{
			DkImageView view;
			DkSampler sampler;
			dkSamplerDefaults(&sampler);
			if (texture)
			{
				view = volume ? volumeView : texture->MakeView();
				static constexpr DkImageSwizzle swizzle[]{DkImageSwizzle_Red, DkImageSwizzle_Green,
														  DkImageSwizzle_Blue, DkImageSwizzle_Alpha, DkImageSwizzle_Zero, DkImageSwizzle_One,
														  DkImageSwizzle_Zero, DkImageSwizzle_Zero};
				for (u32 c = 0; c < 4; ++c)
					view.swizzle[c] = swizzle[AdjustSwizzle(texture->format, (word4 >> (16 + 3 * c)) & 7)];
				const u32 assignment = shader->textureUnitSamplerAssignment[unit];
				if (assignment != LATTE_DECOMPILER_SAMPLER_NONE)
				{
					const auto& words = LatteGPUState.contextNew.SQ_TEX_SAMPLER[assignment + LatteDecompiler_getTextureSamplerBaseIndex(shader->shaderType)];
					const auto& overwrite = texture->baseTexture->overwriteInfo;
					const u32 mip = static_cast<u32>(words.WORD0.get_MIP_FILTER());
					sampler.mipFilter = mip == 0 ? DkMipFilter_None : mip == 1 ? DkMipFilter_Nearest
																			   : DkMipFilter_Linear;
					sampler.lodClampMin = mip ? words.WORD1.get_MIN_LOD() / 64.0f : 0;
					sampler.lodClampMax = mip ? words.WORD1.get_MAX_LOD() / 64.0f : 0;
					sint32 bias = words.WORD1.get_LOD_BIAS();
					if (overwrite.hasRelativeLodBias)
						bias += overwrite.relativeLodBias;
					if (overwrite.hasLodBias)
						bias = overwrite.lodBias;
					sampler.lodBias = bias / 64.0f;
					using Filter = Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER;
					const auto min = words.WORD0.get_XY_MIN_FILTER();
					const auto mag = words.WORD0.get_XY_MAG_FILTER();
					sampler.minFilter = min == Filter::POINT || min == Filter::ANISO_POINT ? DkFilter_Nearest : DkFilter_Linear;
					sampler.magFilter = mag == Filter::POINT || mag == Filter::ANISO_POINT ? DkFilter_Nearest : DkFilter_Linear;
					static constexpr DkWrapMode wraps[]{DkWrapMode_Repeat, DkWrapMode_MirroredRepeat,
														DkWrapMode_ClampToEdge, DkWrapMode_MirrorClampToEdge, DkWrapMode_Clamp,
														DkWrapMode_MirrorClamp, DkWrapMode_ClampToBorder, DkWrapMode_MirrorClampToBorder};
					sampler.wrapMode[0] = wraps[static_cast<u32>(words.WORD0.get_CLAMP_X())];
					sampler.wrapMode[1] = wraps[static_cast<u32>(words.WORD0.get_CLAMP_Y())];
					sampler.wrapMode[2] = wraps[static_cast<u32>(words.WORD0.get_CLAMP_Z())];
					const u32 anisotropy = overwrite.anisotropicLevel >= 0 ? overwrite.anisotropicLevel : words.WORD0.get_MAX_ANISO_RATIO();
					sampler.maxAnisotropy = static_cast<float>(1u << std::min(anisotropy, 4u));
					sampler.compareEnable = shader->textureUsesDepthCompare[unit];
					sampler.compareOp = CompareOperation(static_cast<u32>(words.WORD0.get_DEPTH_COMPARE_FUNCTION()));
					using Border = Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_BORDER_COLOR_TYPE;
					const auto border = words.WORD0.get_BORDER_COLOR_TYPE();
					if (border == Border::OPAQUE_BLACK || border == Border::OPAQUE_WHITE)
						sampler.borderColor[3].value_f = 1;
					if (border == Border::OPAQUE_WHITE)
						for (u32 c = 0; c < 3; ++c)
							sampler.borderColor[c].value_f = 1;
					if (border != Border::TRANSPARENT_BLACK && border != Border::OPAQUE_BLACK && border != Border::OPAQUE_WHITE)
					{
						const auto& ctx = LatteGPUState.contextNew;
						const auto* colors = stageIndex == 0 ? ctx.TD_VS_SAMPLER_BORDER_COLOR : stageIndex == 1 ? ctx.TD_PS_SAMPLER_BORDER_COLOR
																												: ctx.TD_GS_SAMPLER_BORDER_COLOR;
						sampler.borderColor[0].value_f = colors[assignment].red.get_channelValue();
						sampler.borderColor[1].value_f = colors[assignment].green.get_channelValue();
						sampler.borderColor[2].value_f = colors[assignment].blue.get_channelValue();
						sampler.borderColor[3].value_f = colors[assignment].alpha.get_channelValue();
					}
				}
			}
			else
			{
				const u32 index = dim == Latte::E_DIM::DIM_1D ? 0 : dim == Latte::E_DIM::DIM_3D		? 2
																: dim == Latte::E_DIM::DIM_2D_ARRAY ? 3
																: dim == Latte::E_DIM::DIM_CUBEMAP	? 4
																									: 1;
				dkImageViewDefaults(&view, &nullTextures[index]->GetImage());
			}
			cached.sampler = Sampler(sampler);
			dkImageDescriptorInitialize(&cached.image, &view, false, false);
			cached.key = key;
			cached.samplerGeneration = samplerGeneration;
			cached.descriptorFence = 0;
			cached.valid = true;
		}
		if (cached.descriptorFence != g_dk_command_buffer_mgr->GetCurrentFenceCounter())
		{
			cached.handle = TextureHandle(cached.image, cached.sampler);
			cached.descriptorFence = g_dk_command_buffer_mgr->GetCurrentFenceCounter();
		}
		stage.textures[stage.program->bindings.textures[binding]] = cached.handle;
		stage.textureCount = std::max(stage.textureCount, static_cast<u32>(stage.program->bindings.textures[binding] + 1));
	}
}

void Deko3DRenderer::State::PrepareTextures(bool first)
{
	LatteDecompilerShader* shaders[]{LatteSHRC_GetActiveVertexShader(), LatteSHRC_GetActivePixelShader(), LatteSHRC_GetActiveGeometryShader()};
	if (first || dirty || LatteGPUState.requiresTextureBarrier)
	{
		if (samplerCache.size() + LATTE_NUM_MAX_TEX_UNITS * 3 >= NUM_SAMPLER_DESCRIPTORS)
			ResetSamplers();
		for (u32 i = 0; i < stages.size(); ++i)
			if (shaders[i] && stages[i].program)
				BindTextures(shaders[i], stages[i]);
	}
}

void Deko3DRenderer::State::BindResources(bool first, const LatteDrawcallContext&)
{
	LatteDecompilerShader* shaders[]{LatteSHRC_GetActiveVertexShader(), LatteSHRC_GetActivePixelShader(), LatteSHRC_GetActiveGeometryShader()};
	const bool rebind = first || dirty || LatteGPUState.requiresTextureBarrier;
	FlushDescriptors();
	for (u32 i = 0; i < stages.size(); ++i)
	{
		const auto* shader = shaders[i];
		auto& state = stages[i];
		if (!shader || !state.program)
			continue;
		const auto& mapping = shader->resourceMapping;
		const auto& bindings = state.program->bindings;
		const auto stage = ShaderStage(shader->shaderType);
		auto bindUniform = [&](u32 slot, DkBufExtents buffer) {
			auto& previous = state.boundUniforms[slot];
			if (resourcesInvalid || previous.addr != buffer.addr || previous.size != buffer.size)
			{
				dkCmdBufBindUniformBuffers(Cmd(), stage, slot, &buffer, 1);
				previous = buffer;
			}
		};
		for (u32 bank = 0; bank < state.banks.size(); ++bank)
		{
			const sint32 old = mapping.uniformBuffersBindingPoint[bank];
			if (old >= 0 && bindings.uniforms[old] >= 0)
			{
				const DkBufExtents buffer = state.banks[bank].size ? state.banks[bank] : DkBufExtents{zeroBuffer.getGpuAddr(), 16};
				bindUniform(bindings.uniforms[old], buffer);
			}
		}
		const sint32 old = mapping.uniformVarsBufferBindingPoint;
		if (old >= 0 && bindings.uniforms[old] >= 0)
			bindUniform(bindings.uniforms[old], state.variableBuffer);
		const sint32 tf = mapping.tfStorageBindingPoint;
		if (rebind && tf >= 0 && bindings.storage[tf] >= 0 && streamout)
		{
			const DkBufExtents buffer{streamout.getGpuAddr(), streamout.getSize()};
			dkCmdBufBindStorageBuffers(Cmd(), stage, bindings.storage[tf], &buffer, 1);
		}
		if (rebind && state.textureCount && Changed(static_cast<StateSlot>(static_cast<u32>(StateSlot::TexturesVS) + i),
			std::span<const DkResHandle>(state.textures.data(), state.textureCount)))
			dkCmdBufBindTextures(Cmd(), stage, 0, state.textures.data(), state.textureCount);
	}
}

void Deko3DRenderer::State::UpdateUniforms(LatteDecompilerShader* shader, StageState& stage, bool full, bool aluDirty, uint32 banksDirty)
{
	full = full || stage.uniformShader != shader || stage.variables.size() != shader->uniform.uniformRangeSize;
	stage.uniformShader = shader;
	if (stage.variables.size() != shader->uniform.uniformRangeSize)
		stage.variables.resize(shader->uniform.uniformRangeSize);
	auto GET_UNIFORM_DATA_PTR = [&stage](size_t index) {
		return reinterpret_cast<float*>(stage.variables.data() + index);
	};
	if (shader->resourceMapping.uniformVarsBufferBindingPoint < 0)
		return;
	bool changed = full;
	if (full && !shader->uniform.list_ufTexRescale.empty())
	{
		for (auto& entry : shader->uniform.list_ufTexRescale)
		{
			float* xyScale = LatteTexture_getEffectiveTextureScale(shader->shaderType, entry.texUnit);
			memcpy(entry.currentValue, xyScale, sizeof(float) * 2);
			memcpy(GET_UNIFORM_DATA_PTR(entry.uniformLocation), xyScale, sizeof(float) * 2);
		}
	}
	if (full && shader->uniform.loc_alphaTestRef >= 0)
	{
		*GET_UNIFORM_DATA_PTR(shader->uniform.loc_alphaTestRef) = LatteGPUState.contextNew.SX_ALPHA_REF.get_ALPHA_TEST_REF();
	}
	if (full && shader->uniform.loc_pointSize >= 0)
	{
		const auto& pointSizeReg = LatteGPUState.contextNew.PA_SU_POINT_SIZE;
		float pointWidth = (float)pointSizeReg.get_WIDTH() / 8.0f;
		if (pointWidth == 0.0f)
			pointWidth = 1.0f / 8.0f;
		*GET_UNIFORM_DATA_PTR(shader->uniform.loc_pointSize) = pointWidth;
	}
	if (shader->uniform.loc_remapped >= 0 && (full || aluDirty || banksDirty))
	{
		changed |= LatteBufferCache_LoadRemappedUniforms(shader, GET_UNIFORM_DATA_PTR(shader->uniform.loc_remapped), full || aluDirty, full ? ~0u : banksDirty);
	}
	if (shader->uniform.loc_uniformRegister >= 0 && (full || aluDirty))
	{
		sint32 shaderAluConst;
		switch (shader->shaderType)
		{
		case LatteConst::ShaderType::Vertex:
			shaderAluConst = 0x400;
			break;
		case LatteConst::ShaderType::Pixel:
			shaderAluConst = 0;
			break;
		case LatteConst::ShaderType::Geometry:
			shaderAluConst = 0;
			break;
		default:
			UNREACHABLE;
		}
		uint32* uniformRegData = (uint32*)(LatteGPUState.contextRegister + mmSQ_ALU_CONSTANT0_0 + shaderAluConst);
		memcpy(GET_UNIFORM_DATA_PTR(shader->uniform.loc_uniformRegister), uniformRegData, shader->uniform.count_uniformRegister * 16);
		changed = true;
	}
	if (full && shader->uniform.loc_windowSpaceToClipSpaceTransform >= 0)
	{
		sint32 viewportWidth;
		sint32 viewportHeight;
		LatteRenderTarget_GetCurrentVirtualViewportSize(&viewportWidth, &viewportHeight);
		float* v = GET_UNIFORM_DATA_PTR(shader->uniform.loc_windowSpaceToClipSpaceTransform);
		v[0] = 2.0f / (float)viewportWidth;
		v[1] = 2.0f / (float)viewportHeight;
	}
	if (full && shader->uniform.loc_fragCoordScale >= 0)
	{
		LatteMRT::GetCurrentFragCoordScale(GET_UNIFORM_DATA_PTR(shader->uniform.loc_fragCoordScale));
	}
	if (shader->uniform.loc_verticesPerInstance >= 0)
	{
		auto* vertices = reinterpret_cast<uint32*>(GET_UNIFORM_DATA_PTR(shader->uniform.loc_verticesPerInstance));
		changed |= *vertices != verticesPerInstance;
		*vertices = verticesPerInstance;
		for (sint32 b = 0; b < LATTE_NUM_STREAMOUT_BUFFER; b++)
		{
			if (shader->uniform.loc_streamoutBufferBase[b] >= 0)
			{
				auto* offset = reinterpret_cast<uint32*>(GET_UNIFORM_DATA_PTR(shader->uniform.loc_streamoutBufferBase[b]));
				changed |= *offset != streamoutOffsets[b];
				*offset = streamoutOffsets[b];
			}
		}
	}
	if (changed)
		stage.variablesDirty = stage.variableFence != g_dk_command_buffer_mgr->GetCurrentFenceCounter() ||
			stage.uploadedVariables != stage.variables;
}
