#include "DKRendererInternal.h"
#include "config/CemuConfig.h"
#include "config/ActiveSettings.h"
#include "platform/switch/SwitchLSFG.h"
#include "platform/switch/SwitchLSFGDeko.h"
#include "platform/switch/SwitchPlatform.h"
#include "platform/switch/SwitchSwkbd.h"
#include "platform/switch/SwitchThread.h"

using namespace Deko3D;

void Deko3DRenderer::State::BindDisplay()
{
	auto view = display->MakeView();
	const DkImageView* target = &view;
	dkCmdBufBindRenderTargets(Cmd(), &target, 1, nullptr);
	Invalidate();
}

void Deko3DRenderer::State::BindUtility(const DKShaderProgram* vertex, const DKShaderProgram* fragment, bool blend)
{
	Invalidate();
	vertex->code->PrepareForUse(Cmd());
	fragment->code->PrepareForUse(Cmd());
	const DkShader* shaders[]{&vertex->shader, &fragment->shader};
	dkCmdBufBindShaders(Cmd(), DkStageFlag_GraphicsMask, shaders, 2);
	DkRasterizerState raster;
	dkRasterizerStateDefaults(&raster);
	raster.cullMode = DkFace_None;
	dkCmdBufBindRasterizerState(Cmd(), &raster);
	DkDepthStencilState depth;
	dkDepthStencilStateDefaults(&depth);
	depth.depthTestEnable = false;
	depth.depthWriteEnable = false;
	dkCmdBufBindDepthStencilState(Cmd(), &depth);
	DkColorState color;
	dkColorStateDefaults(&color);
	color.blendEnableMask = blend ? 1 : 0;
	dkCmdBufBindColorState(Cmd(), &color);
	const DkColorWriteState write{0xf};
	dkCmdBufBindColorWriteState(Cmd(), &write);
	DkBlendState blending;
	dkBlendStateDefaults(&blending);
	blending.srcAlphaBlendFactor = DkBlendFactor_One;
	blending.dstAlphaBlendFactor = DkBlendFactor_InvSrcAlpha;
	BindBlendStates({&blending, 1});
	DkMultisampleState multisample;
	dkMultisampleStateDefaults(&multisample);
	dkCmdBufBindMultisampleState(Cmd(), &multisample);
	dkCmdBufSetPrimitiveRestart(Cmd(), false, ~0u);
	dkCmdBufSetSampleMask(Cmd(), 0xffff);
	FlushDescriptors();
	Invalidate();
}

bool Deko3DRenderer::BeginFrame(bool mainWindow)
{
	if (SwitchSwkbd_IsAppletActive() || (!mainWindow && !SwitchPlatform_IsGamePadOutputActive()))
		return false;
	if (!m_state->compositeStarted)
	{
		m_state->BindDisplay();
		const DkScissor scissor{0, 0, m_state->display->Width(), m_state->display->Height()};
		dkCmdBufSetScissors(m_state->Cmd(), 0, &scissor, 1);
		const float black[4]{};
		dkCmdBufClearColor(m_state->Cmd(), 0, DkColorMask_RGBA, black);
		m_state->compositeStarted = true;
	}
	return true;
}

void Deko3DRenderer::ClearColorbuffer(bool padView)
{
	if (!BeginFrame(!padView))
		return;
	m_state->BindDisplay();
	int x, y, width, height;
	SwitchPlatform_GetGamePadViewRegion(padView, m_state->display->Width(), m_state->display->Height(), x, y, width, height);
	const DkScissor scissor{static_cast<u32>(x), static_cast<u32>(y), static_cast<u32>(width), static_cast<u32>(height)};
	dkCmdBufSetScissors(m_state->Cmd(), 0, &scissor, 1);
	const float black[4]{};
	dkCmdBufClearColor(m_state->Cmd(), 0, DkColorMask_RGBA, black);
}

void Deko3DRenderer::DrawEmptyFrame(bool mainWindow)
{
	if (BeginFrame(mainWindow))
		SwapBuffers(mainWindow, !mainWindow);
}

