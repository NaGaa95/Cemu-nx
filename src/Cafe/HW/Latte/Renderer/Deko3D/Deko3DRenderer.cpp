#include "DKRendererInternal.h"
#include "DKMemoryTracker.h"
#include "Cafe/HW/Latte/Core/LatteBufferCache.h"
#include "platform/switch/SwitchPlatform.h"
#include "platform/switch/SwitchSwkbd.h"
#include "platform/switch/SwitchLSFG.h"
#include "platform/switch/SwitchLSFGDeko.h"
#include "config/CemuConfig.h"
#include <imgui.h>

using namespace Deko3D;

namespace Deko3D
{
	void InvalidateCachedState()
	{
		if (g_renderer && g_renderer->GetType() == RendererAPI::Deko3D)
			static_cast<Deko3DRenderer*>(g_renderer.get())->InvalidateState();
	}

	u32 StageIndex(LatteConst::ShaderType stage)
	{
		switch (stage)
		{
		case LatteConst::ShaderType::Vertex:
			return 0;
		case LatteConst::ShaderType::Pixel:
			return 1;
		case LatteConst::ShaderType::Geometry:
			return 2;
		default:
			throw std::runtime_error("Invalid shader stage");
		}
	}

	DkStage ShaderStage(LatteConst::ShaderType stage)
	{
		static constexpr DkStage stages[]{DkStage_Vertex, DkStage_Fragment, DkStage_Geometry};
		return stages[StageIndex(stage)];
	}

	DkCompareOp CompareOperation(u32 operation)
	{
		return static_cast<DkCompareOp>((operation & 7) + 1);
	}
} // namespace Deko3D

Deko3DRenderer::Deko3DRenderer() : Renderer(RendererAPI::Deko3D), m_state(std::make_unique<State>())
{
	try
	{
		g_dk_context = DKContext::Create();
		if (!g_dk_context)
			throw std::runtime_error("Deko3D could not create its device or queue");
		g_dk_command_buffer_mgr = std::make_unique<DKCommandBufferManager>();
		if (!g_dk_command_buffer_mgr->Initialize())
			throw std::runtime_error("Deko3D could not allocate command memory");
		State& state = *m_state;
		state.swapchain = DKSwapChain::Create();
		if (!state.swapchain)
			throw std::runtime_error("Deko3D could not create the swapchain");
		state.display = DKImage::Create(state.swapchain->Width(), state.swapchain->Height(),
										DkImageFormat_RGBA8_Unorm, DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine);
		state.uploads = DKStreamBuffer::Create(TEXTURE_UPLOAD_BUFFER_SIZE, "texture and buffer uploads");
		state.dynamic = DKStreamBuffer::Create(16 * 1024 * 1024, "uniform and index data");
		state.descriptors = DKStreamBuffer::Create(NUM_IMAGE_DESCRIPTORS * sizeof(DkImageDescriptor), "image descriptors");
		const u32 samplerBytes = AlignUp(NUM_SAMPLER_DESCRIPTORS * sizeof(DkSamplerDescriptor), DK_MEMBLOCK_ALIGNMENT);
		state.samplers = dk::MemBlockMaker{g_dk_context->GetDevice(), samplerBytes}
							 .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
							 .create();
		if (!state.display || !state.uploads || !state.dynamic || !state.descriptors || !state.samplers)
			throw std::bad_alloc();
		MemoryTracker::RegisterMemBlock(state.samplers, "sampler descriptors");
		const DkImageType types[]{DkImageType_1D, DkImageType_2D, DkImageType_3D,
								  DkImageType_2DArray, DkImageType_CubemapArray};
		for (u32 i = 0; i < state.nullTextures.size(); ++i)
		{
			state.nullTextures[i] = DKImage::Create(1, 1, DkImageFormat_RGBA8_Unorm,
													types[i] == DkImageType_3D ? 0 : DkImageFlags_Usage2DEngine, types[i], i == 4 ? 6 : 1);
			if (!state.nullTextures[i])
				throw std::bad_alloc();
		}
		state.scissor = {0, 0, state.swapchain->Width(), state.swapchain->Height()};
		state.viewport = {0, 0, static_cast<float>(state.swapchain->Width()),
						  static_cast<float>(state.swapchain->Height()), 0, 1};
		m_vendor = GfxVendor::Nvidia;
		m_selectedDeviceName = "NVIDIA Tegra X1 (Deko3D)";
		RendererShaderDK::InitializeCompiler();
	} catch (...)
	{
		RendererShaderDK::ShutdownCompiler();
		m_state.reset();
		g_dk_command_buffer_mgr.reset();
		g_dk_context.reset();
		throw;
	}
}

