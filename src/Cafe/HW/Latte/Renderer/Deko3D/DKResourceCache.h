#pragma once

#include <array>
#include <cstdint>
#include <deko3d.h>

namespace Deko3D
{
	struct TextureBindingCache
	{
		std::array<uint32_t, 16> key{};
		DkImageDescriptor image{};
		DkResHandle handle = 0;
		uint64_t descriptorFence = 0;
		uint64_t samplerGeneration = 0;
		uint32_t sampler = 0;
		bool valid = false;

		bool Matches(const std::array<uint32_t, 16>& value, uint64_t generation) const
		{
			return valid && samplerGeneration == generation && key == value;
		}
	};
}
