#pragma once

#include "Deko3DRenderer.h"
#include "DKCommandBufferManager.h"
#include "DKContext.h"
#include "DKFrameGenerator.h"
#include "DKShader.h"
#include "DKShaderThread.h"
#include "DKStreamBuffer.h"
#include "DKGuestBufferCache.h"
#include "DKIndexBufferPool.h"
#include "DKSwapChain.h"
#include "DKTexture.h"
#include "DKStateCache.h"
#include "DKResourceCache.h"
#include "Cafe/HW/Latte/Renderer/FenceCallbacks.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.h"
#include "Cafe/HW/Latte/Core/FetchShader.h"
#include <map>
#include <unordered_set>

namespace Deko3D
{
	class QueryPool;
	struct UploadAllocation
	{
		void* cpu = nullptr;
		DkGpuAddr gpu = DK_GPU_ADDR_INVALID;
		u32 size = 0;
	};

	struct StageState
	{
		const DKShaderProgram* program = nullptr;
		std::array<DkBufExtents, LATTE_NUM_MAX_UNIFORM_BUFFERS> banks{};
		std::vector<u8> variables;
		std::vector<u8> uploadedVariables;
		const LatteDecompilerShader* uniformShader = nullptr;
		bool variablesDirty = true;
		u64 variableFence = 0;
		DkBufExtents variableBuffer{};
		std::array<DkResHandle, DK_NUM_TEXTURE_BINDINGS> textures{};
		std::array<TextureBindingCache, LATTE_NUM_MAX_TEX_UNITS> textureBindings{};
		std::array<DkBufExtents, DK_NUM_UNIFORM_BUFS> boundUniforms{};
		u32 textureCount = 0;
	};

	DkStage ShaderStage(LatteConst::ShaderType stage);
	u32 StageIndex(LatteConst::ShaderType stage);
	DkCompareOp CompareOperation(u32 operation);
	void SetViewport(DkCmdBuf command, const DkViewport& viewport);
} // namespace Deko3D

struct Deko3DRenderer::State
{
	class Readback;
	class Query;
	std::unique_ptr<Deko3D::DKSwapChain> swapchain;
	std::unique_ptr<Deko3D::DKImage> display;
	std::array<std::unique_ptr<Deko3D::DKImage>, 5> nullTextures;
	std::unique_ptr<Deko3D::DKStreamBuffer> uploads;
	std::unique_ptr<Deko3D::DKStreamBuffer> dynamic;
	std::unique_ptr<Deko3D::DKStreamBuffer> descriptors;
	std::unique_ptr<Deko3D::DKGuestBufferCache> guestBuffers;
	dk::UniqueMemBlock samplers;
	dk::UniqueMemBlock importedMemory;
	dk::UniqueMemBlock zeroBuffer;
	MPTR importedBase = 0;
	uint32 importedSize = 0;
	bool bufferReadsDirty = true;
	dk::UniqueMemBlock streamout;
	dk::UniqueCmdBuf stateCapture;
	Deko3D::StateCache stateCache;
	std::shared_ptr<Deko3D::QueryPool> queries;
	std::map<std::array<uint32, sizeof(DkSamplerDescriptor) / sizeof(uint32)>, uint32> samplerCache;
	std::map<std::array<uint32, sizeof(DkImageDescriptor) / sizeof(uint32)>, uint32> descriptorCache;
	uint64 samplerGeneration = 1;
	std::array<Deko3D::StageState, 3> stages;
	std::array<Deko3D::LatteTextureViewDK*, LATTE_CEMU_GS_TEX_UNIT_BASE + LATTE_NUM_MAX_TEX_UNITS> textures{};
	std::array<DkBufExtents, Latte::GPU_LIMITS::NUM_VERTEX_BUFFERS> vertexBuffers{};
	std::array<uint32, Latte::GPU_LIMITS::NUM_VERTEX_BUFFERS> vertexStrides{};
	std::array<uint32, LATTE_NUM_STREAMOUT_BUFFER> streamoutOffsets{};
	uint64 streamoutReadBegin = ~0ULL, streamoutReadEnd = 0;
	uint32 verticesPerInstance = 0;
	LatteCachedFBO* framebuffer = nullptr;
	uint32 colorTargetMask = 0;
	std::vector<Deko3D::LatteTextureViewDK*> volumeTargets;
	DkViewport viewport{};
	DkScissor scissor{};
	bool dirty = true;
	bool resourcesInvalid = true;
	bool descriptorSetsBound = false;
	bool descriptorsDirty = false;
	bool skipSequence = false;
	bool pending = false;
	uint64 guestReadbackFence = 0;
	uint32 snapshotEpoch = 0;
	FenceCallbacks guestCompletions;
	bool shutdown = false;
	bool initialized = false;
	bool compositeStarted = false;
	bool gameFrame = false;
	bool imguiPad = false;
	uint32 commandsSinceSubmit = 0;
	uint32 sequencesSinceSubmit = 0;
	Deko3D::UploadAllocation descriptorBatch;
	uint32 descriptorCursor = 0;
	uint64 serial = 1;
	uint64 lastQueryFence = 0;
	std::chrono::steady_clock::time_point lastSubmit{};
	std::unordered_map<void*, Deko3D::UploadAllocation> textureUploads;
	Deko3D::DKIndexBufferPool indexBuffers;
	std::unordered_set<Deko3D::DKImage*> uiTextures;
	Deko3D::DKImage* fontTexture = nullptr;
	std::unique_ptr<Deko3D::RendererShaderDK> imguiVertex;
	std::unique_ptr<Deko3D::RendererShaderDK> imguiFragment;
	std::unique_ptr<Deko3D::RendererShaderDK> copyVertex;
	std::array<std::unique_ptr<Deko3D::RendererShaderDK>, 2> copyFragment;
	std::map<std::array<uint32, 11>, std::unique_ptr<Deko3D::RendererShaderDK>> rectangleShaders;
	std::unique_ptr<Deko3D::FrameGenerator> frameGenerator;
	std::future<std::string> framePreparation;
	std::unique_ptr<Deko3D::ShaderThread> framePreparationThread;
	bool framePreparationStarted = false;
	bool frameRequested = false;
	std::chrono::steady_clock::time_point lastSource{};
	double sourceInterval = 0;
	unsigned sourceSamples = 0;
	unsigned slowSamples = 0;
	int rateDecision = -1;