Deko3DRenderer::~Deko3DRenderer()
{
	Shutdown();
	MemoryTracker::UnregisterMemBlock(m_state->samplers);
	MemoryTracker::UnregisterMemBlock(m_state->importedMemory);
	MemoryTracker::UnregisterMemBlock(m_state->zeroBuffer);
	MemoryTracker::UnregisterMemBlock(m_state->streamout);
	m_state.reset();
	g_dk_command_buffer_mgr.reset();
	g_dk_context.reset();
}

void Deko3DRenderer::Initialize()
{
	Renderer::Initialize();
	m_state->initialized = true;
	m_state->PollFrameGeneration();
}

void Deko3DRenderer::Shutdown()
{
	if (!m_state || m_state->shutdown)
		return;
	m_state->shutdown = true;
	if (m_state->framePreparation.valid())
		m_state->framePreparation.wait();
	m_state->framePreparationThread.reset();
	RendererShaderDK::ShutdownCompiler();
	m_state->Submit(true);
	g_dk_context->WaitIdle();
	DeleteFontTextures();
	for (auto* image : m_state->uiTextures)
		delete image;
	m_state->uiTextures.clear();
	if (m_state->initialized)
	{
		Renderer::Shutdown();
		m_state->initialized = false;
	}
	LatteBufferCache_setHostMemorySync(0, 0);
	SwitchLSFGDeko_SetAvailable(false);
	SwitchLSFGDeko_SetPreparing(false);
}

bool Deko3DRenderer::GetVRAMInfo(int& usage, int& total) const
{
	usage = static_cast<int>(MemoryTracker::GetUsage() / (1024 * 1024));
	total = -1;
	return true;
}

bool Deko3DRenderer::IsPadWindowActive()
{
	return SwitchPlatform_IsGamePadOutputActive();
}
void Deko3DRenderer::InvalidateState()
{
	if (m_state)
		m_state->Invalidate();
}

void Deko3DRenderer::State::Invalidate()
{
	dirty = true;
	resourcesInvalid = true;
	descriptorSetsBound = false;
	stateCache.Invalidate();
}

void Deko3DRenderer::State::Barrier(DkBarrier barrier, uint32 flags)
{
	dkCmdBufBarrier(Cmd(), barrier, flags);
}

DkCmdBuf Deko3DRenderer::State::Cmd()
{
	pending = true;
	return g_dk_command_buffer_mgr->GetCurrentCommandBuffer();
}

void Deko3DRenderer::State::Submit(bool wait, DKSwapChain* present, int slot)
{
	if (!pending && !present && !wait)
		return;
	g_dk_command_buffer_mgr->SubmitCommandBuffer(wait, present, slot);
	pending = false;
	bufferReadsDirty = true;
	Invalidate();
	descriptorCache.clear();
	commandsSinceSubmit = 0;
	sequencesSinceSubmit = 0;
	lastSubmit = std::chrono::steady_clock::now();
	PollGuestCompletions();
}

