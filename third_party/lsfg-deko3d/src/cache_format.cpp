// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0-or-later

#include <lsfg/common/cache_format.hpp>

namespace lsfg::cache {
namespace {

constexpr std::uint32_t crc32_polynomial = 0xEDB8'8320U;

}

std::uint32_t crc32(const std::span<const std::uint8_t> data, const std::uint32_t seed) noexcept {
    std::uint32_t remainder = ~seed;
    for (const std::uint8_t byte : data) {
        remainder ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = static_cast<std::uint32_t>(-static_cast<std::int32_t>(remainder & 1U));
            remainder = (remainder >> 1U) ^ (crc32_polynomial & mask);
        }
    }
    return ~remainder;
}

Digest cache_key(const CacheKeyInputs& inputs) noexcept {
    Sha256 hasher;
    hasher.update_field(inputs.dll_bytes);
    hasher.update_field(inputs.extractor_version);
    hasher.update_field(inputs.spirv_cross_revision);
    hasher.update_field(inputs.uam_revision);
    hasher.update_field(inputs.translation_options);
    hasher.update_field(inputs.graph_options);
    hasher.update_field(inputs.backend_abi_version);
    return hasher.finish();
}

std::uint32_t pack_options(const Precision precision, const graph::Config& config) noexcept {
    std::uint32_t word = 0;
    word |= config.performance ? 1U << 0U : 0U;
    word |= config.hdr ? 1U << 1U : 0U;
    word |= precision == Precision::low ? 1U << 2U : 0U;
    word |= (config.generated_frames & 0x1FU) << 3U;
    word |= (config.flow_numerator & 0xFFU) << 8U;
    word |= (config.flow_denominator & 0xFFU) << 16U;
    return word;
}

void initialize(ManifestHeader& header) noexcept {
    header = ManifestHeader{};
    header.magic = manifest_magic;
    header.abi_version = abi_version;
    header.extractor_version = extractor_version;
    header.graph_version = graph::graph_version;
    header.backend_abi_version = backend_abi_version;
}

void describe(ManifestHeader& header, const graph::Graph& graph) noexcept {
    header.graph_version = graph::graph_version;
    header.image_count = static_cast<std::uint32_t>(graph.images.size());
    header.dispatch_count = static_cast<std::uint32_t>(graph.dispatches.size());
    header.variant_count = static_cast<std::uint32_t>(graph.variants.size());
    header.binding_count = static_cast<std::uint32_t>(graph.bindings.size());
    header.uniform_buffer_count = graph.uniform_buffer_count;
    header.generated_frames = graph.config.generated_frames;
    header.flow_numerator = graph.config.flow_numerator;
    header.flow_denominator = graph.config.flow_denominator;

    header.options &= ~(option_performance | option_hdr);
    header.options |= graph.config.performance ? option_performance : 0U;
    header.options |= graph.config.hdr ? option_hdr : 0U;
}

graph::Config configuration(const ManifestHeader& header) noexcept {
    return graph::Config{
        .performance = (header.options & option_performance) != 0U,
        .hdr = (header.options & option_hdr) != 0U,
        .generated_frames = header.generated_frames,
        .flow_numerator = header.flow_numerator,
        .flow_denominator = header.flow_denominator,
    };
}

ErrorCode validate(const ManifestHeader& header) noexcept {
    if (header.magic != manifest_magic) {
        return ErrorCode::cache_integrity_failure;
    }
    if (header.abi_version != abi_version) {
        return ErrorCode::cache_version_mismatch;
    }
    if (header.extractor_version != extractor_version || header.graph_version != graph::graph_version) {
        return ErrorCode::cache_version_mismatch;
    }
    if (header.dll_size == 0) {
        return ErrorCode::cache_integrity_failure;
    }
    if (header.pass_count == 0 || header.pass_count > max_passes) {
        return ErrorCode::cache_integrity_failure;
    }
    if (header.slot_count == 0 || header.slot_count > max_slots) {
        return ErrorCode::cache_integrity_failure;
    }
    if (header.image_count == 0 || header.image_count > max_images) {
        return ErrorCode::cache_integrity_failure;
    }
    if (header.dispatch_count == 0 || header.dispatch_count > max_dispatches) {
        return ErrorCode::cache_integrity_failure;
    }
    if (header.variant_count == 0 || header.variant_count > max_variants) {
        return ErrorCode::cache_integrity_failure;
    }
    if (header.binding_count == 0 || header.binding_count > max_bindings) {
        return ErrorCode::cache_integrity_failure;
    }
    if (header.shader_block_size == 0 || header.shader_precision > 1U) {
        return ErrorCode::shader_set_unknown;
    }
    if (header.flow_numerator == 0 || header.flow_denominator == 0
        || header.flow_numerator > header.flow_denominator) {
        return ErrorCode::cache_integrity_failure;
    }
    return ErrorCode::ok;
}

