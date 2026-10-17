// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lsfg/common/error.hpp>
#include <lsfg/common/spirv_module.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace lsfg::translate {

enum class SlotKind : std::uint8_t {
    uniform_buffer,
    texture,
    storage_image,
};

struct SlotAssignment {
    SlotKind kind{};
    std::uint32_t slot{};
    std::uint32_t spirv_binding{};
    std::uint32_t spirv_sampler_binding{};
    bool uses_dummy_sampler{};
    std::string name;
};

struct Limits {
    std::uint32_t textures{32};
    std::uint32_t storage_images{8};
    std::uint32_t uniform_buffers{16};
    std::uint32_t storage_buffers{16};
};

inline constexpr Limits uam_limits{};

struct Options {
    std::uint32_t glsl_version{450};

    spirv::ImageFormat unformatted_storage_image_format{spirv::ImageFormat::rgba8};
    Limits limits{uam_limits};
};

struct Module {
    std::string glsl;
    std::vector<SlotAssignment> slots;

    std::uint32_t local_size_x{};
    std::uint32_t local_size_y{};
    std::uint32_t local_size_z{};

    std::uint32_t texture_count{};
    std::uint32_t storage_image_count{};
    std::uint32_t uniform_buffer_count{};

    spirv::PatchSummary patch;

    bool needed_dummy_sampler{};
};

[[nodiscard]] ErrorCode to_glsl(
    std::span<const std::uint8_t> module_bytes,
    const Options& options,
    Module& out);

[[nodiscard]] std::string_view slot_kind_name(SlotKind kind) noexcept;

}