void Deko3DRenderer::State::PollGuestCompletions()
{
	g_dk_command_buffer_mgr->PollCompletedFences();
	if (guestBuffers)
		guestBuffers->Reclaim();
	guestCompletions.Complete(g_dk_command_buffer_mgr->GetCompletedFenceCounter());
}

UploadAllocation Deko3DRenderer::State::Reserve(DKStreamBuffer& stream, uint32 size, uint32 alignment)
{
	if (!size || size > stream.GetCurrentSize() - alignment)
		throw std::runtime_error("Deko3D stream allocation exceeds its buffer");
	if (!stream.ReserveMemory(size, alignment))
	{
		pending = true;
		Submit(false);
		if (!stream.ReserveMemory(size, alignment))
			throw std::runtime_error("Deko3D could not reserve stream memory");
	}
	UploadAllocation result{stream.GetCurrentHostPointer(), stream.GetCurrentGpuAddr(), size};
	stream.CommitMemory(size);
	return result;
}

UploadAllocation Deko3DRenderer::State::Allocate(uint32 bytes, uint32 alignment)
{
	if (bytes > std::numeric_limits<uint32>::max() - std::max(alignment, static_cast<u32>(DK_MEMBLOCK_ALIGNMENT)))
		throw std::bad_alloc();
	UploadAllocation result;
	if (bytes <= dynamic->GetCurrentSize() - alignment)
		result = Reserve(*dynamic, bytes, alignment);
	else
	{
		const u32 size = AlignUp(bytes, DK_MEMBLOCK_ALIGNMENT);
		auto block = dk::MemBlockMaker{g_dk_context->GetDevice(), size}
						 .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
						 .create();
		if (!block)
			throw std::bad_alloc();
		result = {block.getCpuAddr(), block.getGpuAddr(), bytes};
		DeferMemBlockDestruction(std::move(block));
	}
	return result;
}

UploadAllocation Deko3DRenderer::State::Upload(std::span<const uint8> bytes, uint32 alignment)
{
	if (bytes.size() > std::numeric_limits<uint32>::max() - alignment)
		throw std::bad_alloc();
	const auto result = Allocate(static_cast<u32>(bytes.size()), alignment);
	std::memcpy(result.cpu, bytes.data(), bytes.size());
	return result;
}

void Deko3DRenderer::State::BindDescriptors()
{
	if (descriptorSetsBound)
		return;
	dkCmdBufBindImageDescriptorSet(Cmd(), descriptors->GetGpuAddr(), NUM_IMAGE_DESCRIPTORS);
	dkCmdBufBindSamplerDescriptorSet(Cmd(), samplers.getGpuAddr(), NUM_SAMPLER_DESCRIPTORS);
	descriptorSetsBound = true;
	descriptorsDirty = true;
}

void Deko3DRenderer::State::FlushDescriptors()
{
	BindDescriptors();
	if (descriptorsDirty)
	{
		Barrier(DkBarrier_None, DkInvalidateFlags_Descriptors);
		descriptorsDirty = false;
	}
}

uint32 Deko3DRenderer::State::Sampler(const DkSampler& sampler)
{
	DkSamplerDescriptor descriptor;
	dkSamplerDescriptorInitialize(&descriptor, &sampler);
	std::array<u32, sizeof(descriptor) / sizeof(u32)> key;
	std::memcpy(key.data(), &descriptor, sizeof(descriptor));
	if (auto it = samplerCache.find(key); it != samplerCache.end())
		return it->second;
	if (samplerCache.size() >= NUM_SAMPLER_DESCRIPTORS)
		ResetSamplers();
	const u32 slot = samplerCache.size();
	std::memcpy(static_cast<u8*>(samplers.getCpuAddr()) + slot * sizeof(descriptor), &descriptor, sizeof(descriptor));
	samplerCache.emplace(key, slot);
	descriptorsDirty = true;
	return slot;
}

void Deko3DRenderer::State::ResetSamplers()
{
	Submit(true);
	samplerCache.clear();
	++samplerGeneration;
}

