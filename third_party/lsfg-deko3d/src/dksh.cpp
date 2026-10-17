// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0-or-later

#include <lsfg/common/dksh.hpp>

#include <cstring>

namespace lsfg::dksh {
namespace {

constexpr std::size_t offset_type = 0;
constexpr std::size_t offset_entry_point = 4;
constexpr std::size_t offset_gprs = 8;
constexpr std::size_t offset_per_warp_scratch = 20;
constexpr std::size_t offset_block_dims = 24;
constexpr std::size_t offset_shared_memory = 36;
constexpr std::size_t offset_barriers = 52;

std::uint32_t read_u32(const std::span<const std::uint8_t> bytes, const std::size_t offset) noexcept {
    std::uint32_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

bool aligned(const std::uint32_t value) noexcept {
    return value % section_alignment == 0;
}

}

ErrorCode validate(const std::span<const std::uint8_t> bytes, ComputeProgram& out) noexcept {
    out = ComputeProgram{};

    if (bytes.size() < sizeof(FileHeader)) {
        return ErrorCode::cache_integrity_failure;
    }

    FileHeader header{};
    std::memcpy(&header, bytes.data(), sizeof(header));

    if (header.magic != magic || header.header_size != sizeof(FileHeader)) {
        return ErrorCode::cache_integrity_failure;
    }
    if (!aligned(header.control_size) || !aligned(header.code_size)) {
        return ErrorCode::cache_integrity_failure;
    }
    if (header.program_count != 1) {
        return ErrorCode::unsupported;
    }

    const std::uint64_t total
        = static_cast<std::uint64_t>(header.control_size) + header.code_size;
    if (total != bytes.size()) {
        return ErrorCode::cache_integrity_failure;
    }

    const std::uint64_t programs_end
        = static_cast<std::uint64_t>(header.programs_offset) + program_header_size;
    if (header.programs_offset < sizeof(FileHeader) || programs_end > header.control_size) {
        return ErrorCode::cache_integrity_failure;
    }

    const std::span<const std::uint8_t> program
        = bytes.subspan(header.programs_offset, program_header_size);

    if (read_u32(program, offset_type) != static_cast<std::uint32_t>(ProgramType::compute)) {
        return ErrorCode::unsupported;
    }

    out.entry_point = read_u32(program, offset_entry_point);
    out.gprs = read_u32(program, offset_gprs);
    out.per_warp_scratch_bytes = read_u32(program, offset_per_warp_scratch);
    out.block_dim_x = read_u32(program, offset_block_dims);
    out.block_dim_y = read_u32(program, offset_block_dims + 4);
    out.block_dim_z = read_u32(program, offset_block_dims + 8);
    out.shared_memory_bytes = read_u32(program, offset_shared_memory);
    out.barriers = read_u32(program, offset_barriers);

    if (out.block_dim_x == 0 || out.block_dim_y == 0 || out.block_dim_z == 0) {
        return ErrorCode::cache_integrity_failure;
    }
    if (out.entry_point >= header.code_size) {
        return ErrorCode::cache_integrity_failure;
    }

    return ErrorCode::ok;
}

}
