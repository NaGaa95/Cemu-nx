// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Cafe/HW/Latte/Renderer/Deko3D/DKCommandBufferManager.h"

#include <algorithm>
#include <utility>

#include "Cafe/HW/Latte/Renderer/Deko3D/DKContext.h"
#include "Cafe/HW/Latte/Renderer/Deko3D/DKMemoryTracker.h"
#include "Cafe/HW/Latte/Renderer/Deko3D/DKSwapChain.h"

namespace Deko3D
{
	std::unique_ptr<DKCommandBufferManager> g_dk_command_buffer_mgr;

	void DKCommandBufferManager::CommandMemory::Rewind()
	{
		cmdbuf.clear();
		cmdbuf.addMemory(initial_block, initial_offset, initial_size);
		next_growth_chunk = 0;
	}

	void DKCommandBufferManager::CommandMemory::Grow(size_t min_req_size)
	{
		const u32 size = static_cast<u32>(AlignUp(
			std::max<size_t>(min_req_size, COMMAND_BUFFER_GROWTH_SIZE), DK_MEMBLOCK_ALIGNMENT));

		if (next_growth_chunk == growth_chunks.size())
			growth_chunks.push_back(std::make_unique<GrowthChunk>());

		GrowthChunk& chunk = *growth_chunks[next_growth_chunk];
		if (!chunk.block || chunk.size < size)
		{
			MemoryTracker::UnregisterMemBlock(chunk.block);
			chunk.block = dk::MemBlockMaker{g_dk_context->GetDevice(), size}
							  .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
							  .create();
			if (!chunk.block)
				throw std::bad_alloc();
			chunk.size = size;
			MemoryTracker::RegisterMemBlock(chunk.block, "command memory growth chunk");
		}

		cmdbuf.addMemory(chunk.block, 0, chunk.size);
		++next_growth_chunk;
	}

	DKCommandBufferManager::DKCommandBufferManager() = default;

	DKCommandBufferManager::~DKCommandBufferManager()
	{
		if (g_dk_context)
			g_dk_context->WaitIdle();

		m_shuttingDown = true;
		MemoryTracker::UnregisterMemBlock(m_command_memory);
		for (CmdBufferResources& resources : m_command_buffers)
		{
			for (CommandMemory* memory : {&resources.init, &resources.draw})
				for (auto& chunk : memory->growth_chunks)
					MemoryTracker::UnregisterMemBlock(chunk->block);
			for (auto& cleanup : resources.cleanup_resources)
				cleanup();
			resources.cleanup_resources.clear();
		}
	}

	bool DKCommandBufferManager::Initialize()
	{
		if (!CreateCommandBuffers())
			return false;

		m_command_buffers[0].fence_counter = m_next_fence_counter++;

		return true;
	}

	void DKCommandBufferManager::AddMemoryCallback(void* user_data, DkCmdBuf,
												   size_t min_req_size)
	{
		static_cast<CommandMemory*>(user_data)->Grow(min_req_size);
	}

	bool DKCommandBufferManager::CreateCommandBuffers()
	{
		DkDevice device = g_dk_context->GetDevice();

		const u32 total_size = static_cast<u32>(AlignUp(
			static_cast<u64>(INIT_COMMAND_BUFFER_SIZE + DRAW_COMMAND_BUFFER_SIZE) * NUM_COMMAND_BUFFERS,
			DK_MEMBLOCK_ALIGNMENT));

		m_command_memory = dk::MemBlockMaker{device, total_size}
							   .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
							   .create();
		if (!m_command_memory)
		{
			cemuLog_log(LogType::Force, "deko3d: failed to allocate {} bytes of command memory", total_size);
			return false;
		}

		MemoryTracker::RegisterMemBlock(m_command_memory, "command memory");

		u32 offset = 0;
		const auto create = [&](CommandMemory& mem, u32 size) {
			mem.cmdbuf = dk::CmdBufMaker{device}
							 .setUserData(&mem)
							 .setCbAddMem(&DKCommandBufferManager::AddMemoryCallback)
							 .create();
			if (!mem.cmdbuf)
				return false;

			mem.initial_block = m_command_memory;
			mem.initial_offset = offset;
			mem.initial_size = size;
			mem.cmdbuf.addMemory(m_command_memory, offset, size);
			offset += size;
			return true;
		};

		for (CmdBufferResources& resources : m_command_buffers)
		{
			if (!create(resources.init, INIT_COMMAND_BUFFER_SIZE) ||
				!create(resources.draw, DRAW_COMMAND_BUFFER_SIZE))
			{
				cemuLog_log(LogType::Force, "deko3d: failed to create command buffer");
				return false;
			}
		}

		return true;
	}

	DkCmdBuf DKCommandBufferManager::GetCurrentInitCommandBuffer()
	{
		CmdBufferResources& resources = m_command_buffers[m_current_cmd_buffer];
		resources.init_cmdbuf_used = true;
		return resources.init.cmdbuf;
	}

	DkCmdBuf DKCommandBufferManager::GetCurrentCommandBuffer() const
	{
		return m_command_buffers[m_current_cmd_buffer].draw.cmdbuf;
	}

	u64 DKCommandBufferManager::GetCurrentFenceCounter() const
	{
		return m_command_buffers[m_current_cmd_buffer].fence_counter;
	}

