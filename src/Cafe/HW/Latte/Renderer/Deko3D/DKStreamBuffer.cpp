// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Cafe/HW/Latte/Renderer/Deko3D/DKStreamBuffer.h"

#include "Cafe/HW/Latte/Renderer/Deko3D/DKCommandBufferManager.h"
#include "Cafe/HW/Latte/Renderer/Deko3D/DKContext.h"
#include "Cafe/HW/Latte/Renderer/Deko3D/DKMemoryTracker.h"

namespace Deko3D
{
	DKStreamBuffer::DKStreamBuffer(u32 size, std::string name) : m_size(size), m_name(std::move(name))
	{
	}

	DKStreamBuffer::~DKStreamBuffer()
	{
		MemoryTracker::UnregisterMemBlock(m_memblock);
		DeferMemBlockDestruction(std::move(m_memblock));
	}

	std::unique_ptr<DKStreamBuffer> DKStreamBuffer::Create(u32 size, std::string name)
	{
		std::unique_ptr<DKStreamBuffer> buffer(new DKStreamBuffer(size, std::move(name)));
		if (!buffer->Allocate())
			return nullptr;

		return buffer;
	}

	bool DKStreamBuffer::Allocate()
	{
		m_size = static_cast<u32>(AlignUp(m_size, DK_MEMBLOCK_ALIGNMENT));

		m_memblock = dk::MemBlockMaker{g_dk_context->GetDevice(), m_size}
						 .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
						 .create();
		if (!m_memblock)
		{
			cemuLog_log(LogType::Force, "deko3d: failed to allocate the {} byte '{}' stream buffer", m_size,
						m_name);
			return false;
		}

		MemoryTracker::RegisterMemBlock(m_memblock, fmt::format("stream buffer '{}'", m_name));

		m_gpu_addr = m_memblock.getGpuAddr();
		m_host_pointer = static_cast<u8*>(m_memblock.getCpuAddr());
		m_current_offset = 0;
		m_current_gpu_position = 0;
		m_tracked_fences.clear();
		return true;
	}

	bool DKStreamBuffer::ReserveMemory(u32 num_bytes, u32 alignment)
	{
		const u32 required_bytes = num_bytes + alignment;

		if (required_bytes > m_size)
		{
			cemuLog_log(LogType::Force, "Attempting to allocate {} bytes from a {} byte stream buffer", num_bytes,
						m_size);

			return false;
		}

		UpdateCurrentFencePosition();
		if (m_current_offset >= m_current_gpu_position)
		{
			const u32 remaining_bytes = m_size - m_current_offset;
			if (required_bytes <= remaining_bytes)
			{
				m_current_offset = AlignUp(m_current_offset, alignment);
				m_last_allocation_size = num_bytes;
				return true;
			}

			if (required_bytes < m_current_gpu_position)
			{
				m_current_offset = 0;
				m_last_allocation_size = num_bytes;
				return true;
			}
		}

		if (m_current_offset < m_current_gpu_position)
		{
			const u32 remaining_bytes = m_current_gpu_position - m_current_offset;
			if (required_bytes < remaining_bytes)
			{
				m_current_offset = AlignUp(m_current_offset, alignment);
				m_last_allocation_size = num_bytes;
				return true;
			}
		}

		if (WaitForClearSpace(required_bytes))
		{
			m_current_offset = AlignUp(m_current_offset, alignment);
			m_last_allocation_size = num_bytes;
			return true;
		}

		return false;
	}

	void DKStreamBuffer::CommitMemory(u32 final_num_bytes)
	{
		cemu_assert((m_current_offset + final_num_bytes) <= m_size);
		cemu_assert(final_num_bytes <= m_last_allocation_size);

		m_current_offset += final_num_bytes;
	}

	void DKStreamBuffer::UpdateCurrentFencePosition()
	{
		if (m_current_offset == m_current_gpu_position)
			return;

		const u64 counter = g_dk_command_buffer_mgr->GetCurrentFenceCounter();
		if (!m_tracked_fences.empty() && m_tracked_fences.back().first == counter)
		{
			m_tracked_fences.back().second = m_current_offset;
			return;
		}

		UpdateGPUPosition();
		m_tracked_fences.emplace_back(counter, m_current_offset);
	}

	void DKStreamBuffer::UpdateGPUPosition()
	{
		auto start = m_tracked_fences.begin();
		auto end = start;

		const u64 completed_counter = g_dk_command_buffer_mgr->GetCompletedFenceCounter();
		while (end != m_tracked_fences.end() && completed_counter >= end->first)
		{
			m_current_gpu_position = end->second;
			++end;
		}

		if (start != end)
			m_tracked_fences.erase(start, end);
	}

	bool DKStreamBuffer::WaitForClearSpace(u32 num_bytes)
	{
		u32 new_offset = 0;
		u32 new_gpu_position = 0;

		auto iter = m_tracked_fences.begin();
		for (; iter != m_tracked_fences.end(); ++iter)
		{
			u32 gpu_position = iter->second;
			if (m_current_offset == gpu_position)
			{
				new_offset = 0;
				new_gpu_position = 0;
				break;
			}

			if (m_current_offset > gpu_position)
			{
				const u32 remaining_space_after_offset = m_size - m_current_offset;
				if (remaining_space_after_offset >= num_bytes)
				{
					new_offset = m_current_offset;
					new_gpu_position = gpu_position;
					break;
				}

				if (gpu_position > num_bytes)
				{
					new_offset = 0;
					new_gpu_position = gpu_position;
					break;
				}
			}
			else
			{
				u32 available_space_inbetween = gpu_position - m_current_offset;
				if (available_space_inbetween > num_bytes)
				{
					new_offset = m_current_offset;
					new_gpu_position = gpu_position;
					break;
				}
			}
		}

		if (iter == m_tracked_fences.end() ||
			iter->first == g_dk_command_buffer_mgr->GetCurrentFenceCounter())
		{
			return false;
		}

		g_dk_command_buffer_mgr->WaitForFenceCounter(iter->first);
		m_tracked_fences.erase(m_tracked_fences.begin(),
							   m_current_offset == iter->second ? m_tracked_fences.end() : ++iter);
		m_current_offset = new_offset;
		m_current_gpu_position = new_gpu_position;
		return true;
	}
} // namespace Deko3D
