// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DKRendererInternal.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "config/CemuConfig.h"

using namespace Deko3D;

void Deko3D::SetViewport(DkCmdBuf command, const DkViewport& viewport)
{
	DkViewport normalized = viewport;
	normalized.x += std::min(0.0f, viewport.width);
	normalized.y += std::min(0.0f, viewport.height);
	normalized.width = std::abs(viewport.width);
	normalized.height = std::abs(viewport.height);
	const DkViewportSwizzle swizzle{viewport.width < 0 ? DkSwizzle_NegativeX : DkSwizzle_PositiveX,
									viewport.height < 0 ? DkSwizzle_PositiveY : DkSwizzle_NegativeY, DkSwizzle_PositiveZ, DkSwizzle_PositiveW};
	dkCmdBufSetViewportSwizzles(command, 0, &swizzle, 1);
	dkCmdBufSetViewports(command, 0, &normalized, 1);
}

namespace
{
	DkVtxAttribSize VertexFormat(Latte::E_HWFMT format)
	{
		using F = Latte::E_HWFMT;
		switch (format)
		{
		case F::HWFMT_8:
			return DkVtxAttribSize_1x8;
		case F::HWFMT_8_8:
			return DkVtxAttribSize_2x8;
		case F::HWFMT_8_8_8:
			return DkVtxAttribSize_3x8;
		case F::HWFMT_8_8_8_8:
			return DkVtxAttribSize_4x8;
		case F::HWFMT_16:
		case F::HWFMT_16_FLOAT:
			return DkVtxAttribSize_1x16;
		case F::HWFMT_16_16:
		case F::HWFMT_16_16_FLOAT:
			return DkVtxAttribSize_2x16;
		case F::HWFMT_16_16_16:
		case F::HWFMT_16_16_16_FLOAT:
			return DkVtxAttribSize_3x16;
		case F::HWFMT_16_16_16_16:
		case F::HWFMT_16_16_16_16_FLOAT:
			return DkVtxAttribSize_4x16;
		case F::HWFMT_32:
		case F::HWFMT_32_FLOAT:
		case F::HWFMT_2_10_10_10:
			return DkVtxAttribSize_1x32;
		case F::HWFMT_32_32:
		case F::HWFMT_32_32_FLOAT:
			return DkVtxAttribSize_2x32;
		case F::HWFMT_32_32_32:
		case F::HWFMT_32_32_32_FLOAT:
			return DkVtxAttribSize_3x32;
		case F::HWFMT_32_32_32_32:
		case F::HWFMT_32_32_32_32_FLOAT:
			return DkVtxAttribSize_4x32;
		default:
			throw std::runtime_error(fmt::format("Unsupported Deko3D vertex format: {:x}", static_cast<u32>(format)));
		}
	}

	DkBlendOp BlendOperation(u32 op)
	{
		static constexpr DkBlendOp operations[]{DkBlendOp_Add, DkBlendOp_Sub, DkBlendOp_Min,
												DkBlendOp_Max, DkBlendOp_RevSub};
		return op < std::size(operations) ? operations[op] : DkBlendOp_Add;
	}

	DkBlendFactor BlendFactor(u32 factor)
	{
		static constexpr DkBlendFactor factors[]{DkBlendFactor_Zero, DkBlendFactor_One,
												 DkBlendFactor_SrcColor, DkBlendFactor_InvSrcColor, DkBlendFactor_SrcAlpha,
												 DkBlendFactor_InvSrcAlpha, DkBlendFactor_DstAlpha, DkBlendFactor_InvDstAlpha,
												 DkBlendFactor_DstColor, DkBlendFactor_InvDstColor, DkBlendFactor_SrcAlphaSaturate,
												 DkBlendFactor_Zero, DkBlendFactor_Zero, DkBlendFactor_ConstColor, DkBlendFactor_InvConstColor,
												 DkBlendFactor_Src1Color, DkBlendFactor_InvSrc1Color, DkBlendFactor_Src1Alpha,
												 DkBlendFactor_InvSrc1Alpha, DkBlendFactor_ConstAlpha, DkBlendFactor_InvConstAlpha};
		return factor < std::size(factors) ? factors[factor] : DkBlendFactor_Zero;
	}