	void DKCommandBufferManager::DeferCleanup(std::function<void()> cleanup)
	{
		std::unique_lock lock(m_cleanupMutex);
		if (m_shuttingDown)
		{
			lock.unlock();
			cleanup();
			return;
		}
		m_command_buffers[m_current_cmd_buffer].cleanup_resources.push_back(std::move(cleanup));
	}

	void DKCommandBufferManager::PollCompletedFences()
	{
		for (u32 step = 1; step < NUM_COMMAND_BUFFERS; ++step)
		{
			const u32 index = (m_current_cmd_buffer + step) % NUM_COMMAND_BUFFERS;
			auto& resources = m_command_buffers[index];
			if (resources.fence_counter <= m_completed_fence_counter)
				continue;
			if (dkFenceWait(&resources.fence, 0) != DkResult_Success)
				break;
			WaitForCommandBufferCompletion(index);
		}
	}

	void DKCommandBufferManager::NotifyCpuReadback()
	{
		m_command_buffers[m_current_cmd_buffer].needs_cpu_readback = true;
	}

	void DKCommandBufferManager::NotifyCpuUpload()
	{
		m_command_buffers[m_current_cmd_buffer].needs_cpu_upload = true;
	}

	void DKCommandBufferManager::SubmitCommandBuffer(bool wait_for_completion,
													 DKSwapChain* present_swap_chain, int present_slot)
	{
		CmdBufferResources& resources = m_command_buffers[m_current_cmd_buffer];
		dk::Queue queue = g_dk_context->GetGraphicsQueue();
		if (resources.needs_cpu_upload)
		{
			constexpr u32 flags = DkInvalidateFlags_Shader | DkInvalidateFlags_L2Cache;
			dkCmdBufBarrier(GetCurrentInitCommandBuffer(), DkBarrier_None, flags);
		}
		if (resources.init_cmdbuf_used)
			queue.submitCommands(resources.init.cmdbuf.finishList());

		queue.submitCommands(resources.draw.cmdbuf.finishList());

		queue.signalFence(resources.fence, resources.needs_cpu_readback);

		if (present_swap_chain)
			present_swap_chain->Present(present_slot);
		else
			queue.flush();

		if (wait_for_completion)
			WaitForCommandBufferCompletion(m_current_cmd_buffer);

		BeginCommandBuffer();
	}

	void DKCommandBufferManager::BeginCommandBuffer()
	{
		const u32 next_buffer_index = (m_current_cmd_buffer + 1) % NUM_COMMAND_BUFFERS;
		CmdBufferResources& resources = m_command_buffers[next_buffer_index];

		if (resources.fence_counter > m_completed_fence_counter)
			WaitForCommandBufferCompletion(next_buffer_index);

		resources.init.Rewind();
		resources.draw.Rewind();
		resources.init_cmdbuf_used = false;
		resources.needs_cpu_readback = false;
		resources.needs_cpu_upload = false;
		resources.fence_counter = m_next_fence_counter++;

		{
			std::lock_guard lock(m_cleanupMutex);
			m_current_cmd_buffer = next_buffer_index;
		}
	}

	void DKCommandBufferManager::WaitForCommandBufferCompletion(u32 index)
	{
		CmdBufferResources& resources = m_command_buffers[index];

		const DkResult res = dkFenceWait(&resources.fence, -1);

		if (res != DkResult_Success)
			throw std::runtime_error(fmt::format("Deko3D fence wait failed ({})", static_cast<int>(res)));

		const u64 now_completed_counter = resources.fence_counter;
		for (u32 step = 1; step <= NUM_COMMAND_BUFFERS; ++step)
		{
			const u32 retired_index = (m_current_cmd_buffer + step) % NUM_COMMAND_BUFFERS;
			CmdBufferResources& retired = m_command_buffers[retired_index];
			if (retired.fence_counter > now_completed_counter)
				continue;

			std::vector<std::function<void()>> cleanup;
			{
				std::lock_guard lock(m_cleanupMutex);
				cleanup.swap(retired.cleanup_resources);
			}
			for (auto& release : cleanup)
				release();
		}

		m_completed_fence_counter = now_completed_counter;
	}

	void DKCommandBufferManager::WaitForFenceCounter(u64 fence_counter)
	{
		if (m_completed_fence_counter >= fence_counter)
			return;

		if (fence_counter >= m_command_buffers[m_current_cmd_buffer].fence_counter)
		{
			SubmitCommandBuffer(true);
			return;
		}

		u32 index = (m_current_cmd_buffer + 1) % NUM_COMMAND_BUFFERS;
		while (index != m_current_cmd_buffer)
		{
			if (m_command_buffers[index].fence_counter >= fence_counter)
				break;

			index = (index + 1) % NUM_COMMAND_BUFFERS;
		}

		cemu_assert(index != m_current_cmd_buffer);
		WaitForCommandBufferCompletion(index);
	}

	void DeferMemBlockDestruction(dk::UniqueMemBlock block)
	{
		if (!block)
			return;

		if (g_dk_command_buffer_mgr)
		{
			auto shared = std::make_shared<dk::UniqueMemBlock>(std::move(block));
			g_dk_command_buffer_mgr->DeferCleanup([shared]() {});
		}
	}
} // namespace Deko3D
