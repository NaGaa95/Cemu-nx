// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lsfg/common/error.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lsfg::spirv {

inline constexpr std::uint32_t magic = 0x0723'0203U;
inline constexpr std::size_t header_words = 5;

enum class Capability : std::uint32_t {
    shader = 1,
    float16 = 9,
    int16 = 22,
    int8 = 39,
    storage_image_extended_formats = 49,
    image_query = 50,
    derivative_control = 51,
    storage_image_read_without_format = 55,
    storage_image_write_without_format = 56,
    variable_pointers = 4442,
};

enum class ExecutionModel : std::uint32_t {
    gl_compute = 5,
};

enum class ImageFormat : std::uint32_t {
    unknown = 0,
    rgba32f = 1,
    rgba16f = 2,
    r32f = 3,
    rgba8 = 4,
    rgba8_snorm = 5,
    rg32f = 6,
    rg16f = 7,
    r11f_g11f_b10f = 8,
    r16f = 9,
    rgba16 = 10,
    rgb10_a2 = 11,
    rg16 = 12,
    rg8 = 13,
    r16 = 14,
    r8 = 15,
    rgba16_snorm = 16,
    rg16_snorm = 17,
    rg8_snorm = 18,
    r16_snorm = 19,
    r8_snorm = 20,
};

enum class ResourceKind : std::uint8_t {
    unknown,
    sampler,
    sampled_image,
    separate_image,
    storage_image,
    uniform_buffer,
    storage_buffer,
};

struct Binding {
    std::uint32_t set{};
    std::uint32_t binding{};
    std::uint32_t array_size{1};
    ResourceKind kind{ResourceKind::unknown};
    std::uint32_t image_format{};
    std::uint32_t image_dim{};
    bool non_readable{};
    bool non_writable{};
};

struct SpecConstant {
    std::uint32_t spec_id{};
    std::uint32_t result_id{};
    std::uint32_t value{};
    bool has_value{};
};

struct Inventory {
    std::uint32_t version_major{};
    std::uint32_t version_minor{};
    std::uint32_t generator{};
    std::uint32_t bound{};

    std::uint32_t execution_model{};
    std::string entry_point;

    std::uint32_t local_size_x{};
    std::uint32_t local_size_y{};
    std::uint32_t local_size_z{};

    bool local_size_is_specialised{};

    bool uses_push_constants{};

    std::vector<std::uint32_t> capabilities;
    std::vector<Binding> bindings;
    std::vector<SpecConstant> spec_constants;
};

struct DescriptorCounts {
    std::uint32_t samplers{};
    std::uint32_t sampled_images{};
    std::uint32_t separate_images{};
    std::uint32_t storage_images{};
    std::uint32_t uniform_buffers{};
    std::uint32_t storage_buffers{};
    std::uint32_t highest_set{};
    std::uint32_t highest_binding{};
};

[[nodiscard]] ErrorCode inspect(std::span<const std::uint32_t> words, Inventory& out);

[[nodiscard]] ErrorCode inspect_bytes(std::span<const std::uint8_t> bytes, Inventory& out);

[[nodiscard]] bool is_spirv(std::span<const std::uint8_t> bytes) noexcept;

[[nodiscard]] bool has_capability(const Inventory& inventory, Capability capability) noexcept;

[[nodiscard]] DescriptorCounts count_descriptors(const Inventory& inventory) noexcept;

[[nodiscard]] std::string_view capability_name(std::uint32_t capability) noexcept;
[[nodiscard]] std::string_view image_format_name(std::uint32_t format) noexcept;
[[nodiscard]] std::string_view resource_kind_name(ResourceKind kind) noexcept;

struct PatchSummary {
    std::uint32_t capabilities_replaced{};
    std::uint32_t images_formatted{};
};

[[nodiscard]] ErrorCode patch_storage_image_format(
    std::span<std::uint32_t> words,
    ImageFormat format,
    PatchSummary& summary) noexcept;

}
