// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lsfg/common/error.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lsfg::dksh {

inline constexpr std::uint32_t magic = 0x4853'4B44U;

enum class ProgramType : std::uint32_t {
    vertex = 0,
    fragment = 1,
    geometry = 2,
    tess_control = 3,
    tess_eval = 4,
    compute = 5,
};

struct FileHeader {
    std::uint32_t magic{};
    std::uint32_t header_size{};
    std::uint32_t control_size{};
    std::uint32_t code_size{};
    std::uint32_t programs_offset{};
    std::uint32_t program_count{};
};

static_assert(sizeof(FileHeader) == 24);

struct ComputeProgram {
    std::uint32_t entry_point{};
    std::uint32_t gprs{};
    std::uint32_t per_warp_scratch_bytes{};
    std::uint32_t block_dim_x{};
    std::uint32_t block_dim_y{};
    std::uint32_t block_dim_z{};
    std::uint32_t shared_memory_bytes{};
    std::uint32_t barriers{};
};

inline constexpr std::size_t program_header_size = 64;
inline constexpr std::uint32_t section_alignment = 256;

struct Blob {
    std::vector<std::uint8_t> bytes;
    std::string log;
    ComputeProgram program;
};

[[nodiscard]] bool compiler_available() noexcept;

[[nodiscard]] ErrorCode compile(std::string_view glsl, Blob& out);

[[nodiscard]] ErrorCode validate(std::span<const std::uint8_t> bytes, ComputeProgram& out) noexcept;

}
