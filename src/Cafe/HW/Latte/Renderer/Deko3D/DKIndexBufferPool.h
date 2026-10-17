#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace Deko3D
{
	struct IndexReservation
	{
		std::vector<uint8_t> cached;
	};

	class DKIndexBufferPool
	{
	public:
		DKIndexBufferPool()
		{
			m_active.reserve(16);
			m_free.reserve(8);
		}

		IndexReservation* Reserve(uint32_t size)
		{
			std::unique_ptr<IndexReservation> reservation;
			if (m_free.empty())
				reservation = std::make_unique<IndexReservation>();
			else
			{
				auto entry = std::find_if(m_free.begin(), m_free.end(), [size](const auto& buffer) {
					return buffer->cached.capacity() >= size;
				});
				if (entry == m_free.end())
					entry = m_free.begin();
				reservation = std::move(*entry);
				m_cachedBytes -= reservation->cached.capacity();
				if (entry != m_free.end() - 1)
					*entry = std::move(m_free.back());
				m_free.pop_back();
			}
			reservation->cached.clear();
			reservation->cached.resize(size);
			auto* result = reservation.get();
			m_active.push_back(std::move(reservation));
			return result;
		}

		void Release(IndexReservation* reservation)
		{
			auto entry = std::find_if(m_active.begin(), m_active.end(), [reservation](const auto& buffer) {
				return buffer.get() == reservation;
			});
			if (entry == m_active.end())
				return;
			auto buffer = std::move(*entry);
			if (entry != m_active.end() - 1)
				*entry = std::move(m_active.back());
			m_active.pop_back();
			if (m_free.size() < 8 && buffer->cached.capacity() <= 4 * 1024 * 1024 - m_cachedBytes)
			{
				m_cachedBytes += buffer->cached.capacity();
				m_free.push_back(std::move(buffer));
			}
		}

	private:
		std::vector<std::unique_ptr<IndexReservation>> m_active;
		std::vector<std::unique_ptr<IndexReservation>> m_free;
		std::size_t m_cachedBytes = 0;
	};
}
