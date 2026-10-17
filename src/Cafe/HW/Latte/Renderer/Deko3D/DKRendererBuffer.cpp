#include "DKRendererInternal.h"
#include "DKMemoryTracker.h"
#include "Cafe/HW/Latte/Core/LatteBufferCache.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/MMU/MMU.h"
#include "platform/switch/SwitchMemory.h"
#include "platform/switch/common/SwitchLaunchPolicy.h"
#include "util/MemMapper/MemMapper.h"

using namespace Deko3D;

void Deko3DRenderer::bufferCache_init(sint32 size)
{
	auto& state = *m_state;
	LatteBufferCache_setHostMemorySync(0, 0);
	if (!SwitchMemory_CanSynchronizeForGpu())
		throw SwitchLaunch::Failure(SwitchLaunch::Error::HostMemoryUnavailable, "Kernel cache synchronization unavailable");
	state.importedBase = mmuRange_MEM2.getBase();
	state.importedSize = mmuRange_MEM2.getSize();
	auto* memory = memory_getPointerFromVirtualOffset(state.importedBase);
	if ((reinterpret_cast<uintptr_t>(memory) & (DK_MEMBLOCK_ALIGNMENT - 1)) ||
		!state.importedSize || (state.importedSize & (DK_MEMBLOCK_ALIGNMENT - 1)))
		throw SwitchLaunch::Failure(SwitchLaunch::Error::HostMemoryUnavailable, "MEM2 does not satisfy the Deko3D import alignment");
	state.zeroBuffer = dk::MemBlockMaker{g_dk_context->GetDevice(), DK_MEMBLOCK_ALIGNMENT}
		.setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_ZeroFillInit).create();
	if (!state.zeroBuffer)
		throw std::bad_alloc();
	MemoryTracker::RegisterMemBlock(state.zeroBuffer, "null vertex and uniform data");
	state.importedMemory = dk::MemBlockMaker{g_dk_context->GetDevice(), state.importedSize}
		.setStorage(memory).setFlags(DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuCached).create();
	if (!state.importedMemory)
		throw SwitchLaunch::Failure(SwitchLaunch::Error::HostMemoryUnavailable, "Deko3D could not import MEM2");
	MemoryTracker::RegisterMemBlock(state.importedMemory, "imported Wii U MEM2");
	SwitchMemory_CleanForGpu(memory, state.importedSize);
	LatteBufferCache_setHostMemorySync(state.importedBase, state.importedSize);
	const u32 cacheSize = AlignUp(std::min(static_cast<u32>(size), 64u * 1024 * 1024), DK_MEMBLOCK_ALIGNMENT);
	state.guestBuffers = std::make_unique<DKGuestBufferCache>(cacheSize);
	cemuLog_log(LogType::Force, "Deko3D: imported {} MB of Wii U MEM2; {} MB versioned buffer snapshots; {}",
		state.importedSize / (1024 * 1024), cacheSize / (1024 * 1024), MemMapper::DescribeGuestBacking());
}

bool Deko3DRenderer::State::IsImportedRange(MPTR address, uint64 size) const
{
	return address >= importedBase && address - importedBase < importedSize &&
		size <= importedSize - (address - importedBase);
}

void Deko3DRenderer::bufferCache_invalidate(uint32 address, uint32 size)
{
	if (m_state->guestBuffers)
		m_state->guestBuffers->InvalidateSnapshots(address, size);
}

