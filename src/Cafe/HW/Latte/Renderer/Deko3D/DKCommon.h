#pragma once

#include "Common/precompiled.h"
#include <deko3d.hpp>

namespace Deko3D
{
	using u8 = uint8;
	using u32 = uint32;
	using u64 = uint64;

	template<typename T, typename U>
	constexpr T AlignUp(T value, U alignment)
	{
		return (value + static_cast<T>(alignment) - 1) & ~(static_cast<T>(alignment) - 1);
	}

	constexpr size_t NUM_COMMAND_BUFFERS = 8;
	constexpr u32 INIT_COMMAND_BUFFER_SIZE = 64 * 1024;
	constexpr u32 DRAW_COMMAND_BUFFER_SIZE = 256 * 1024;
	constexpr u32 COMMAND_BUFFER_GROWTH_SIZE = 256 * 1024;
	constexpr u32 DRAW_SUBMIT_THRESHOLD = 1024;
	constexpr u32 SEQUENCE_SUBMIT_THRESHOLD = 64;
	constexpr u32 PER_WARP_SCRATCH_MEMORY_SIZE = 128 * 1024;
	constexpr u32 TEXTURE_UPLOAD_BUFFER_SIZE = 32 * 1024 * 1024;
	constexpr u32 NUM_IMAGE_DESCRIPTORS = 64 * 1024;
	constexpr u32 NUM_SAMPLER_DESCRIPTORS = 1024;
} // namespace Deko3D
