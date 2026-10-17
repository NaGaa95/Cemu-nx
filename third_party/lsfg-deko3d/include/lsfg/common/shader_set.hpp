// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lsfg/common/error.hpp>
#include <lsfg/common/pe_resources.hpp>

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace lsfg::shaders {

enum class Precision : std::uint8_t {
    low,
    high,
};

struct ShaderSet {
    std::uint32_t first_resource_id{};
    std::uint32_t block_size{};
    std::uint32_t low_precision_block{};
    std::uint32_t high_precision_block{};
};

inline constexpr std::uint32_t performance_offset = 23;

inline constexpr std::uint32_t binding_range = 16;
inline constexpr std::uint32_t binding_base_uniform_buffer = 0;
inline constexpr std::uint32_t binding_base_sampler = 16;
inline constexpr std::uint32_t binding_base_image = 32;
inline constexpr std::uint32_t binding_base_storage_image = 48;

struct ChainSlot {
    std::string_view name;
    std::uint32_t block_index{};
    bool has_performance_variant{};
};

struct ModuleRequest {
    std::string_view name;
    std::uint32_t block_index{};
    std::uint32_t resource_id{};
};

[[nodiscard]] std::span<const ChainSlot> chain_slots() noexcept;

[[nodiscard]] ErrorCode identify(
    std::span<const std::uint8_t> image,
    std::span<const pe::Resource> resources,
    ShaderSet& out);

[[nodiscard]] std::uint32_t resource_id_for(
    const ShaderSet& set,
    Precision precision,
    std::uint32_t block_index) noexcept;

[[nodiscard]] ErrorCode required_modules(
    const ShaderSet& set,
    Precision precision,
    bool performance,
    std::vector<ModuleRequest>& out);

[[nodiscard]] std::string_view precision_name(Precision precision) noexcept;

}
