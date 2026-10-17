#include "DKRendererInternal.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/Core/LatteIndices.h"
#include "Cafe/HW/Latte/Renderer/RendererCore.h"

using namespace Deko3D;

extern bool hasValidFramebufferAttached;

namespace
{
	bool UsesPrimitiveRestart(LattePrimitiveMode mode)
	{
		switch (mode)
		{
		case LattePrimitiveMode::LINE_LOOP:
		case LattePrimitiveMode::LINE_STRIP:
		case LattePrimitiveMode::TRIANGLE_STRIP:
		case LattePrimitiveMode::TRIANGLE_FAN:
		case LattePrimitiveMode::LINE_STRIP_ADJACENT:
		case LattePrimitiveMode::TRIANGLE_STRIP_ADJACENT:
			return true;
		default:
			return false;
		}
	}

	DkPrimitive Primitive(LattePrimitiveMode mode)
	{
		switch (mode)
		{
		case LattePrimitiveMode::POINTS:
			return DkPrimitive_Points;
		case LattePrimitiveMode::LINES:
			return DkPrimitive_Lines;
		case LattePrimitiveMode::LINE_LOOP:
		case LattePrimitiveMode::LINE_STRIP:
			return DkPrimitive_LineStrip;
		case LattePrimitiveMode::TRIANGLE_STRIP:
			return DkPrimitive_TriangleStrip;
		case LattePrimitiveMode::TRIANGLE_FAN:
			return DkPrimitive_TriangleFan;
		case LattePrimitiveMode::LINES_ADJACENT:
			return DkPrimitive_LinesAdjacency;
		case LattePrimitiveMode::LINE_STRIP_ADJACENT:
			return DkPrimitive_LineStripAdjacency;
		case LattePrimitiveMode::TRIANGLES_ADJACENT:
			return DkPrimitive_TrianglesAdjacency;
		case LattePrimitiveMode::TRIANGLE_STRIP_ADJACENT:
			return DkPrimitive_TriangleStripAdjacency;
		default:
			return DkPrimitive_Triangles;
		}
	}
} // namespace

void Deko3DRenderer::draw_beginSequence()
{
	auto& state = *m_state;
	state.skipSequence = false;
	LatteSHRC_UpdateActiveShaders();
	if (LatteGPUState.activeShaderHasError)
	{
		state.skipSequence = true;
		return;
	}
	LatteGPUState.requiresTextureBarrier = false;
	const bool streamout = LatteGPUState.contextRegister[mmVGT_STRMOUT_EN] != 0;
	do
	{
		LatteGPUState.repeatTextureInitialization = false;
		if (!LatteMRT::UpdateCurrentFBO() || (!hasValidFramebufferAttached && !streamout))
		{
			state.skipSequence = true;
			return;
		}
		LatteTexture_updateTextures();
	}
	while (LatteGPUState.repeatTextureInitialization);
	LatteMRT::ApplyCurrentState();
	LatteRenderTarget_updateViewport();
	LatteRenderTarget_updateScissorBox();
	const auto& ctx = LatteGPUState.contextNew;
	state.skipSequence = ctx.PA_CL_CLIP_CNTL.get_DX_RASTERIZATION_KILL() &&
						 ctx.PA_CL_VTE_CNTL.get_VPORT_X_OFFSET_ENA() && !streamout;
	state.dirty = true;
}