	DkCmdBuf Cmd();
	void Invalidate();
	void Barrier(DkBarrier barrier, uint32 flags);
	void FlushDescriptors();
	template<typename T>
	bool Changed(Deko3D::StateSlot slot, const T& value)
	{
		return stateCache.Changed(slot, value);
	}
	void Submit(bool wait, Deko3D::DKSwapChain* present = nullptr, int slot = -1);
	void PollGuestCompletions();
	bool IsImportedRange(MPTR address, uint64 size) const;
	DkBufExtents GuestBuffer(MPTR address, uint32 size, bool uniform);
	void UpdateGuestBuffers(uint32 maxVertex, uint32 baseInstance, uint32 instanceCount);
	void SyncGuestBuffers();
	Deko3D::UploadAllocation Reserve(Deko3D::DKStreamBuffer& stream, uint32 size, uint32 alignment);
	Deko3D::UploadAllocation Allocate(uint32 size, uint32 alignment);
	Deko3D::UploadAllocation Upload(std::span<const uint8> bytes, uint32 alignment = DK_UNIFORM_BUF_ALIGNMENT);
	uint32 Sampler(const DkSampler& sampler);
	void ResetSamplers();
	DkResHandle TextureHandle(const DkImageView& view, uint32 sampler);
	DkResHandle TextureHandle(const DkImageDescriptor& descriptor, uint32 sampler);
	void BindDescriptors();
	void BindFramebuffer();
	DkImageView RenderView(Deko3D::LatteTextureViewDK& view);
	void CopyVolumeSlice(Deko3D::LatteTextureViewDK& view, bool store);
	void StoreFramebuffer();
	void BindDisplay();
	Deko3D::DKImage* CreateUITexture(const uint8* pixels, uint32 width, uint32 height);
	void BindUtility(const Deko3D::DKShaderProgram* vertex, const Deko3D::DKShaderProgram* fragment, bool blend);
	void BindShaders(bool bind = true);
	const Deko3D::DKShaderProgram* RectangleShader(LatteDecompilerShader* vertex);
	void BindResources(bool first, const LatteDrawcallContext& context);
	void PrepareTextures(bool first);
	void BindTextures(LatteDecompilerShader* shader, Deko3D::StageState& stage);
	void UpdateUniforms(LatteDecompilerShader* shader, Deko3D::StageState& stage, bool full, bool aluDirty, uint32 banksDirty);
	void BindPipeline();
	void BindVertexBuffers(std::span<const DkBufExtents> buffers);
	void BindBlendStates(std::span<const DkBlendState> states);
	void PresentImage(const Deko3D::DKImage& image);
	void PollFrameGeneration();
	bool GenerateFrame();
	void TextureBarrier(Deko3D::LatteTextureDK& texture, bool write, DkBarrier barrier = DkBarrier_Primitives);
};
