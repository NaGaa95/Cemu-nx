#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <utility>

class FenceCallbacks
{
public:
	void Push(uint64_t fence, std::function<void()> callback)
	{
		m_pending.push_back({fence, std::move(callback)});
	}

	void Complete(uint64_t fence)
	{
		while (!m_pending.empty() && m_pending.front().fence <= fence)
		{
			auto callback = std::move(m_pending.front().callback);
			m_pending.pop_front();
			callback();
		}
	}

	bool Empty() const { return m_pending.empty(); }

private:
	struct Entry
	{
		uint64_t fence;
		std::function<void()> callback;
	};
	std::deque<Entry> m_pending;
};