void Deko3DRenderer::draw_execute(uint32 baseVertex, uint32 baseInstance, uint32 instanceCount,
								  uint32 count, MPTR indexAddress, Latte::LATTE_VGT_DMA_INDEX_TYPE::E_INDEX_TYPE indexType,
								  const LatteDrawcallContext& context)
{
	auto& state = *m_state;
	if (state.skipSequence || !count || !instanceCount)
	{
		return;
	}
	if (LatteGPUState.contextNew.GetSpecialStateValues()[8])
	{
		LatteDraw_handleSpecialState8_clearAsDepth();
		return;
	}
	if (LatteGPUState.contextNew.GetSpecialStateValues()[5])
	{
		auto* color = LatteMRT::GetColorAttachment(0);
		auto* depth = LatteMRT::GetDepthAttachment();
		if (color && depth)
		{
			sint32 width, height;
			LatteMRT::GetVirtualViewportDimensions(width, height);
			surfaceCopy_copySurfaceWithFormatConversion(depth->baseTexture, depth->firstMip, depth->firstSlice,
														color->baseTexture, color->firstMip, color->firstSlice, width, height);
		}
		return;
	}
	state.verticesPerInstance = count;
	LatteStreamout_PrepareDrawcall(count, instanceCount);
	LatteDecompilerShader* shaders[]{LatteSHRC_GetActiveVertexShader(), LatteSHRC_GetActivePixelShader(), LatteSHRC_GetActiveGeometryShader()};
	const bool aluDirty[]{context.aluConstVSDirty, context.aluConstPSDirty, false};
	const u32 banksDirty[]{context.vsUniformBufferDirtyMask, context.psUniformBufferDirtyMask, context.gsUniformBufferDirtyMask};
	for (u32 i = 0; i < state.stages.size(); ++i)
		if (shaders[i])
			state.UpdateUniforms(shaders[i], state.stages[i], context.isFirst, aluDirty[i], banksDirty[i]);

	const auto mode = LatteGPUState.contextNew.VGT_PRIMITIVE_TYPE.get_PRIMITIVE_MODE();
	INDEX_TYPE hostType;
	u32 hostCount = 0, indexMax = 0;
	IndexAllocation indices{};
	LatteIndices_decode(memory_getPointerFromVirtualOffset(indexAddress), indexType, count, mode,
						indexMax, hostType, hostCount, indices);
	if (state.dirty)
		state.BindShaders(false);
	if (state.skipSequence)
		return;
	const auto* reservation = hostType == INDEX_TYPE::NONE ? nullptr : static_cast<IndexReservation*>(indices.rendererInternal);
	std::array<bool, 3> uploadVariables{};
	UploadAllocation allocation;
	while (true)
	{
		const u64 fence = g_dk_command_buffer_mgr->GetCurrentFenceCounter();
		state.UpdateGuestBuffers(indexMax + baseVertex, baseInstance, instanceCount);
		state.PrepareTextures(context.isFirst);
		u32 bytes = reservation ? AlignUp(static_cast<u32>(reservation->cached.size()), DK_UNIFORM_BUF_ALIGNMENT) : 0;
		for (u32 i = 0; i < state.stages.size(); ++i)
		{
			const auto& stage = state.stages[i];
			uploadVariables[i] = shaders[i] && !stage.variables.empty() &&
				(stage.variableFence != fence || stage.variablesDirty);
			if (uploadVariables[i])
				bytes += AlignUp(static_cast<u32>(stage.variables.size()), DK_UNIFORM_BUF_ALIGNMENT);
		}
		if (bytes)
			allocation = state.Reserve(*state.dynamic, bytes, DK_UNIFORM_BUF_ALIGNMENT);
		if (fence == g_dk_command_buffer_mgr->GetCurrentFenceCounter())
			break;
	}
	u32 offset = 0;
	if (reservation)
	{
		std::memcpy(allocation.cpu, reservation->cached.data(), reservation->cached.size());
		dkCmdBufBindIdxBuffer(state.Cmd(), hostType == INDEX_TYPE::U16 ? DkIdxFormat_Uint16 : DkIdxFormat_Uint32, allocation.gpu);
		offset = AlignUp(static_cast<u32>(reservation->cached.size()), DK_UNIFORM_BUF_ALIGNMENT);
	}
	for (u32 i = 0; i < state.stages.size(); ++i)
	{
		auto& stage = state.stages[i];
		if (!shaders[i] || stage.variables.empty())
			continue;
		if (!uploadVariables[i])
			continue;
		std::memcpy(static_cast<u8*>(allocation.cpu) + offset, stage.variables.data(), stage.variables.size());
		stage.variableBuffer = {allocation.gpu + offset, static_cast<u32>(stage.variables.size())};
		stage.uploadedVariables = stage.variables;
		stage.variablesDirty = false;
		stage.variableFence = g_dk_command_buffer_mgr->GetCurrentFenceCounter();
		offset += AlignUp(static_cast<u32>(stage.variables.size()), DK_UNIFORM_BUF_ALIGNMENT);
	}
	state.SyncGuestBuffers();
	if (state.dirty)
	{
		state.BindShaders();
		if (state.skipSequence)
			return;
		state.BindFramebuffer();
		state.BindPipeline();
	}
	state.BindResources(context.isFirst, context);
	state.BindVertexBuffers(state.vertexBuffers);
	const std::array<u32, 2> restart{hostType != INDEX_TYPE::NONE && UsesPrimitiveRestart(mode),
		LatteGPUState.contextNew.VGT_MULTI_PRIM_IB_RESET_INDX.get_RESTART_INDEX() & (hostType == INDEX_TYPE::U16 ? 0xffffu : ~0u)};
	if (state.Changed(StateSlot::Restart, restart))
		dkCmdBufSetPrimitiveRestart(state.Cmd(), restart[0], restart[1]);
	if (hostType == INDEX_TYPE::NONE)
		dkCmdBufDraw(state.Cmd(), Primitive(mode), count, instanceCount, baseVertex, baseInstance);
	else
		dkCmdBufDrawIndexed(state.Cmd(), Primitive(mode), hostCount, instanceCount, 0, baseVertex, baseInstance);
	state.dirty = false;
	state.resourcesInvalid = false;
	LatteStreamout_FinishDrawcall(true);
	state.StoreFramebuffer();
	if (state.framebuffer)
		for (auto* texture : state.framebuffer->GetTextures())
			static_cast<LatteTextureDK*>(texture)->lastWrite = state.serial;
	++LatteGPUState.drawCallCounter;
	if (++state.commandsSinceSubmit >= DRAW_SUBMIT_THRESHOLD)
		state.Submit(false);
}

void Deko3DRenderer::draw_endSequence()
{
	if (m_state->pending)
		++m_state->sequencesSinceSubmit;
	if (!m_state->skipSequence && LatteSHRC_GetActivePixelShader())
		LatteRenderTarget_trackUpdates();
	LatteTextureReadback_Update();
	if (m_state->sequencesSinceSubmit >= SEQUENCE_SUBMIT_THRESHOLD)
		m_state->Submit(false);
}