	bool IntegerFormat(DkImageFormat format)
	{
		switch (format)
		{
		case DkImageFormat_R8_Uint:
		case DkImageFormat_R8_Sint:
		case DkImageFormat_RG8_Uint:
		case DkImageFormat_RG8_Sint:
		case DkImageFormat_RGBA8_Uint:
		case DkImageFormat_RGBA8_Sint:
		case DkImageFormat_R16_Uint:
		case DkImageFormat_R16_Sint:
		case DkImageFormat_RG16_Uint:
		case DkImageFormat_RG16_Sint:
		case DkImageFormat_RGBA16_Uint:
		case DkImageFormat_RGBA16_Sint:
		case DkImageFormat_R32_Uint:
		case DkImageFormat_R32_Sint:
		case DkImageFormat_RG32_Uint:
		case DkImageFormat_RG32_Sint:
		case DkImageFormat_RGBA32_Uint:
		case DkImageFormat_RGBA32_Sint:
			return true;
		default:
			return false;
		}
	}
} // namespace

void Deko3DRenderer::renderTarget_setViewport(float x, float y, float width, float height,
											  float nearZ, float farZ, bool)
{
	m_state->viewport = {x, y + height, width, -height, nearZ, farZ};
	m_state->dirty = true;
}

void Deko3DRenderer::renderTarget_setScissor(sint32 x, sint32 y, sint32 width, sint32 height)
{
	const sint32 right = std::max(0, x + width);
	const sint32 bottom = std::max(0, y + height);
	x = std::clamp(x, 0, right);
	y = std::clamp(y, 0, bottom);
	m_state->scissor = {static_cast<u32>(x), static_cast<u32>(y),
						static_cast<u32>(right - x), static_cast<u32>(bottom - y)};
	m_state->dirty = true;
}

void Deko3DRenderer::State::BindShaders(bool bind)
{
	LatteDecompilerShader* shaders[]{LatteSHRC_GetActiveVertexShader(), LatteSHRC_GetActivePixelShader(),
									 LatteSHRC_GetActiveGeometryShader()};
	std::array<const DkShader*, 3> native{};
	u32 count = 0;
	for (u32 i = 0; i < stages.size(); ++i)
	{
		if (shaders[i] && GetConfig().async_compile.GetValue() && !shaders[i]->shader->IsCompiled())
		{
			skipSequence = true;
			return;
		}
		stages[i].program = shaders[i] ? static_cast<RendererShaderDK*>(shaders[i]->shader)->GetProgram() : nullptr;
		if (shaders[i] && !stages[i].program)
		{
			skipSequence = true;
			return;
		}
		if (stages[i].program)
			native[count++] = &stages[i].program->shader;
	}
	if (!bind)
		return;
	for (const auto& stage : stages)
		if (stage.program)
			stage.program->code->PrepareForUse(Cmd());
	if (!shaders[2] && shaders[0] && LatteGPUState.contextNew.VGT_PRIMITIVE_TYPE.get_PRIMITIVE_MODE() == LattePrimitiveMode::RECTS)
	{
		const auto* rect = RectangleShader(shaders[0]);
		if (!rect)
		{
			skipSequence = true;
			return;
		}
		native[count++] = &rect->shader;
		rect->code->PrepareForUse(Cmd());
	}
	std::array<DkShader, 3> key{};
	for (u32 i = 0; i < count; ++i)
		key[i] = *native[i];
	if (Changed(StateSlot::Shaders, key))
		dkCmdBufBindShaders(Cmd(), DkStageFlag_GraphicsMask, native.data(), count);
}

void Deko3DRenderer::State::BindVertexBuffers(std::span<const DkBufExtents> buffers)
{
	constexpr u32 wordsPerBuffer = 6;
	if (buffers.size() > DK_MAX_VERTEX_BUFFERS)
		throw std::invalid_argument("Too many Deko3D vertex buffers");
	std::array<u64, DK_MAX_VERTEX_BUFFERS * 2> key{};
	for (u32 i = 0; i < buffers.size(); ++i)
	{
		key[i * 2] = buffers[i].addr;
		key[i * 2 + 1] = buffers[i].size;
	}
	if (!Changed(StateSlot::VertexBuffers, std::span<const u64>(key.data(), buffers.size() * 2)))
		return;
	if (!stateCapture)
	{
		stateCapture = dk::CmdBufMaker{g_dk_context->GetDevice()}.create();
		if (!stateCapture)
			throw std::bad_alloc();
	}
	std::array<u32, DK_MAX_VERTEX_BUFFERS * wordsPerBuffer + 1> commands;
	dkCmdBufBeginCaptureCmds(stateCapture, commands.data(), commands.size());
	dkCmdBufBindVtxBuffers(stateCapture, 0, buffers.data(), buffers.size());
	const u32 count = dkCmdBufEndCaptureCmds(stateCapture);
	if (count != buffers.size() * wordsPerBuffer)
		throw std::runtime_error("Unexpected Deko3D vertex buffer command layout");
	dkCmdBufReplayCmds(Cmd(), commands.data(), count);
}

