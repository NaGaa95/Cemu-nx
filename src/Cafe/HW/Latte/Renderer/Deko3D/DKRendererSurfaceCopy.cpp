#include "DKRendererInternal.h"

using namespace Deko3D;

void Deko3DRenderer::surfaceCopy_copySurfaceWithFormatConversion(LatteTexture* source, sint32 srcMip,
																 sint32 srcSlice, LatteTexture* destination, sint32 dstMip, sint32 dstSlice, sint32 width, sint32 height)
{
	auto& state = *m_state;
	auto& src = *static_cast<LatteTextureDK*>(source);
	auto& dst = *static_cast<LatteTextureDK*>(destination);
	if (src.isDepth == dst.isDepth)
	{
		texture_copyImageSubData(source, srcMip, 0, 0, srcSlice, destination, dstMip, 0, 0, dstSlice, width, height, 1);
		return;
	}
	if (src.GetBPP() != dst.GetBPP() || !LatteTexture_doesEffectiveRescaleRatioMatch(source, srcMip, destination, dstMip))
		return;
	LatteTexture_scaleToEffectiveSize(source, &width, &height, 0);
	src.AllocateOnHost();
	dst.AllocateOnHost();
	state.TextureBarrier(src, false);
	state.TextureBarrier(dst, true);
	if (!state.copyVertex)
	{
		state.copyVertex = std::make_unique<RendererShaderDK>(RendererShader::ShaderType::kVertex, 0, 0, R"(#version 450
void main() {
    vec2 p=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2);
    gl_Position=vec4(p*2.0-1.0,0,1);
}
)",
															  false, false);
		state.copyFragment[0] = std::make_unique<RendererShaderDK>(RendererShader::ShaderType::kFragment, 0, 0, R"(#version 450
layout(binding=0) uniform sampler2D source;
layout(location=0) out float color;
void main() { color=texelFetch(source,ivec2(gl_FragCoord.xy),0).r; }
)",
																   false, false);
		state.copyFragment[1] = std::make_unique<RendererShaderDK>(RendererShader::ShaderType::kFragment, 0, 0, R"(#version 450
layout(binding=0) uniform sampler2D source;
void main() { gl_FragDepth=texelFetch(source,ivec2(gl_FragCoord.xy),0).r; }
)",
																   false, false);
	}
	const auto* vertex = state.copyVertex->GetProgram();
	const auto* fragment = state.copyFragment[dst.isDepth ? 1 : 0]->GetProgram();
	if (!vertex || !fragment)
		return;
	DkSampler sampler;
	dkSamplerDefaults(&sampler);
	auto srcView = src.image->MakeCopyView(srcMip, srcSlice);
	auto dstView = dst.image->MakeCopyView(dstMip, dstSlice);
	srcView.type = dstView.type = DkImageType_2D;
	if (src.Is3DTexture())
	{
		auto* slice = static_cast<LatteTextureViewDK*>(src.GetOrCreateView(Latte::E_DIM::DIM_2D, src.format, srcMip, 1, srcSlice, 1));
		srcView = state.RenderView(*slice);
	}
	LatteTextureViewDK* volume = nullptr;
	if (dst.Is3DTexture())
	{
		volume = static_cast<LatteTextureViewDK*>(dst.GetOrCreateView(Latte::E_DIM::DIM_2D, dst.format, dstMip, 1, dstSlice, 1));
		dstView = state.RenderView(*volume);
	}
	const DkResHandle handle = state.TextureHandle(srcView, state.Sampler(sampler));
	state.BindUtility(vertex, fragment, false);
	if (dst.isDepth)
	{
		dkCmdBufBindRenderTargets(state.Cmd(), nullptr, 0, &dstView);
		DkDepthStencilState depth;
		dkDepthStencilStateDefaults(&depth);
		depth.depthCompareOp = DkCompareOp_Always;
		dkCmdBufBindDepthStencilState(state.Cmd(), &depth);
	}
	else
	{
		const DkImageView* target = &dstView;
		dkCmdBufBindRenderTargets(state.Cmd(), &target, 1, nullptr);
		const DkColorWriteState write{DkColorMask_R};
		dkCmdBufBindColorWriteState(state.Cmd(), &write);
	}
	const DkViewport viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
	const DkScissor scissor{0, 0, static_cast<u32>(width), static_cast<u32>(height)};
	SetViewport(state.Cmd(), viewport);
	dkCmdBufSetScissors(state.Cmd(), 0, &scissor, 1);
	dkCmdBufBindTextures(state.Cmd(), DkStage_Fragment, fragment->bindings.textures[0], &handle, 1);
	dkCmdBufDraw(state.Cmd(), DkPrimitive_Triangles, 3, 1, 0, 0);
	if (volume)
		state.CopyVolumeSlice(*volume, true);
	state.Barrier(DkBarrier_Primitives, DkInvalidateFlags_Image);
	++state.serial;
	state.Invalidate();
}
