#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#if defined(__SWITCH__)
#include "platform/switch/SwitchThread.h"
#endif
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineCompiler.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineStableCache.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/Core/LattePerformanceMonitor.h"
#include "Cafe/HW/Latte/Core/LatteCachedFBO.h"
#include "Cafe/OS/libs/gx2/GX2.h"
#include "config/ActiveSettings.h"
#include "util/helpers/Serializer.h"
#include "Cafe/HW/Latte/Common/RegisterSerializer.h"
#include "Cemu/FileCache/FileCache.h"
#include "Cafe/HW/Latte/Core/LatteShaderCache.h"
#include "util/helpers/helpers.h"
#include <openssl/sha.h>

struct
{
	uint32 pipelineLoadIndex;
	uint32 pipelineMaxFileIndex;
	
	std::atomic_uint32_t pipelinesQueued;
	std::atomic_uint32_t pipelinesLoaded;
}g_vkCacheState;

VulkanPipelineStableCache g_vkPipelineStableCacheInstance;

VulkanPipelineStableCache& VulkanPipelineStableCache::GetInstance()
{
	return g_vkPipelineStableCacheInstance;
}

uint32 VulkanPipelineStableCache::BeginLoading(uint64 cacheTitleId)
{
	std::error_code ec;
	fs::create_directories(ActiveSettings::GetCachePath("shaderCache/transferable"), ec);
	const auto pathCacheFile = ActiveSettings::GetCachePath("shaderCache/transferable/{:016x}_vkpipeline.bin", cacheTitleId);
	
	// init cache loader state
	g_vkCacheState.pipelineLoadIndex = 0;
	g_vkCacheState.pipelineMaxFileIndex = 0;
	g_vkCacheState.pipelinesLoaded = 0;
	g_vkCacheState.pipelinesQueued = 0;
	
	// start async compilation threads
	m_compilationQueue.clear();

#if defined(__SWITCH__)
	m_numCompilationThreads = 1;
#else
	uint32 cpuCoreCount = GetPhysicalCoreCount();
	m_numCompilationThreads = std::clamp(cpuCoreCount, 1u, 8u);
#endif
	if (VulkanRenderer::GetInstance()->GetDisableMultithreadedCompilation())
		m_numCompilationThreads = 1;
	{
		std::scoped_lock lock(m_pipelineIsCachedLock);
		m_pipelineIsCached.clear();
	}

	const uint32 threadCount = m_numCompilationThreads.load();
	cemu_assert_debug(m_compilationThreads.empty());
	m_compilationThreads.reserve(threadCount);
	for (uint32 i = 0; i < threadCount; i++)
		m_compilationThreads.emplace_back(&VulkanPipelineStableCache::CompilerThread, this);

	{
		std::scoped_lock lock(m_cacheMutex);
		++m_cacheGeneration;
		cemu_assert_debug(s_cache == nullptr);
		s_cache = FileCache::Open(pathCacheFile, true, LatteShaderCache_getPipelineCacheExtraVersion(cacheTitleId));
		if (!s_cache)
			return 0;
		s_cache->UseCompression(false);
		g_vkCacheState.pipelineMaxFileIndex = s_cache->GetMaximumFileIndex();
		return s_cache->GetFileCount();
	}
}