void Deko3DRenderer::State::BindBlendStates(std::span<const DkBlendState> states)
{
	constexpr u32 wordsPerState = 7;
	if (states.size() > DK_MAX_RENDER_TARGETS)
		throw std::invalid_argument("Too many Deko3D blend targets");
	if (!Changed(StateSlot::Blend, states))
		return;
	if (!stateCapture)
	{
		stateCapture = dk::CmdBufMaker{g_dk_context->GetDevice()}.create();
		if (!stateCapture)
			throw std::bad_alloc();
	}
	std::array<u32, DK_MAX_RENDER_TARGETS * wordsPerState + 1> commands;
	dkCmdBufBeginCaptureCmds(stateCapture, commands.data(), commands.size());
	dkCmdBufBindBlendStates(stateCapture, 0, states.data(), states.size());
	const u32 count = dkCmdBufEndCaptureCmds(stateCapture);
	if (count != states.size() * wordsPerState)
		throw std::runtime_error("Unexpected Deko3D blend command layout");
	for (u32 i = 0; i < states.size(); ++i)
	{
		const u32 factor = static_cast<u32>(states[i].dstAlphaBlendFactor);
		commands[i * wordsPerState + 6] = (factor & 0x1f) | ((factor & 0x20) ? 0xc000 : 0);
	}
	dkCmdBufReplayCmds(Cmd(), commands.data(), count);
}

