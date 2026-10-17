// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0-or-later

#include <lsfg/backend/binding.hpp>

#include <cstddef>

namespace lsfg::backend {
namespace {

[[nodiscard]] bool same_interface(
    const cache::PassEntry& entry,
    const graph::VariantEntry& variant) noexcept {
    const std::uint32_t uniform_buffers = variant.uniform_buffer == graph::no_uniform_buffer ? 0U : 1U;
    return entry.image_count == variant.texture_count
        && entry.storage_image_count == variant.storage_count
        && entry.sampler_count == variant.sampler_count
        && entry.uniform_buffer_count == uniform_buffers;
}

}

ErrorCode bind(
    const cache::Loaded& cache,
    const Plan& plan,
    const DescriptorLayout& descriptors,
    const std::uint32_t dispatch,
    const std::uint32_t phase,
    DispatchBinding& out) noexcept {
    out = DispatchBinding{};

    if (dispatch >= cache.graph.dispatches.size() || dispatch >= plan.dispatches.size()) {
        return ErrorCode::invalid_argument;
    }
    if (descriptors.images.size() != cache.graph.images.size()) {
        return ErrorCode::invalid_argument;
    }

    const graph::DispatchEntry& entry = cache.graph.dispatches[dispatch];
    const DispatchPlan& planned = plan.dispatches[dispatch];
    if (entry.variant_count == 0 || planned.pass >= cache.passes.size()) {
        return ErrorCode::cache_integrity_failure;
    }

    const std::uint32_t variant_index = entry.variant_first + (phase % entry.variant_count);
    if (variant_index >= cache.graph.variants.size()) {
        return ErrorCode::cache_integrity_failure;
    }

    const graph::VariantEntry& variant = cache.graph.variants[variant_index];
    const cache::LoadedPass& pass = cache.passes[planned.pass];

    if (!same_interface(pass.entry, variant)) {
        return ErrorCode::shader_interface_mismatch;
    }
    if (pass.entry.texture_slot_count > max_texture_slots
        || pass.entry.storage_image_count > max_storage_slots
        || pass.entry.uniform_buffer_count > max_uniform_slots) {
        return ErrorCode::shader_interface_mismatch;
    }
    if (pass.slots.size() != pass.entry.slot_count) {
        return ErrorCode::cache_integrity_failure;
    }

    const std::size_t bound = static_cast<std::size_t>(variant.texture_count) + variant.storage_count;
    if (variant.binding_first > cache.graph.bindings.size()
        || bound > cache.graph.bindings.size() - variant.binding_first) {
        return ErrorCode::cache_integrity_failure;
    }
    const std::uint32_t* const bindings = cache.graph.bindings.data() + variant.binding_first;

    out.pass = planned.pass;
    out.variant = variant_index;
    out.groups_x = planned.groups_x;
    out.groups_y = planned.groups_y;

    for (const cache::SlotEntry& slot : pass.slots) {
        switch (static_cast<cache::SlotKind>(slot.kind)) {
        case cache::SlotKind::texture: {
            if (slot.ordinal >= variant.texture_count || slot.slot >= max_texture_slots
                || out.texture_count >= max_texture_slots) {
                return ErrorCode::shader_interface_mismatch;
            }

            std::uint32_t sampler = introduced_sampler;
            if (slot.sampler_ordinal != cache::introduced_sampler) {
                if (slot.sampler_ordinal >= variant.sampler_count) {
                    return ErrorCode::shader_interface_mismatch;
                }
                sampler = variant.samplers[slot.sampler_ordinal];
                if (sampler >= introduced_sampler) {
                    return ErrorCode::cache_integrity_failure;
                }
            }

            const std::uint32_t image = bindings[slot.ordinal];
            if (image >= descriptors.images.size()) {
                return ErrorCode::cache_integrity_failure;
            }

            const std::uint32_t descriptor = descriptors.images[image].sampled;
            if (descriptor == no_descriptor) {
                return ErrorCode::cache_integrity_failure;
            }

            out.textures[out.texture_count++] = TextureBinding{
                .slot = slot.slot,
                .image = image,
                .descriptor = descriptor,
                .sampler = sampler,
            };
            break;
        }
        case cache::SlotKind::storage_image: {
            if (slot.ordinal >= variant.storage_count || slot.slot >= max_storage_slots
                || out.storage_count >= max_storage_slots) {
                return ErrorCode::shader_interface_mismatch;
            }

            const std::uint32_t image = bindings[variant.texture_count + slot.ordinal];
            if (image >= descriptors.images.size()) {
                return ErrorCode::cache_integrity_failure;
            }

            const std::uint32_t descriptor = descriptors.images[image].storage;
            if (descriptor == no_descriptor) {
                return ErrorCode::cache_integrity_failure;
            }

            out.storages[out.storage_count++] = StorageBinding{
                .slot = slot.slot,
                .image = image,
                .descriptor = descriptor,
            };
            break;
        }
        case cache::SlotKind::uniform_buffer: {
            if (variant.uniform_buffer == graph::no_uniform_buffer
                || variant.uniform_buffer >= plan.uniform_buffers || out.uniform_slot != no_slot) {
                return ErrorCode::shader_interface_mismatch;
            }

            out.uniform_slot = slot.slot;
            out.uniform_buffer = variant.uniform_buffer;
            break;
        }
        default:
            return ErrorCode::shader_interface_mismatch;
        }
    }

    if (out.texture_count != pass.entry.texture_slot_count
        || out.storage_count != pass.entry.storage_image_count) {
        return ErrorCode::shader_interface_mismatch;
    }

    return ErrorCode::ok;
}

}