void Deko3DRenderer::DrawBackbufferQuad(LatteTextureView* source, RendererOutputShader* shader,
										bool linear, sint32 x, sint32 y, sint32 width, sint32 height, bool padView, bool clear)
{
	if ((!padView && SwitchPlatform_IsGamePadOnly()) || !BeginFrame(!padView))
		return;
	auto* texture = static_cast<LatteTextureViewDK*>(source);
	auto& state = *m_state;
	state.TextureBarrier(*texture->Texture(), false);
	if (SwitchPlatform_IsGamePadCompositeActive())
		SwitchPlatform_GetGamePadImageRect(padView, state.display->Width(), state.display->Height(),
										   source->baseTexture->width, source->baseTexture->height, x, y, width, height);
	if (clear)
		ClearColorbuffer(padView);
	const auto* vertex = static_cast<RendererShaderDK*>(shader->GetVertexShader())->GetProgram();
	const auto* fragment = static_cast<RendererShaderDK*>(shader->GetFragmentShader())->GetProgram();
	if (!vertex || !fragment)
		return;
	DkSampler sampler;
	dkSamplerDefaults(&sampler);
	sampler.minFilter = sampler.magFilter = linear ? DkFilter_Linear : DkFilter_Nearest;
	for (auto& wrap : sampler.wrapMode)
		wrap = DkWrapMode_ClampToEdge;
	const u32 samplerID = state.Sampler(sampler);
	const auto parameters = shader->FillUniformBlockBuffer(*source, {width, height}, padView);
	DkResHandle handle;
	UploadAllocation uniforms;
	while (true)
	{
		const u64 fence = g_dk_command_buffer_mgr->GetCurrentFenceCounter();
		handle = state.TextureHandle(texture->MakeView(), samplerID);
		uniforms = state.Upload({reinterpret_cast<const u8*>(&parameters), sizeof(parameters)});
		if (fence == g_dk_command_buffer_mgr->GetCurrentFenceCounter())
			break;
	}
	state.BindDisplay();
	state.BindUtility(vertex, fragment, false);
	const DkViewport viewport{static_cast<float>(x), static_cast<float>(y), static_cast<float>(width), static_cast<float>(height), 0, 1};
	const DkScissor scissor{0, 0, state.display->Width(), state.display->Height()};
	SetViewport(state.Cmd(), viewport);
	dkCmdBufSetScissors(state.Cmd(), 0, &scissor, 1);
	const DkBufExtents buffer{uniforms.gpu, uniforms.size};
	if (fragment->bindings.uniforms[1] >= 0)
		dkCmdBufBindUniformBuffers(state.Cmd(), DkStage_Fragment, fragment->bindings.uniforms[1], &buffer, 1);
	if (fragment->bindings.textures[0] >= 0)
		dkCmdBufBindTextures(state.Cmd(), DkStage_Fragment, fragment->bindings.textures[0], &handle, 1);
	dkCmdBufDraw(state.Cmd(), DkPrimitive_Triangles, 6, 1, 0, 0);
	state.gameFrame = true;
}