DkBufExtents Deko3DRenderer::State::GuestBuffer(MPTR address, uint32 size, bool uniform)
{
	if (!address || !size)
		return {};
	const u32 alignment = uniform ? DK_UNIFORM_BUF_ALIGNMENT : 4;
	const u64 padded = AlignUp(static_cast<u64>(size), alignment);
	if (padded > std::numeric_limits<u32>::max() ||
		static_cast<u64>(address) + size > 0x1'0000'0000ULL)
		throw std::runtime_error("Deko3D guest buffer exceeds the Wii U address space");
	if (guestReadbackFence > g_dk_command_buffer_mgr->GetCompletedFenceCounter())
	{
		if (!uniform)
		{
			if (const auto source = guestBuffers->GpuSource(address, size, snapshotEpoch))
			{
				streamoutReadBegin = std::min<uint64>(streamoutReadBegin, source->addr);
				streamoutReadEnd = std::max<uint64>(streamoutReadEnd, source->addr + source->size);
				return *source;
			}
		}
		if (const u64 fence = guestBuffers->PendingWriteFence(address, size))
		{
			if (fence == g_dk_command_buffer_mgr->GetCurrentFenceCounter())
				Submit(false);
			g_dk_command_buffer_mgr->WaitForFenceCounter(fence);
			PollGuestCompletions();
		}
	}
	const std::span<const u8> bytes{memory_getPointerFromVirtualOffset(address), size};
	bool uploaded = false;
	auto snapshot = guestBuffers->Get(address, bytes, snapshotEpoch, LatteGPUState.drawCallCounter, uploaded);
	if (!snapshot && size <= guestBuffers->Capacity() && guestBuffers->HasPendingAllocations())
	{
		Submit(true);
		snapshot = guestBuffers->Get(address, bytes, snapshotEpoch, LatteGPUState.drawCallCounter, uploaded);
	}
	if (uploaded)
		bufferReadsDirty = true;
	if (snapshot)
		return *snapshot;
	const auto allocation = Allocate(static_cast<u32>(padded), alignment);
	std::memcpy(allocation.cpu, bytes.data(), size);
	std::memset(static_cast<u8*>(allocation.cpu) + size, 0, padded - size);
	bufferReadsDirty = true;
	return {allocation.gpu, size};
}

void Deko3DRenderer::State::UpdateGuestBuffers(uint32 maxVertex, uint32 baseInstance, uint32 instanceCount)
{
	LatteBufferCache_processDCFlushQueue([&](MPTR address, u32 size) {
		guestBuffers->Invalidate(address, size);
	}, false);
	const auto* fetch = LatteSHRC_GetActiveFetchShader();
	const u64 maxInstance = static_cast<u64>(baseInstance) + instanceCount - 1;
	for (const auto& group : fetch->bufferGroups)
	{
		const u32 index = group.attributeBufferIndex;
		const auto* registers = LatteGPUState.contextRegister + mmSQ_VTX_ATTRIBUTE_BLOCK_START + index * 7;
		const u32 stride = (registers[2] >> 11) & 0xffff;
		if (vertexStrides[index] != stride)
			dirty = true;
		vertexStrides[index] = stride;
		const u64 last = std::max(group.hasVtxIndexAccess ? static_cast<u64>(maxVertex) : 0,
			group.hasInstanceIndexAccess ? maxInstance : 0);
		const u64 bytes = last * stride + group.totalAttribRangeSize;
		if (bytes > std::numeric_limits<u32>::max())
			throw std::runtime_error("Deko3D vertex buffer range exceeds the hardware limit");
		vertexBuffers[index] = GuestBuffer(registers[0], static_cast<u32>(bytes), false);
	}
	LatteDecompilerShader* shaders[]{LatteSHRC_GetActiveVertexShader(), LatteSHRC_GetActivePixelShader(), LatteSHRC_GetActiveGeometryShader()};
	constexpr u32 registerBases[]{mmSQ_VTX_UNIFORM_BLOCK_START, mmSQ_PS_UNIFORM_BLOCK_START, mmSQ_GS_UNIFORM_BLOCK_START};
	for (u32 stage = 0; stage < stages.size(); ++stage)
	{
		const auto* shader = shaders[stage];
		if (!shader || shader->uniformMode != LATTE_DECOMPILER_UNIFORM_MODE_FULL_CBANK)
			continue;
		for (const auto& bank : shader->list_quickBufferList)
		{
			const auto* registers = LatteGPUState.contextRegister + registerBases[stage] + bank.index * 7;
			const u64 size = std::min(static_cast<u64>(registers[1]) + 1, static_cast<u64>(bank.size));
			if (size > DK_UNIFORM_BUF_MAX_SIZE)
				throw std::runtime_error("Deko3D uniform buffer exceeds the hardware limit");
			stages[stage].banks[bank.index] = GuestBuffer(registers[0], static_cast<u32>(size), true);
		}
	}
}

void Deko3DRenderer::State::SyncGuestBuffers()
{
	if (bufferReadsDirty)
	{
		g_dk_command_buffer_mgr->NotifyCpuUpload();
		bufferReadsDirty = false;
	}
}

void Deko3DRenderer::bufferCache_upload(uint8*, sint32, uint32)
{
	throw std::logic_error("Deko3D uses versioned guest buffers");
}

void Deko3DRenderer::bufferCache_copy(uint32, uint32, uint32)
{
	throw std::logic_error("Deko3D uses versioned guest buffers");
}

