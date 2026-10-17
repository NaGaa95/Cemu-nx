#include "DKRendererInternal.h"
#include "DKMemoryTracker.h"

using namespace Deko3D;

namespace Deko3D
{
	class QueryPool
	{
	  public:
		static constexpr u32 Count = 1024;
		static constexpr u32 Stride = 32;
		QueryPool()
		{
			memory = dk::MemBlockMaker{g_dk_context->GetDevice(), Count * Stride}
						 .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
						 .create();
			if (!memory)
				throw std::bad_alloc();
			MemoryTracker::RegisterMemBlock(memory, "occlusion queries");
			for (u32 i = 0; i < Count; ++i)
				free.push_back(i);
		}
		~QueryPool()
		{
			MemoryTracker::UnregisterMemBlock(memory);
			DeferMemBlockDestruction(std::move(memory));
		}
		dk::UniqueMemBlock memory;
		std::vector<u32> free;
	};
} // namespace Deko3D

class Deko3DRenderer::State::Query final : public LatteQueryObject
{
  public:
	Query(State& state, std::shared_ptr<QueryPool> pool, u32 slot) : m_state(state), m_pool(std::move(pool)), m_slot(slot) {}
	~Query()
	{
		g_dk_command_buffer_mgr->DeferCleanup([pool = m_pool, slot = m_slot] { pool->free.push_back(slot); });
	}
	void begin() override
	{
		m_ended = false;
		dkCmdBufReportCounter(m_state.Cmd(), DkCounter_SamplesPassed, Address());
	}
	void end() override
	{
		dkCmdBufReportCounter(m_state.Cmd(), DkCounter_SamplesPassed, Address() + 16);
		g_dk_command_buffer_mgr->NotifyCpuReadback();
		m_fence = g_dk_command_buffer_mgr->GetCurrentFenceCounter();
		m_state.lastQueryFence = m_fence;
		m_ended = true;
	}
	bool getResult(uint64& result) override
	{
		if (!m_ended)
			return false;
		if (m_fence == g_dk_command_buffer_mgr->GetCurrentFenceCounter())
			m_state.Submit(false);
		if (g_dk_command_buffer_mgr->GetCompletedFenceCounter() < m_fence)
			g_dk_command_buffer_mgr->PollCompletedFences();
		if (g_dk_command_buffer_mgr->GetCompletedFenceCounter() < m_fence)
			return false;
		const auto* reports = reinterpret_cast<const u64*>(static_cast<const u8*>(m_pool->memory.getCpuAddr()) + m_slot * QueryPool::Stride);
		result = reports[2] - reports[0];
		return true;
	}

  private:
	DkGpuAddr Address() const
	{
		return m_pool->memory.getGpuAddr() + m_slot * QueryPool::Stride;
	}
	State& m_state;
	std::shared_ptr<QueryPool> m_pool;
	u32 m_slot;
	u64 m_fence = 0;
	bool m_ended = false;
};

LatteQueryObject* Deko3DRenderer::occlusionQuery_create()
{
	if (!m_state->queries)
	{
		m_state->queries = std::make_shared<QueryPool>();
		dkCmdBufResetCounter(m_state->Cmd(), DkCounter_SamplesPassed);
	}
	auto& pool = *m_state->queries;
	if (pool.free.empty())
		m_state->queries = std::make_shared<QueryPool>();
	const u32 slot = m_state->queries->free.back();
	m_state->queries->free.pop_back();
	return new State::Query(*m_state, m_state->queries, slot);
}

void Deko3DRenderer::occlusionQuery_destroy(LatteQueryObject* query)
{
	delete static_cast<State::Query*>(query);
}
void Deko3DRenderer::occlusionQuery_flush()
{
	if (m_state->lastQueryFence == g_dk_command_buffer_mgr->GetCurrentFenceCounter())
		m_state->Submit(false);
	if (m_state->lastQueryFence)
		g_dk_command_buffer_mgr->WaitForFenceCounter(m_state->lastQueryFence);
}
void Deko3DRenderer::occlusionQuery_updateState()
{
	if (g_dk_command_buffer_mgr->GetCompletedFenceCounter() < m_state->lastQueryFence)
		g_dk_command_buffer_mgr->PollCompletedFences();
}