bool VulkanPipelineStableCache::UpdateLoading(uint32& pipelinesLoadedTotal, uint32& pipelinesMissingShaders)
{
	pipelinesLoadedTotal = g_vkCacheState.pipelinesLoaded;
	pipelinesMissingShaders = 0;
	if (!s_cache)
		return false;

	constexpr size_t kMaxQueuedPipelines = 50;
	const uint32 loadedBefore = pipelinesLoadedTotal;
	while (g_vkCacheState.pipelineLoadIndex <= g_vkCacheState.pipelineMaxFileIndex)
	{
		if (m_compilationQueue.size() >= kMaxQueuedPipelines)
			break;

		uint64 fileNameA, fileNameB;
		std::vector<uint8> fileData;
		if (s_cache->GetFileByIndex(g_vkCacheState.pipelineLoadIndex, &fileNameA, &fileNameB, fileData))
		{
			g_vkCacheState.pipelinesQueued++;
			m_compilationQueue.push(std::move(fileData));
		}
		g_vkCacheState.pipelineLoadIndex++;
	}

	pipelinesLoadedTotal = g_vkCacheState.pipelinesLoaded;
	const bool producerDone = g_vkCacheState.pipelineLoadIndex > g_vkCacheState.pipelineMaxFileIndex;
	if (producerDone && pipelinesLoadedTotal == g_vkCacheState.pipelinesQueued)
		return false;

	std::unique_lock lock(m_loadingMutex);
	m_loadingCv.wait(lock, [&] {
		return m_numCompilationThreads == 0 ||
			g_vkCacheState.pipelinesLoaded != loadedBefore ||
			(!producerDone && m_compilationQueue.size() < kMaxQueuedPipelines);
	});
	pipelinesLoadedTotal = g_vkCacheState.pipelinesLoaded;
	return m_numCompilationThreads != 0;
}

void VulkanPipelineStableCache::EndLoading()
{
	// shut down compilation threads
	const uint32 threadCount = m_numCompilationThreads.exchange(0);
	for (uint32 i = 0; i < threadCount; i++)
		m_compilationQueue.push({});
	m_loadingCv.notify_all();
	for (auto& thread : m_compilationThreads)
		thread.join();
	m_compilationThreads.clear();
	// keep cache file open for writing of new pipelines
}

void VulkanPipelineStableCache::Close()
{
	if (m_pipelineCacheStoreThreadStarted)
	{
		m_pipelineCachingQueue.push(nullptr);
		if (m_pipelineCacheStoreThread.joinable())
			m_pipelineCacheStoreThread.join();
		m_pipelineCacheStoreThreadStarted = false;
	}

	{
		std::scoped_lock lock(m_cacheMutex);
		++m_cacheGeneration;
		if (s_cache)
		{
			delete s_cache;
			s_cache = nullptr;
		}
	}
	{
		std::scoped_lock lock(m_pipelineIsCachedLock);
		m_pipelineIsCached.clear();
	}
}

struct CachedPipeline
{
	struct ShaderHash
	{
		uint64 baseHash;
		uint64 auxHash;
		bool isPresent{};

		void set(uint64 baseHash, uint64 auxHash)
		{
			this->baseHash = baseHash;
			this->auxHash = auxHash;
			this->isPresent = true;
		}
	};

	ShaderHash vsHash; // includes fetch shader
	ShaderHash gsHash;
	ShaderHash psHash;

	Latte::GPUCompactedRegisterState gpuState;
	uint64 cacheGeneration{};
};

VkFormat __getColorBufferVkFormat(const uint32 index, const LatteContextRegister& lcr)
{
	Latte::E_GX2SURFFMT colorBufferFormat = LatteMRT::GetColorBufferFormat(index, lcr);
	VulkanRenderer::FormatInfoVK texFormatInfo;
	VulkanRenderer::GetInstance()->GetTextureFormatInfoVK(colorBufferFormat, false, Latte::E_DIM::DIM_2D, 1280, 720, &texFormatInfo);
	return texFormatInfo.vkImageFormat;
}

