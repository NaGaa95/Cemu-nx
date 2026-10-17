// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <functional>
#include <memory>
#include <vector>

#include <deko3d.hpp>

#include "DKCommon.h"

namespace Deko3D
{
	class DKSwapChain;

	class DKCommandBufferManager
	{
	  public:
		DKCommandBufferManager();
		~DKCommandBufferManager();

		bool Initialize();

		DkCmdBuf GetCurrentInitCommandBuffer();
		DkCmdBuf GetCurrentCommandBuffer() const;

		u64 GetCompletedFenceCounter() const
		{
			return m_completed_fence_counter;
		}
		void PollCompletedFences();
		u64 GetCurrentFenceCounter() const;

		void WaitForFenceCounter(u64 fence_counter);

		void NotifyCpuReadback();
		void NotifyCpuUpload();

		void SubmitCommandBuffer(bool wait_for_completion, DKSwapChain* present_swap_chain = nullptr,
								 int present_slot = -1);

		void DeferCleanup(std::function<void()> cleanup);

	  private:
		struct GrowthChunk
		{
			dk::UniqueMemBlock block;
			u32 size = 0;
		};

		struct CommandMemory
		{
			dk::UniqueCmdBuf cmdbuf;

			DkMemBlock initial_block = nullptr;
			u32 initial_offset = 0;
			u32 initial_size = 0;

			std::vector<std::unique_ptr<GrowthChunk>> growth_chunks;
			size_t next_growth_chunk = 0;

			void Rewind();
			void Grow(size_t min_req_size);
		};

		struct CmdBufferResources
		{
			CommandMemory init;
			CommandMemory draw;

			DkFence fence = {};
			u64 fence_counter = 0;
			bool init_cmdbuf_used = false;
			bool needs_cpu_readback = false;
			bool needs_cpu_upload = false;

			std::vector<std::function<void()>> cleanup_resources;
		};

		static void AddMemoryCallback(void* user_data, DkCmdBuf cmdbuf, size_t min_req_size);

		bool CreateCommandBuffers();
		void BeginCommandBuffer();
		void WaitForCommandBufferCompletion(u32 index);

		std::mutex m_cleanupMutex;
		bool m_shuttingDown = false;
		dk::UniqueMemBlock m_command_memory;

		u64 m_next_fence_counter = 1;
		u64 m_completed_fence_counter = 0;

		std::array<CmdBufferResources, NUM_COMMAND_BUFFERS> m_command_buffers;
		u32 m_current_cmd_buffer = 0;
	};

	extern std::unique_ptr<DKCommandBufferManager> g_dk_command_buffer_mgr;

	void DeferMemBlockDestruction(dk::UniqueMemBlock block);
} // namespace Deko3D