void Deko3DRenderer::State::PollFrameGeneration()
{
	try
	{
		if (!framePreparationStarted && SwitchLSFG_IsEnabled())
		{
			framePreparationStarted = true;
			SwitchLSFGDeko_SetPreparing(true);
			const bool performance = SwitchLSFGDeko_GetConfig().performance;
			auto task = std::make_shared<std::packaged_task<std::string()>>([performance] {
				SwitchThread_AllowHelperCore("Deko3D frame generation shaders");
				std::string error;
				if (FrameGeneration::PrepareShaders(performance, error))
					return std::string{};
				return error.empty() ? std::string{"Shader preparation failed"} : error;
			});
			framePreparation = task->get_future();
			framePreparationThread = std::make_unique<ShaderThread>([task] { (*task)(); });
		}
		if (framePreparation.valid() && framePreparation.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
		{
			framePreparationThread.reset();
			SwitchLSFGDeko_SetPreparing(false);
			const std::string error = framePreparation.get();
			if (!error.empty())
			{
				SwitchLSFGDeko_SetAvailable(false);
				cemuLog_log(LogType::Force, "Deko3D frame generation: {}", error);
			}
			else
			{
				const auto config = SwitchLSFGDeko_GetConfig();
				frameGenerator = CreateFrameGenerator({config.performance, 2, config.flowDenominator});
				SwitchLSFGDeko_SetAvailable(frameGenerator != nullptr);
			}
		}
	} catch (const std::exception& error)
	{
		framePreparationThread.reset();
		framePreparation = {};
		SwitchLSFGDeko_SetPreparing(false);
		SwitchLSFGDeko_SetAvailable(false);
		frameGenerator.reset();
		cemuLog_log(LogType::Force, "Deko3D frame generation: {}", error.what());
	}
}

bool Deko3DRenderer::State::GenerateFrame()
{
	const bool requested = SwitchLSFG_IsEnabled();
	const auto now = std::chrono::steady_clock::now();
	const double interval = lastSource.time_since_epoch().count() ? std::chrono::duration<double>(now - lastSource).count() : 0;
	lastSource = now;
	if (!requested || rateDecision != 0)
	{
		if (interval >= 0.004 && interval <= 0.1)
		{
			sourceInterval = sourceSamples ? sourceInterval * 0.875 + interval * 0.125 : interval;
			sourceSamples = std::min(sourceSamples + 1, 120u);
		}
	}
	if (requested != frameRequested)
	{
		frameRequested = requested;
		rateDecision = requested && sourceSamples >= 8 ? (sourceInterval < 0.030 ? 1 : 0) : -1;
		slowSamples = 0;
		if (frameGenerator)
			frameGenerator->Reset();
	}
	if (!requested || !frameGenerator)
	{
		SwitchLSFGDeko_SetHighRate(false);
		return false;
	}
	if (interval > 0.25)
	{
		frameGenerator->Reset();
		return false;
	}
	if (rateDecision < 0 && sourceSamples >= 8)
		rateDecision = sourceInterval < 0.030 ? 1 : 0;
	if (rateDecision == 1)
	{
		slowSamples = interval >= 0.0315 ? slowSamples + 1 : interval > 0 ? 0
																		  : slowSamples;
		if (slowSamples < 16)
		{
			SwitchLSFGDeko_SetHighRate(true);
			return false;
		}
		rateDecision = 0;
		slowSamples = 0;
	}
	SwitchLSFGDeko_SetHighRate(false);
	if (rateDecision < 0)
		return false;
	Barrier(DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_Shader);
	try
	{
		return frameGenerator->Generate(display.get(), {0, 0, static_cast<int>(display->Width()), static_cast<int>(display->Height())});
	} catch (const std::exception& error)
	{
		SwitchLSFGDeko_SetAvailable(false);
		frameGenerator.reset();
		InvalidateCachedState();
		cemuLog_log(LogType::Force, "Deko3D frame generation: {}", error.what());
		return false;
	}
}

void Deko3DRenderer::State::PresentImage(const DKImage& image)
{
	int slot;
	{
		slot = swapchain->Acquire();
	}
	if (slot < 0)
		throw std::runtime_error("Deko3D could not acquire a presentation image");
	auto source = image.MakeView();
	DkImageView destination;
	dkImageViewDefaults(&destination, &swapchain->GetImage(slot));
	const DkImageRect region{0, 0, 0, display->Width(), display->Height(), 1};
	Barrier(DkBarrier_Full, DkInvalidateFlags_Image);
	dkCmdBufBlitImage(Cmd(), &source, &region, &destination, &region, DkBlitFlag_FilterNearest, 0);
	Submit(false, swapchain.get(), slot);
}

void Deko3DRenderer::SwapBuffers(bool tv, bool drc)
{
	auto& state = *m_state;
	if (SwitchSwkbd_IsAppletActive() || !(tv || (drc && SwitchPlatform_IsGamePadOutputActive())))
		return;
	if (SwitchPlatform_TakeGamePadLayoutChange())
	{
		if (state.frameGenerator)
			state.frameGenerator->Reset();
		state.lastSource = {};
		state.sourceSamples = 0;
		state.rateDecision = -1;
	}
	state.PollFrameGeneration();
	const bool generated = state.gameFrame && state.GenerateFrame();
	state.swapchain->SetVSync(generated || GetConfig().vsync.GetValue() != 0);
	if (generated)
		for (u32 i = 0; i < state.frameGenerator->GetGeneratedFrameCount(); ++i)
			state.PresentImage(*state.frameGenerator->GetGeneratedFrame(i));
	state.PresentImage(*state.display);
	state.compositeStarted = false;
	state.gameFrame = false;
	if (ActiveSettings::FlushGPUCacheOnSwap())
		++state.snapshotEpoch;
	SwitchPlatform_NotifyGameFrameSubmitted();
}