void __getDepthBufferVkFormat(const LatteContextRegister& lcr, VkFormat& dbFormat, bool& hasStencil)
{
	Latte::E_GX2SURFFMT format = LatteMRT::GetDepthBufferFormat(lcr);
	VulkanRenderer::FormatInfoVK texFormatInfo;
	VulkanRenderer::GetInstance()->GetTextureFormatInfoVK(format, true, Latte::E_DIM::DIM_2D, 1280, 720, &texFormatInfo);
	dbFormat = texFormatInfo.vkImageFormat;
	hasStencil = (texFormatInfo.vkImageAspect & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
}

// create placeholder renderpass for cached pipeline
VKRObjectRenderPass* __CreateTemporaryRenderPass(const LatteDecompilerShader* pixelShader, const LatteContextRegister& lcr)
{
	VKRObjectRenderPass::AttachmentInfo_t attachmentInfo;

	uint8 cbMask = LatteMRT::GetActiveColorBufferMask(pixelShader, lcr);
	bool dbMask = LatteMRT::GetActiveDepthBufferMask(lcr);

	for (int i = 0; i < 8; ++i)
	{
		if ((cbMask & (1 << i)) == 0)
		{
			attachmentInfo.colorAttachment[i].viewObj = nullptr;
			continue;
		}
		// setup color attachment
		attachmentInfo.colorAttachment[i].viewObj = nullptr;
		attachmentInfo.colorAttachment[i].isPresent = true;
		attachmentInfo.colorAttachment[i].format = __getColorBufferVkFormat(i, lcr);
	}

	// setup depth attachment
	if (dbMask)
	{
		attachmentInfo.depthAttachment.viewObj = nullptr;
		attachmentInfo.depthAttachment.isPresent = true;
		VkFormat dbFormat;
		bool hasStencil;
		__getDepthBufferVkFormat(lcr, dbFormat, hasStencil);
		attachmentInfo.depthAttachment.format = dbFormat;
		attachmentInfo.depthAttachment.hasStencil = hasStencil;
	}
	else
	{
		// no depth attachment
		attachmentInfo.depthAttachment.viewObj = nullptr;
		attachmentInfo.depthAttachment.isPresent = false;
	}

	return new VKRObjectRenderPass(attachmentInfo);
}

void VulkanPipelineStableCache::LoadPipelineFromCache(std::span<uint8> fileData)
{
	static FSpinlock s_spinlockSharedInternal;

	// deserialize file
	LatteContextRegister* lcr = new LatteContextRegister();
	s_spinlockSharedInternal.lock();
	CachedPipeline* cachedPipeline = new CachedPipeline();
	s_spinlockSharedInternal.unlock();
	const auto releaseCachedState = [&]() {
		s_spinlockSharedInternal.lock();
		delete lcr;
		delete cachedPipeline;
		s_spinlockSharedInternal.unlock();
	};

	MemStreamReader streamReader(fileData.data(), fileData.size());
	if (!DeserializePipeline(streamReader, *cachedPipeline))
	{
		releaseCachedState();
		return;
	}
	// restored register view from compacted state
	Latte::LoadGPURegisterState(*lcr, cachedPipeline->gpuState);

	LatteDecompilerShader* vertexShader = nullptr;
	LatteDecompilerShader* geometryShader = nullptr;
	LatteDecompilerShader* pixelShader = nullptr;
	// find vertex shader
	if (cachedPipeline->vsHash.isPresent)
	{
		vertexShader = LatteSHRC_FindVertexShader(cachedPipeline->vsHash.baseHash, cachedPipeline->vsHash.auxHash);
		if (!vertexShader)
		{
			cemuLog_logDebug(LogType::Force, "Vertex shader not found in cache");
			releaseCachedState();
			return;
		}
	}
	// find geometry shader
	if (cachedPipeline->gsHash.isPresent)
	{
		geometryShader = LatteSHRC_FindGeometryShader(cachedPipeline->gsHash.baseHash, cachedPipeline->gsHash.auxHash);
		if (!geometryShader)
		{
			cemuLog_logDebug(LogType::Force, "Geometry shader not found in cache");
			releaseCachedState();
			return;
		}
	}
	// find pixel shader
	if (cachedPipeline->psHash.isPresent)
	{
		pixelShader = LatteSHRC_FindPixelShader(cachedPipeline->psHash.baseHash, cachedPipeline->psHash.auxHash);
		if (!pixelShader)
		{
			cemuLog_logDebug(LogType::Force, "Pixel shader not found in cache");
			releaseCachedState();
			return;
		}
	}
	// create temporary renderpass
	if (!vertexShader || !pixelShader)
	{
		releaseCachedState();
		return;
	}
	auto renderPass = __CreateTemporaryRenderPass(pixelShader, *lcr);
	PipelineInfo* pipelineInfo = new PipelineInfo(0, 0, vertexShader->compatibleFetchShader, vertexShader, pixelShader, geometryShader);
	// compile
	{
		PipelineCompiler pipelineCompiler;
		bool requiresRobustBufferAccess = PipelineCompiler::CalcRobustBufferAccessRequirement(vertexShader, pixelShader, geometryShader);
		if (!pipelineCompiler.InitFromCurrentGPUState(pipelineInfo, *lcr, renderPass, requiresRobustBufferAccess))
		{
			s_spinlockSharedInternal.lock();
			delete pipelineInfo;
			delete lcr;
			delete cachedPipeline;
			VulkanRenderer::GetInstance()->ReleaseDestructibleObject(renderPass);
			s_spinlockSharedInternal.unlock();
			return;
		}
		pipelineCompiler.Compile(true, true, false);
	}
	// on success, calculate pipeline hash and flag as present in cache
	if (pipelineInfo->m_vkrObjPipeline->GetPipeline() != VK_NULL_HANDLE)
	{
		uint64 pipelineBaseHash = vertexShader->baseHash;
		uint64 pipelineStateHash = VulkanRenderer::draw_calculateGraphicsPipelineHash(vertexShader->compatibleFetchShader, vertexShader, geometryShader, pixelShader, renderPass, *lcr);
		std::scoped_lock lock(m_pipelineIsCachedLock);
		m_pipelineIsCached.emplace(pipelineBaseHash, pipelineStateHash);
	}
	// clean up
	s_spinlockSharedInternal.lock();
	delete pipelineInfo;
	delete lcr;
	delete cachedPipeline;
	VulkanRenderer::GetInstance()->ReleaseDestructibleObject(renderPass);
	s_spinlockSharedInternal.unlock();
}

bool VulkanPipelineStableCache::HasPipelineCached(uint64 baseHash, uint64 pipelineStateHash)
{
	PipelineHash ph(baseHash, pipelineStateHash);
	std::scoped_lock lock(m_pipelineIsCachedLock);
	return m_pipelineIsCached.find(ph) != m_pipelineIsCached.end();
}

void VulkanPipelineStableCache::AddCurrentStateToCache(uint64 baseHash, uint64 pipelineStateHash)
{
	{
		std::scoped_lock lock(m_pipelineIsCachedLock);
		m_pipelineIsCached.emplace(baseHash, pipelineStateHash);
	}
	if (!m_pipelineCacheStoreThreadStarted)
	{
		m_pipelineCacheStoreThread = std::thread(&VulkanPipelineStableCache::WorkerThread, this);
		m_pipelineCacheStoreThreadStarted = true;
	}
	// fill job structure with cached GPU state
	// for each cached pipeline we store:
	// - Active shaders (referenced by hash)
	// - An almost-complete register state of the GPU (minus some ALU uniform constants which aren't relevant)
	auto job = std::make_unique<CachedPipeline>();
	auto vs = LatteSHRC_GetActiveVertexShader();
	auto gs = LatteSHRC_GetActiveGeometryShader();
	auto ps = LatteSHRC_GetActivePixelShader();
	if (vs)
		job->vsHash.set(vs->baseHash, vs->auxHash);
	if (gs)
		job->gsHash.set(gs->baseHash, gs->auxHash);
	if (ps)
		job->psHash.set(ps->baseHash, ps->auxHash);
	Latte::StoreGPURegisterState(LatteGPUState.contextNew, job->gpuState);
	{
		std::scoped_lock lock(m_cacheMutex);
		job->cacheGeneration = m_cacheGeneration;
	}
	// queue job
	m_pipelineCachingQueue.push(job.release());
}

bool VulkanPipelineStableCache::SerializePipeline(MemStreamWriter& memWriter, CachedPipeline& cachedPipeline)
{
	memWriter.writeBE<uint8>(0x01); // version
	uint8 presentMask = 0;
	if (cachedPipeline.vsHash.isPresent)
		presentMask |= 1;
	if (cachedPipeline.gsHash.isPresent)
		presentMask |= 2;
	if (cachedPipeline.psHash.isPresent)
		presentMask |= 4;
	memWriter.writeBE<uint8>(presentMask);
	if (cachedPipeline.vsHash.isPresent)
	{
		memWriter.writeBE<uint64>(cachedPipeline.vsHash.baseHash);
		memWriter.writeBE<uint64>(cachedPipeline.vsHash.auxHash);
	}
	if (cachedPipeline.gsHash.isPresent)
	{
		memWriter.writeBE<uint64>(cachedPipeline.gsHash.baseHash);
		memWriter.writeBE<uint64>(cachedPipeline.gsHash.auxHash);
	}
	if (cachedPipeline.psHash.isPresent)
	{
		memWriter.writeBE<uint64>(cachedPipeline.psHash.baseHash);
		memWriter.writeBE<uint64>(cachedPipeline.psHash.auxHash);
	}
	Latte::SerializeRegisterState(cachedPipeline.gpuState, memWriter);
	return true;
}

bool VulkanPipelineStableCache::DeserializePipeline(MemStreamReader& memReader, CachedPipeline& cachedPipeline)
{
	// version
	if (memReader.readBE<uint8>() != 1)
	{
		cemuLog_log(LogType::Force, "Cached Vulkan pipeline corrupted or has unknown version");
		return false;
	}
	// shader hashes
	uint8 presentMask = memReader.readBE<uint8>();
	if (memReader.hasError() || (presentMask & ~uint8{7}) != 0)
		return false;
	if (presentMask & 1)
	{
		uint64 baseHash = memReader.readBE<uint64>();
		uint64 auxHash = memReader.readBE<uint64>();
		cachedPipeline.vsHash.set(baseHash, auxHash);
	}
	if (presentMask & 2)
	{
		uint64 baseHash = memReader.readBE<uint64>();
		uint64 auxHash = memReader.readBE<uint64>();
		cachedPipeline.gsHash.set(baseHash, auxHash);
	}
	if (presentMask & 4)
	{
		uint64 baseHash = memReader.readBE<uint64>();
		uint64 auxHash = memReader.readBE<uint64>();
		cachedPipeline.psHash.set(baseHash, auxHash);
	}
	// deserialize GPU state
	if (!Latte::DeserializeRegisterState(cachedPipeline.gpuState, memReader))
	{
		return false;
	}
	return !memReader.hasError();
}

int VulkanPipelineStableCache::CompilerThread()
{
	SetThreadName("plCacheCompiler");
#if defined(__SWITCH__)
	SwitchThread_AllowHelperCore("pipeline cache compiler");
#endif
	while (m_numCompilationThreads != 0)
	{
		std::vector<uint8> pipelineData = m_compilationQueue.pop();
		m_loadingCv.notify_one();
		if(pipelineData.empty())
			continue;
		LoadPipelineFromCache(pipelineData);
		++g_vkCacheState.pipelinesLoaded;
		m_loadingCv.notify_all();
	}
	return 0;
}

void VulkanPipelineStableCache::WorkerThread()
{
	SetThreadName("plCacheWriter");
#if defined(__SWITCH__)
	SwitchThread_AllowHelperCore("pipeline cache writer");
#endif
	while (true)
	{
		CachedPipeline* job = m_pipelineCachingQueue.pop();
		if (!job)
			break;
		// serialize
		MemStreamWriter memWriter(1024 * 4);
		SerializePipeline(memWriter, *job);
		auto blob = memWriter.getResult();
		// file name is derived from data hash
		uint8 hash[SHA256_DIGEST_LENGTH];
		SHA256(blob.data(), blob.size(), hash);
		uint64be nameABe;
		uint64be nameBBe;
		std::memcpy(&nameABe, hash, sizeof(nameABe));
		std::memcpy(&nameBBe, hash + sizeof(nameABe), sizeof(nameBBe));
		const uint64 nameA = nameABe;
		const uint64 nameB = nameBBe;
		{
			std::scoped_lock lock(m_cacheMutex);
			if (s_cache && job->cacheGeneration == m_cacheGeneration)
				s_cache->AddFileAsync({ nameA, nameB }, blob.data(), blob.size());
		}
		delete job;
	}
}