void Deko3DRenderer::State::BindPipeline()
{
	const auto& ctx = LatteGPUState.contextNew;
	const auto& poly = ctx.PA_SU_SC_MODE_CNTL;
	DkRasterizerState raster{};
	dkRasterizerStateDefaults(&raster);
	raster.rasterizerEnable = !ctx.PA_CL_CLIP_CNTL.get_DX_RASTERIZATION_KILL() || !ctx.PA_CL_VTE_CNTL.get_VPORT_X_OFFSET_ENA();
	raster.depthClampEnable = ctx.PA_CL_CLIP_CNTL.get_ZCLIP_FAR_DISABLE();
	raster.cullMode = static_cast<DkFace>((poly.get_CULL_FRONT() ? DkFace_Front : 0) | (poly.get_CULL_BACK() ? DkFace_Back : 0));
	raster.frontFace = poly.get_FRONT_FACE() == Latte::LATTE_PA_SU_SC_MODE_CNTL::E_FRONTFACE::CCW ? DkFrontFace_CCW : DkFrontFace_CW;
	raster.provokingVertex = DkProvokingVertex_First;
	raster.depthBiasEnableMask = poly.get_OFFSET_FRONT_ENABLED() ? DkPolygonFlag_All : 0;
	if (Changed(StateSlot::Rasterizer, raster))
		dkCmdBufBindRasterizerState(Cmd(), &raster);
	DkMultisampleState multisample{};
	dkMultisampleStateDefaults(&multisample);
	if (Changed(StateSlot::Multisample, multisample))
		dkCmdBufBindMultisampleState(Cmd(), &multisample);
	if (Changed(StateSlot::Static, true))
	{
		dkCmdBufSetSampleMask(Cmd(), 0xffff);
		dkCmdBufSetLineWidth(Cmd(), 1.0f);
		dkCmdBufSetDepthBounds(Cmd(), false, 0, 1);
	}
	const std::array<float, 3> bias{ctx.PA_SU_POLY_OFFSET_FRONT_OFFSET.get_OFFSET(),
		ctx.PA_SU_POLY_OFFSET_CLAMP.get_CLAMP(), ctx.PA_SU_POLY_OFFSET_FRONT_SCALE.get_SCALE() / 16.0f};
	if (Changed(StateSlot::DepthBias, bias))
		dkCmdBufSetDepthBias(Cmd(), bias[0], bias[1], bias[2]);
	const float pointSize = std::max(0.125f, ctx.PA_SU_POINT_SIZE.get_WIDTH() / 8.0f);
	if (Changed(StateSlot::PointSize, pointSize))
		dkCmdBufSetPointSize(Cmd(), pointSize);
	DkColorState color{};
	dkColorStateDefaults(&color);
	color.blendEnableMask = ctx.CB_COLOR_CONTROL.get_BLEND_MASK();
	switch (ctx.CB_COLOR_CONTROL.get_ROP())
	{
	case Latte::LATTE_CB_COLOR_CONTROL::E_LOGICOP::CLEAR:
		color.logicOp = DkLogicOp_Clear;
		break;
	case Latte::LATTE_CB_COLOR_CONTROL::E_LOGICOP::OR:
		color.logicOp = DkLogicOp_Or;
		break;
	case Latte::LATTE_CB_COLOR_CONTROL::E_LOGICOP::SET:
		color.logicOp = DkLogicOp_Set;
		break;
	default:
		color.logicOp = DkLogicOp_Copy;
		break;
	}
	std::array<DkBlendState, 8> blends{};
	for (u32 i = 0; i < blends.size(); ++i)
	{
		const auto& reg = ctx.CB_BLENDN_CONTROL[i];
		auto& blend = blends[i];
		blend.colorBlendOp = BlendOperation(static_cast<u32>(reg.get_COLOR_COMB_FCN()));
		blend.srcColorBlendFactor = BlendFactor(static_cast<u32>(reg.get_COLOR_SRCBLEND()));
		blend.dstColorBlendFactor = BlendFactor(static_cast<u32>(reg.get_COLOR_DSTBLEND()));
		blend.alphaBlendOp = reg.get_SEPARATE_ALPHA_BLEND() ? BlendOperation(static_cast<u32>(reg.get_ALPHA_COMB_FCN())) : blend.colorBlendOp;
		blend.srcAlphaBlendFactor = reg.get_SEPARATE_ALPHA_BLEND() ? BlendFactor(static_cast<u32>(reg.get_ALPHA_SRCBLEND())) : blend.srcColorBlendFactor;
		blend.dstAlphaBlendFactor = reg.get_SEPARATE_ALPHA_BLEND() ? BlendFactor(static_cast<u32>(reg.get_ALPHA_DSTBLEND())) : blend.dstColorBlendFactor;
		if (framebuffer && framebuffer->colorBuffer[i].texture && IntegerFormat(static_cast<LatteTextureViewDK*>(framebuffer->colorBuffer[i].texture)->Texture()->hostFormat.format))
			color.blendEnableMask &= ~(1 << i);
	}
	const DkColorWriteState write{ctx.CB_TARGET_MASK.get_MASK() & colorTargetMask};
	if (Changed(StateSlot::Color, color))
		dkCmdBufBindColorState(Cmd(), &color);
	if (Changed(StateSlot::ColorWrite, write))
		dkCmdBufBindColorWriteState(Cmd(), &write);
	BindBlendStates(blends);
	const std::array<float, 4> blendConst{ctx.CB_BLEND_RED.get_RED(), ctx.CB_BLEND_GREEN.get_GREEN(),
		ctx.CB_BLEND_BLUE.get_BLUE(), ctx.CB_BLEND_ALPHA.get_ALPHA()};
	if (Changed(StateSlot::BlendConst, blendConst))
		dkCmdBufSetBlendConst(Cmd(), blendConst[0], blendConst[1], blendConst[2], blendConst[3]);

	const auto& reg = ctx.DB_DEPTH_CONTROL;
	DkDepthStencilState ds{};
	dkDepthStencilStateDefaults(&ds);
	ds.depthTestEnable = reg.get_Z_ENABLE();
	ds.depthWriteEnable = reg.get_Z_WRITE_ENABLE();
	ds.depthCompareOp = CompareOperation(static_cast<u32>(reg.get_Z_FUNC()));
	ds.stencilTestEnable = reg.get_STENCIL_ENABLE();
	ds.stencilFrontCompareOp = CompareOperation(static_cast<u32>(reg.get_STENCIL_FUNC_F()));
	ds.stencilFrontFailOp = static_cast<DkStencilOp>(static_cast<u32>(reg.get_STENCIL_FAIL_F()) + 1);
	ds.stencilFrontPassOp = static_cast<DkStencilOp>(static_cast<u32>(reg.get_STENCIL_ZPASS_F()) + 1);
	ds.stencilFrontDepthFailOp = static_cast<DkStencilOp>(static_cast<u32>(reg.get_STENCIL_ZFAIL_F()) + 1);
	const bool back = reg.get_BACK_STENCIL_ENABLE();
	ds.stencilBackCompareOp = back ? CompareOperation(static_cast<u32>(reg.get_STENCIL_FUNC_B())) : ds.stencilFrontCompareOp;
	ds.stencilBackFailOp = back ? static_cast<DkStencilOp>(static_cast<u32>(reg.get_STENCIL_FAIL_B()) + 1) : ds.stencilFrontFailOp;
	ds.stencilBackPassOp = back ? static_cast<DkStencilOp>(static_cast<u32>(reg.get_STENCIL_ZPASS_B()) + 1) : ds.stencilFrontPassOp;
	ds.stencilBackDepthFailOp = back ? static_cast<DkStencilOp>(static_cast<u32>(reg.get_STENCIL_ZFAIL_B()) + 1) : ds.stencilFrontDepthFailOp;
	if (Changed(StateSlot::DepthStencil, ds))
		dkCmdBufBindDepthStencilState(Cmd(), &ds);
	const auto& frontMask = ctx.DB_STENCILREFMASK;
	const auto& backMask = ctx.DB_STENCILREFMASK_BF;
	const std::array<u32, 6> stencil{frontMask.get_STENCILWRITEMASK_F(), frontMask.get_STENCILREF_F(), frontMask.get_STENCILMASK_F(),
		back ? backMask.get_STENCILWRITEMASK_B() : frontMask.get_STENCILWRITEMASK_F(),
		back ? backMask.get_STENCILREF_B() : frontMask.get_STENCILREF_F(), back ? backMask.get_STENCILMASK_B() : frontMask.get_STENCILMASK_F()};
	if (Changed(StateSlot::Stencil, stencil))
	{
		dkCmdBufSetStencil(Cmd(), DkFace_Front, stencil[0], stencil[1], stencil[2]);
		dkCmdBufSetStencil(Cmd(), DkFace_Back, stencil[3], stencil[4], stencil[5]);
	}
	if (Changed(StateSlot::Viewport, viewport))
		SetViewport(Cmd(), viewport);
	if (Changed(StateSlot::Scissor, scissor))
		dkCmdBufSetScissors(Cmd(), 0, &scissor, 1);

	std::array<DkVtxAttribState, 32> attributes{};
	std::array<DkVtxBufferState, Latte::GPU_LIMITS::NUM_VERTEX_BUFFERS> buffers{};
	u32 count = 0;
	const auto* vertex = LatteSHRC_GetActiveVertexShader();
	for (const auto& group : LatteSHRC_GetActiveFetchShader()->bufferGroups)
	{
		const u32 index = group.attributeBufferIndex;
		buffers[index].stride = vertexStrides[index];
		for (sint32 j = 0; j < group.attribCount; ++j)
		{
			const auto& attr = group.attrib[j];
			const sint32 location = vertex->resourceMapping.attributeMapping[attr.semanticId];
			if (location < 0)
				continue;
			if (location >= attributes.size() || attr.offset >= (1 << 14))
				throw std::runtime_error("Deko3D vertex attribute exceeds the hardware layout limits");
			auto& output = attributes[location];
			output.bufferId = index;
			output.offset = attr.offset;
			output.size = VertexFormat(attr.format);
			output.type = DkVtxAttribType_Uint;
			count = std::max(count, static_cast<u32>(location + 1));
			if (attr.fetchType == LatteConst::INSTANCE_DATA)
				buffers[index].divisor = std::max(1, attr.aluDivisor);
		}
	}
	if (Changed(StateSlot::VertexAttributes, std::span<const DkVtxAttribState>(attributes.data(), count)))
		dkCmdBufBindVtxAttribState(Cmd(), attributes.data(), count);
	if (Changed(StateSlot::VertexLayout, buffers))
		dkCmdBufBindVtxBufferState(Cmd(), buffers.data(), buffers.size());
}