DkResHandle Deko3DRenderer::State::TextureHandle(const DkImageView& view, uint32 sampler)
{
	DkImageDescriptor descriptor;
	dkImageDescriptorInitialize(&descriptor, &view, false, false);
	return TextureHandle(descriptor, sampler);
}

DkResHandle Deko3DRenderer::State::TextureHandle(const DkImageDescriptor& descriptor, uint32 sampler)
{
	std::array<u32, sizeof(descriptor) / sizeof(u32)> key;
	std::memcpy(key.data(), &descriptor, sizeof(descriptor));
	if (auto it = descriptorCache.find(key); it != descriptorCache.end())
		return dkMakeTextureHandle(it->second, sampler);
	UploadAllocation allocation;
	if (descriptorCursor + sizeof(DkImageDescriptor) <= descriptorBatch.size)
	{
		allocation = {static_cast<u8*>(descriptorBatch.cpu) + descriptorCursor,
			descriptorBatch.gpu + descriptorCursor, sizeof(DkImageDescriptor)};
		descriptorCursor += sizeof(DkImageDescriptor);
	}
	else
		allocation = Reserve(*descriptors, sizeof(DkImageDescriptor), sizeof(DkImageDescriptor));
	std::memcpy(allocation.cpu, &descriptor, sizeof(descriptor));
	const u32 index = (allocation.gpu - descriptors->GetGpuAddr()) / sizeof(DkImageDescriptor);
	descriptorCache.emplace(key, index);
	descriptorsDirty = true;
	return dkMakeTextureHandle(index, sampler);
}

void Deko3DRenderer::Flush(bool waitIdle)
{
	m_state->Submit(waitIdle);
}
void Deko3DRenderer::NotifyLatteCommandProcessorIdle()
{
	if (m_state->pending && ((!m_state->guestCompletions.Empty() &&
		m_state->guestReadbackFence == g_dk_command_buffer_mgr->GetCurrentFenceCounter()) ||
		std::chrono::steady_clock::now() - m_state->lastSubmit >= std::chrono::milliseconds(1)))
		m_state->Submit(false);
	if (!m_state->guestCompletions.Empty())
		m_state->PollGuestCompletions();
}

void Deko3DRenderer::QueueGuestCompletion(std::function<void()> completion)
{
	if (!m_state->guestCompletions.Empty())
		m_state->PollGuestCompletions();
	const auto fence = m_state->guestReadbackFence;
	if (fence <= g_dk_command_buffer_mgr->GetCompletedFenceCounter())
	{
		completion();
		return;
	}
	m_state->guestCompletions.Push(fence, [completion = std::move(completion)] {
		completion();
	});
	m_state->PollGuestCompletions();
}

void Deko3DRenderer::SurfaceSync(Latte::E_COHER_CNTL, MPTR, uint32)
{
	m_state->Barrier(DkBarrier_Primitives, DkInvalidateFlags_Image | DkInvalidateFlags_Shader);
	++m_state->serial;
}

void Deko3DRenderer::State::TextureBarrier(LatteTextureDK& texture, bool write, DkBarrier barrier)
{
	if ((!write && texture.lastWrite == serial) || (write && texture.lastRead == serial) ||
		(write && barrier == DkBarrier_Full && texture.lastWrite == serial))
	{
		Barrier(barrier, DkInvalidateFlags_Image);
		++serial;
	}
	if (write)
		texture.lastWrite = serial;
	else
		texture.lastRead = serial;
}

RendererShader* Deko3DRenderer::shader_create(RendererShader::ShaderType type, uint64 base,
											  uint64 auxiliary, const std::string& source, bool asynchronous, bool graphicsPack)
{
	return new RendererShaderDK(type, base, auxiliary, source, asynchronous, graphicsPack);
}

void Deko3DRenderer::AppendOverlayDebugInfo()
{
	ImGui::TextUnformatted("Deko3D / Tegra X1");
}
