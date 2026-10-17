#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>
#include <vector>

namespace Deko3D
{
	enum class StateSlot
	{
		Shaders,
		Framebuffer,
		Rasterizer,
		Multisample,
		Static,
		DepthBias,
		PointSize,
		Restart,
		Color,
		ColorWrite,
		Blend,
		BlendConst,
		DepthStencil,
		Stencil,
		Viewport,
		Scissor,
		VertexAttributes,
		VertexLayout,
		VertexBuffers,
		TexturesVS,
		TexturesPS,
		TexturesGS,
		Count
	};

	class StateCache
	{
	  public:
		void Invalidate() { m_valid = 0; }

		bool Changed(StateSlot slot, const void* data, size_t size)
		{
			const auto index = static_cast<size_t>(slot);
			const uint32_t bit = 1u << index;
			auto& previous = m_values[index];
			if ((m_valid & bit) && previous.size() == size && (!size || std::memcmp(previous.data(), data, size) == 0))
				return false;
			previous.resize(size);
			if (size)
				std::memcpy(previous.data(), data, size);
			m_valid |= bit;
			return true;
		}

		template<typename T>
		bool Changed(StateSlot slot, const T& value)
		{
			static_assert(std::is_trivially_copyable_v<T>);
			return Changed(slot, &value, sizeof(value));
		}

		template<typename T>
		bool Changed(StateSlot slot, std::span<const T> values)
		{
			static_assert(std::is_trivially_copyable_v<T>);
			return Changed(slot, values.data(), values.size_bytes());
		}

	  private:
		static_assert(static_cast<size_t>(StateSlot::Count) <= 32);
		uint32_t m_valid = 0;
		std::array<std::vector<uint8_t>, static_cast<size_t>(StateSlot::Count)> m_values;
	};
}