ErrorCode validate(const PassEntry& entry) noexcept {
    if (entry.dksh_size == 0) {
        return ErrorCode::cache_integrity_failure;
    }
    if (entry.workgroup_x == 0 || entry.workgroup_y == 0 || entry.workgroup_z == 0) {
        return ErrorCode::shader_interface_mismatch;
    }
    if (entry.slot_count == 0) {
        return ErrorCode::shader_interface_mismatch;
    }
    if (entry.slot_count
        != entry.texture_slot_count + entry.storage_image_count + entry.uniform_buffer_count) {
        return ErrorCode::shader_interface_mismatch;
    }
    if (entry.texture_slot_count < entry.image_count) {
        return ErrorCode::shader_interface_mismatch;
    }
    return ErrorCode::ok;
}

ErrorCode validate(
    const ManifestHeader& header,
    const std::span<const PassEntry> passes,
    const std::span<const SlotEntry> slots,
    const graph::Graph& graph) noexcept {
    if (const ErrorCode code = validate(header); !succeeded(code)) {
        return code;
    }
    if (passes.size() != header.pass_count || slots.size() != header.slot_count) {
        return ErrorCode::cache_integrity_failure;
    }

    for (const PassEntry& entry : passes) {
        if (const ErrorCode code = validate(entry); !succeeded(code)) {
            return code;
        }
        if (entry.slot_first + entry.slot_count > slots.size()) {
            return ErrorCode::cache_integrity_failure;
        }

        for (std::uint32_t index = 0; index < entry.slot_count; ++index) {
            const SlotEntry& slot = slots[entry.slot_first + index];
            switch (static_cast<SlotKind>(slot.kind)) {
            case SlotKind::uniform_buffer:
                if (slot.slot >= entry.uniform_buffer_count || slot.ordinal >= entry.uniform_buffer_count) {
                    return ErrorCode::shader_interface_mismatch;
                }
                break;
            case SlotKind::texture:
                if (slot.slot >= entry.texture_slot_count || slot.ordinal >= entry.image_count) {
                    return ErrorCode::shader_interface_mismatch;
                }
                if (slot.sampler_ordinal != introduced_sampler
                    && slot.sampler_ordinal >= entry.sampler_count) {
                    return ErrorCode::shader_interface_mismatch;
                }
                break;
            case SlotKind::storage_image:
                if (slot.slot >= entry.storage_image_count
                    || slot.ordinal >= entry.storage_image_count) {
                    return ErrorCode::shader_interface_mismatch;
                }
                break;
            default:
                return ErrorCode::shader_interface_mismatch;
            }
        }
    }

    if (const ErrorCode code = graph::validate(graph); !succeeded(code)) {
        return code;
    }
    if (header.image_count != graph.images.size() || header.dispatch_count != graph.dispatches.size()
        || header.variant_count != graph.variants.size()
        || header.binding_count != graph.bindings.size()
        || header.uniform_buffer_count != graph.uniform_buffer_count) {
        return ErrorCode::cache_integrity_failure;
    }

    for (const graph::DispatchEntry& dispatch : graph.dispatches) {
        const std::uint32_t block_index = graph::block_index_of(graph, dispatch);
        const std::uint32_t tile = 1U << dispatch.grid_shift;

        const PassEntry* module = nullptr;
        for (const PassEntry& entry : passes) {
            if (entry.block_index == block_index) {
                module = &entry;
                break;
            }
        }
        if (module == nullptr) {
            return ErrorCode::cache_integrity_failure;
        }

        if (module->workgroup_z != 1 || module->workgroup_x != module->workgroup_y) {
            return ErrorCode::shader_interface_mismatch;
        }
        if (tile < module->workgroup_x || (tile % module->workgroup_x) != 0) {
            return ErrorCode::shader_interface_mismatch;
        }

        for (std::uint32_t offset = 0; offset < dispatch.variant_count; ++offset) {
            const graph::VariantEntry& variant = graph.variants[dispatch.variant_first + offset];
            const std::uint32_t uniform_buffers
                = variant.uniform_buffer == graph::no_uniform_buffer ? 0U : 1U;

            if (variant.texture_count != module->image_count
                || variant.storage_count != module->storage_image_count
                || variant.sampler_count != module->sampler_count
                || uniform_buffers != module->uniform_buffer_count) {
                return ErrorCode::shader_interface_mismatch;
            }
        }
    }

    return ErrorCode::ok;
}

}
