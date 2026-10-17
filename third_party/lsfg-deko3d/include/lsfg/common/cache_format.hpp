// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lsfg/common/error.hpp>
#include <lsfg/common/image_graph.hpp>
#include <lsfg/common/sha256.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace lsfg::cache {

inline constexpr std::uint32_t manifest_magic = 0x4746'534CU;

inline constexpr std::uint32_t abi_version = 2;

inline constexpr std::uint32_t backend_abi_version = 2;

inline constexpr std::uint32_t extractor_version = 1;

inline constexpr std::size_t digest_size = 32;
inline constexpr std::size_t revision_size = 20;
inline constexpr std::uint32_t max_passes = 64;
inline constexpr std::uint32_t max_slots = 4096;
inline constexpr std::uint32_t max_images = 4096;
inline constexpr std::uint32_t max_dispatches = 1024;
inline constexpr std::uint32_t max_variants = 2048;
inline constexpr std::uint32_t max_bindings = 65536;

using Digest = std::array<std::uint8_t, digest_size>;
using Revision = std::array<std::uint8_t, revision_size>;

enum class Precision : std::uint32_t {
    low = 0,
    high = 1,
};

inline constexpr std::uint32_t option_performance = 1U << 0U;
inline constexpr std::uint32_t option_hdr = 1U << 1U;

struct alignas(8) ManifestHeader {
    std::uint32_t magic{};
    std::uint32_t abi_version{};

    Digest dll_hash{};
    std::uint64_t dll_size{};

    std::uint32_t extractor_version{};
    std::uint32_t graph_version{};

    Revision spirv_cross_revision{};
    Revision uam_revision{};

    std::uint32_t translation_options{};
    std::uint32_t backend_abi_version{};

    std::uint32_t shader_first_resource_id{};
    std::uint32_t shader_block_size{};
    std::uint32_t shader_precision{};
    std::uint32_t options{};

    std::uint32_t pass_count{};
    std::uint32_t slot_count{};
    std::uint32_t image_count{};
    std::uint32_t dispatch_count{};
    std::uint32_t variant_count{};
    std::uint32_t binding_count{};

    std::uint32_t uniform_buffer_count{};
    std::uint32_t generated_frames{};
    std::uint32_t flow_numerator{};
    std::uint32_t flow_denominator{};

    std::uint32_t payload_crc32{};
    std::uint32_t reserved0_{};
};

struct alignas(8) PassEntry {
    std::uint32_t resource_id{};
    std::uint32_t block_index{};
    std::uint32_t dksh_size{};
    std::uint32_t scratch_memory_bytes{};

    Digest dksh_hash{};

    std::uint16_t workgroup_x{};
    std::uint16_t workgroup_y{};
    std::uint16_t workgroup_z{};
    std::uint16_t reserved0_{};

    std::uint32_t image_count{};
    std::uint32_t storage_image_count{};
    std::uint32_t sampler_count{};
    std::uint32_t uniform_buffer_count{};

    std::uint32_t texture_slot_count{};

    std::uint32_t slot_first{};
    std::uint32_t slot_count{};

    std::uint32_t register_count{};
    std::uint32_t shared_memory_bytes{};
};

enum class SlotKind : std::uint8_t {
    uniform_buffer = 0,
    texture = 1,
    storage_image = 2,
};

inline constexpr std::uint8_t introduced_sampler = 0xFFU;

struct alignas(4) SlotEntry {
    std::uint8_t kind{};
    std::uint8_t slot{};
    std::uint8_t ordinal{};
    std::uint8_t sampler_ordinal{};
};

static_assert(sizeof(ManifestHeader) == 168);
static_assert(sizeof(PassEntry) == 96);
static_assert(sizeof(SlotEntry) == 4);
static_assert(alignof(ManifestHeader) == 8);
static_assert(alignof(PassEntry) == 8);

struct CacheKeyInputs {
    std::span<const std::uint8_t> dll_bytes;
    std::uint32_t extractor_version{};
    std::string_view spirv_cross_revision;
    std::string_view uam_revision;
    std::uint32_t translation_options{};
    std::uint32_t graph_options{};
    std::uint32_t backend_abi_version{};
};

[[nodiscard]] std::uint32_t pack_options(Precision precision, const graph::Config& config) noexcept;

[[nodiscard]] Digest cache_key(const CacheKeyInputs& inputs) noexcept;

[[nodiscard]] std::uint32_t crc32(std::span<const std::uint8_t> data, std::uint32_t seed = 0) noexcept;

void initialize(ManifestHeader& header) noexcept;

[[nodiscard]] ErrorCode validate(const ManifestHeader& header) noexcept;
[[nodiscard]] ErrorCode validate(const PassEntry& entry) noexcept;

[[nodiscard]] ErrorCode validate(
    const ManifestHeader& header,
    std::span<const PassEntry> passes,
    std::span<const SlotEntry> slots,
    const graph::Graph& graph) noexcept;

void describe(ManifestHeader& header, const graph::Graph& graph) noexcept;

[[nodiscard]] graph::Config configuration(const ManifestHeader& header) noexcept;

}
