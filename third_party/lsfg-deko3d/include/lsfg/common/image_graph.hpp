// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lsfg/common/error.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace lsfg::graph {

inline constexpr std::uint32_t graph_version = 1;

inline constexpr std::uint32_t max_generated_frames = 3;

enum class Format : std::uint8_t {
    rgba8 = 0,
    r8 = 1,
    rgba16f = 2,
};

enum class ExtentBase : std::uint8_t {
    output = 0,
    flow = 1,
    fixed = 2,
};

enum class ImageRole : std::uint8_t {
    internal = 0,
    history = 1,
    generated = 2,
    constant = 3,
};

struct alignas(4) ImageDesc {
    std::uint8_t base{};
    std::uint8_t shift_before{};
    std::uint8_t ceil_halvings{};
    std::uint8_t shift_after{};

    std::uint16_t fixed_width{};
    std::uint16_t fixed_height{};

    std::uint8_t format{};
    std::uint8_t role{};
    std::uint16_t reserved0_{};
};

static_assert(sizeof(ImageDesc) == 12);

enum class Sampler : std::uint8_t {
    border_black = 0,
    border_white = 1,
    edge = 2,
};

inline constexpr std::uint8_t no_uniform_buffer = 0xFFU;
inline constexpr std::size_t max_dispatch_samplers = 2;

struct alignas(4) VariantEntry {
    std::uint32_t binding_first{};
    std::uint16_t texture_count{};
    std::uint16_t storage_count{};

    std::array<std::uint8_t, max_dispatch_samplers> samplers{};
    std::uint8_t sampler_count{};
    std::uint8_t uniform_buffer{no_uniform_buffer};
};

static_assert(sizeof(VariantEntry) == 12);

struct alignas(4) DispatchEntry {
    std::uint32_t slot{};
    std::uint32_t grid_image{};
    std::uint32_t variant_first{};

    std::uint16_t variant_count{};

    std::uint8_t grid_shift{};
    std::uint8_t stage{};
};

static_assert(sizeof(DispatchEntry) == 16);

inline constexpr std::uint8_t prepass_stage = 0;

struct Config {
    bool performance{};
    bool hdr{};
    std::uint32_t generated_frames{1};

    std::uint32_t flow_numerator{1};
    std::uint32_t flow_denominator{1};
};

inline constexpr std::uint32_t history_image_count = 2;

struct Graph {
    Config config;

    std::vector<ImageDesc> images;
    std::vector<DispatchEntry> dispatches;
    std::vector<VariantEntry> variants;
    std::vector<std::uint32_t> bindings;

    std::uint32_t uniform_buffer_count{};
};

[[nodiscard]] ErrorCode build(const Config& config, Graph& out);

inline constexpr std::uint32_t invalid_block_index = 0xFFFF'FFFFU;

[[nodiscard]] std::uint32_t block_index_of(const Graph& graph, const DispatchEntry& dispatch) noexcept;

[[nodiscard]] ErrorCode validate(const Graph& graph) noexcept;

struct Extent {
    std::uint32_t width{};
    std::uint32_t height{};
};

[[nodiscard]] Extent flow_extent(const Config& config, Extent output) noexcept;
[[nodiscard]] Extent evaluate(const ImageDesc& desc, Extent output, Extent flow) noexcept;

[[nodiscard]] std::uint32_t bytes_per_pixel(Format format) noexcept;

[[nodiscard]] std::uint64_t owned_memory_bytes(const Graph& graph, Extent output) noexcept;

struct ModuleInterface {
    std::uint32_t block_index{};
    std::uint32_t textures{};
    std::uint32_t storage_images{};
    std::uint32_t samplers{};
    std::uint32_t uniform_buffers{};
};

[[nodiscard]] ErrorCode check_against_modules(
    const Graph& graph,
    std::span<const ModuleInterface> modules);

struct alignas(4) ConstantBuffer {
    std::array<std::uint32_t, 2> input_offset{};
    std::uint32_t first_iteration{};
    std::uint32_t first_iteration_s{};
    std::uint32_t advanced_color_kind{};
    std::uint32_t hdr_support{};
    float resolution_inverse_scale{};
    float timestamp{};
    float ui_threshold{};
    std::array<std::uint32_t, 3> padding_{};
};

static_assert(sizeof(ConstantBuffer) == 48);

[[nodiscard]] ConstantBuffer constant_buffer(
    std::uint32_t index,
    const Config& config) noexcept;

[[nodiscard]] std::string_view format_name(Format format) noexcept;
[[nodiscard]] std::string_view role_name(ImageRole role) noexcept;
[[nodiscard]] std::string_view sampler_name(Sampler sampler) noexcept;

}
