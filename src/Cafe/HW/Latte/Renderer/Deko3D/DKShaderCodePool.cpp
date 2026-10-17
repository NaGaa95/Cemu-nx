#include "DKShaderCodePool.h"
#include "DKCommandBufferManager.h"
#include "DKContext.h"
#include "DKMemoryTracker.h"
#include "util/ChunkedHeap/ChunkedHeap.h"

#include <limits>

namespace Deko3D
{
	namespace
	{
		constexpr u32 BLOCK_SIZE = 2 * 1024 * 1024;
		constexpr u32 BLOCK_ALIGNMENT = 64 * 1024;
	}

	struct DKShaderCode::Block
	{
		explicit Block(u32 size) : heap(nullptr, size - DK_SHADER_CODE_UNUSABLE_SIZE)
		{
			memory = dk::MemBlockMaker{g_dk_context->GetDevice(), size}
				.setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code).create();
			if (!memory)
				throw std::bad_alloc();
			MemoryTracker::RegisterMemBlock(memory, "shader code pool");
			std::memset(static_cast<u8*>(memory.getCpuAddr()) + size - DK_SHADER_CODE_UNUSABLE_SIZE,
				0, DK_SHADER_CODE_UNUSABLE_SIZE);
		}

		~Block()
		{
			cemu_assert_debug(!heap.hasAllocations());
			MemoryTracker::UnregisterMemBlock(memory);
			DeferMemBlockDestruction(std::move(memory));
		}

		bool Allocate(u32 bytes, u32& offset)
		{
			std::lock_guard lock(mutex);
			if (!heap.allocOffset(AlignUp(bytes, DK_SHADER_CODE_ALIGNMENT), DK_SHADER_CODE_ALIGNMENT, offset))
				return false;
			return true;
		}

		void Free(u32 offset)
		{
			std::lock_guard lock(mutex);
			heap.freeOffset(offset);
		}

		dk::UniqueMemBlock memory;
		std::mutex mutex;
		VHeap heap;
	};

	DKShaderCode::~DKShaderCode()
	{
		if (!m_block)
			return;
		auto release = [block = std::move(m_block), offset = m_offset] {
			block->Free(offset);
		};
		if (!m_needsInvalidate && g_dk_command_buffer_mgr)
			g_dk_command_buffer_mgr->DeferCleanup(std::move(release));
		else
			release();
	}

	DkMemBlock DKShaderCode::GetMemory() const
	{
		return m_block->memory;
	}

	void DKShaderCode::PrepareForUse(DkCmdBuf commands) const
	{
		if (m_needsInvalidate)
		{
			dkCmdBufBarrier(commands, DkBarrier_None, DkInvalidateFlags_Shader | DkInvalidateFlags_L2Cache);
			m_needsInvalidate = false;
		}
	}

	std::unique_ptr<DKShaderCode> DKShaderCodePool::Upload(std::span<const u8> code)
	{
		if (code.empty() || code.size() > std::numeric_limits<u32>::max() - BLOCK_ALIGNMENT - DK_SHADER_CODE_UNUSABLE_SIZE)
			throw std::invalid_argument("Invalid Deko3D shader code size");
		const u64 required = AlignUp(static_cast<u64>(code.size()), DK_SHADER_CODE_ALIGNMENT) + DK_SHADER_CODE_UNUSABLE_SIZE;
		const u32 bytes = static_cast<u32>(code.size());
		auto result = std::unique_ptr<DKShaderCode>(new DKShaderCode());
		std::lock_guard lock(m_mutex);
		for (const auto& block : m_blocks)
		{
			if (block->Allocate(bytes, result->m_offset))
			{
				result->m_block = block;
				break;
			}
		}
		if (!result->m_block)
		{
			auto block = std::make_shared<DKShaderCode::Block>(std::max(BLOCK_SIZE,
				static_cast<u32>(AlignUp(required, BLOCK_ALIGNMENT))));
			m_blocks.push_back(block);
			if (!block->Allocate(bytes, result->m_offset))
				throw std::bad_alloc();
			result->m_block = std::move(block);
		}
		std::memcpy(static_cast<u8*>(result->m_block->memory.getCpuAddr()) + result->m_offset, code.data(), code.size());
		return result;
	}
} // namespace Deko3D