void Deko3DRenderer::bufferCache_copyStreamoutToMainBuffer(uint32 source, uint32 destination, uint32 size)
{
	if (!m_state->streamout)
		throw std::runtime_error("Streamout copy without a streamout buffer");
	if (!size)
		return;
	if (source > m_state->streamout.getSize() || size > m_state->streamout.getSize() - source ||
		static_cast<u64>(destination) + size > 0x1'0000'0000ULL)
		throw std::runtime_error("Deko3D streamout range exceeds its allocation");
	auto* target = memory_getPointerFromVirtualOffset(destination);
	DkGpuAddr destinationGpu;
	if (m_state->IsImportedRange(destination, size))
	{
		SwitchMemory_FlushForGpuWrite(target, size);
		destinationGpu = m_state->importedMemory.getGpuAddr() + destination - m_state->importedBase;
		g_dk_command_buffer_mgr->DeferCleanup([target, size] { SwitchMemory_InvalidateFromGpu(target, size); });
		m_state->bufferReadsDirty = true;
	}
	else
	{
		const auto readback = m_state->Reserve(*m_state->dynamic, size, 4);
		destinationGpu = readback.gpu;
		g_dk_command_buffer_mgr->DeferCleanup([target, readback, size] { std::memcpy(target, readback.cpu, size); });
	}
	m_state->Barrier(DkBarrier_Full, DkInvalidateFlags_Shader);
	dkCmdBufCopyBuffer(m_state->Cmd(), m_state->streamout.getGpuAddr() + source,
		destinationGpu, size);
	m_state->Barrier(DkBarrier_Full, DkInvalidateFlags_Shader | DkInvalidateFlags_L2Cache);
	m_state->bufferReadsDirty = false;
	g_dk_command_buffer_mgr->NotifyCpuReadback();
	m_state->guestReadbackFence = g_dk_command_buffer_mgr->GetCurrentFenceCounter();
	m_state->guestBuffers->TrackGpuWrite(destination, size, m_state->guestReadbackFence,
		{m_state->streamout.getGpuAddr() + source, size}, m_state->snapshotEpoch);
}

void Deko3DRenderer::buffer_bindVertexBuffers(std::span<BindBufferParam>)
{
	throw std::logic_error("Deko3D binds versioned vertex buffers");
}

void Deko3DRenderer::buffer_bindUniformBuffer(LatteConst::ShaderType, uint32, uint32, uint32)
{
	throw std::logic_error("Deko3D binds versioned uniform buffers");
}

Renderer::IndexAllocation Deko3DRenderer::indexData_reserveIndexMemory(uint32 size)
{
	auto* reservation = m_state->indexBuffers.Reserve(size);
	return {reservation->cached.data(), reservation};
}

void Deko3DRenderer::indexData_releaseIndexMemory(IndexAllocation& allocation)
{
	m_state->indexBuffers.Release(static_cast<IndexReservation*>(allocation.rendererInternal));
	allocation = {};
}

void Deko3DRenderer::indexData_uploadIndexMemory(IndexAllocation&) {}

void Deko3DRenderer::streamout_setupXfbBuffer(uint32 index, sint32 offset, uint32, uint32 size)
{
	if (!m_state->streamout)
	{
		m_state->streamout = dk::MemBlockMaker{g_dk_context->GetDevice(), static_cast<u32>(LatteStreamout_GetRingBufferSize())}
								 .setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_ZeroFillInit)
								 .create();
		if (!m_state->streamout)
			throw std::bad_alloc();
		MemoryTracker::RegisterMemBlock(m_state->streamout, "streamout");
	}
	if (offset < 0 || static_cast<u32>(offset) > m_state->streamout.getSize() ||
		size > m_state->streamout.getSize() - static_cast<u32>(offset))
		throw std::runtime_error("Deko3D streamout allocation exceeds its buffer");
	if (size)
	{
		const DkBufExtents region{m_state->streamout.getGpuAddr() + offset, size};
		if (region.addr < m_state->streamoutReadEnd && region.addr + size > m_state->streamoutReadBegin)
		{
			m_state->Barrier(DkBarrier_Full, DkInvalidateFlags_Shader | DkInvalidateFlags_L2Cache);
			m_state->streamoutReadBegin = ~0ULL;
			m_state->streamoutReadEnd = 0;
		}
		m_state->guestBuffers->InvalidateGpuSource(region);
	}
	m_state->streamoutOffsets[index] = offset;
}

void Deko3DRenderer::streamout_begin() {}
void Deko3DRenderer::streamout_rendererFinishDrawcall()
{
	m_state->Barrier(DkBarrier_Primitives, DkInvalidateFlags_Shader);
}
