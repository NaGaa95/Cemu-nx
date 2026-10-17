// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0-or-later

#include <lsfg/backend/cache_load.hpp>

#include <lsfg/common/dksh.hpp>
#include <lsfg/common/shader_set.hpp>

#include <algorithm>
#include <string>

namespace lsfg::backend {
namespace {

[[nodiscard]] bool refuse(
    Rejection& why,
    const ErrorCode code,
    const std::string_view reason,
    const std::uint32_t pass = no_pass,
    const std::uint64_t observed = 0,
    const std::uint64_t allowed = 0) {
    why = Rejection{
        .code = code,
        .reason = reason,
        .pass = pass,
        .observed = observed,
        .allowed = allowed,
    };
    return false;
}

[[nodiscard]] bool same_configuration(const graph::Config& left, const graph::Config& right) noexcept {
    return left.performance == right.performance && left.hdr == right.hdr
        && left.generated_frames == right.generated_frames
        && left.flow_numerator == right.flow_numerator
        && left.flow_denominator == right.flow_denominator;
}

[[nodiscard]] std::uint32_t groups_for(const std::uint32_t extent, const std::uint32_t tile) noexcept {
    return (extent + tile - 1U) / tile;
}

[[nodiscard]] bool check_module(
    const cache::LoadedPass& pass,
    const std::uint32_t index,
    const Limits& limits,
    Rejection& why) {
    const cache::PassEntry& entry = pass.entry;

    if (entry.texture_slot_count > limits.textures) {
        return refuse(
            why,
            ErrorCode::shader_interface_mismatch,
            "more texture slots than the executor binds",
            index,
            entry.texture_slot_count,
            limits.textures);
    }
    if (entry.storage_image_count > limits.storage_images) {
        return refuse(
            why,
            ErrorCode::shader_interface_mismatch,
            "more storage images than the executor binds",
            index,
            entry.storage_image_count,
            limits.storage_images);
    }
    if (entry.uniform_buffer_count > limits.uniform_buffers) {
        return refuse(
            why,
            ErrorCode::shader_interface_mismatch,
            "more uniform buffers than the executor binds",
            index,
            entry.uniform_buffer_count,
            limits.uniform_buffers);
    }

    const std::uint64_t invocations = static_cast<std::uint64_t>(entry.workgroup_x) * entry.workgroup_y
        * entry.workgroup_z;
    if (invocations > limits.workgroup_invocations) {
        return refuse(
            why,
            ErrorCode::shader_interface_mismatch,
            "a workgroup larger than the executor dispatches",
            index,
            invocations,
            limits.workgroup_invocations);
    }
    if (entry.shared_memory_bytes > limits.shared_memory_bytes) {
        return refuse(
            why,
            ErrorCode::shader_interface_mismatch,
            "more shared memory than the executor provides",
            index,
            entry.shared_memory_bytes,
            limits.shared_memory_bytes);
    }

    dksh::ComputeProgram program;
    if (const ErrorCode code = dksh::validate(pass.dksh, program); !succeeded(code)) {
        return refuse(why, code, "a module the executor cannot load", index);
    }
    if (program.block_dim_x != entry.workgroup_x || program.block_dim_y != entry.workgroup_y
        || program.block_dim_z != entry.workgroup_z) {
        return refuse(
            why,
            ErrorCode::shader_interface_mismatch,
            "a module whose workgroup is not the one recorded",
            index,
            program.block_dim_x);
    }
    if (program.gprs != entry.register_count
        || program.per_warp_scratch_bytes != entry.scratch_memory_bytes
        || program.shared_memory_bytes != entry.shared_memory_bytes) {
        return refuse(
            why,
            ErrorCode::cache_integrity_failure,
            "a module whose requirements are not the ones recorded",
            index);
    }

    return true;
}

}

bool accept(const cache::Loaded& cache, const Request& request, Plan& out, Rejection& why) {
    out = Plan{};
    why = Rejection{};

    if (request.output.width == 0 || request.output.height == 0) {
        return refuse(why, ErrorCode::invalid_argument, "no output extent to run at");
    }

    const cache::ManifestHeader& header = cache.header;
    if (const ErrorCode code = cache::validate(header); !succeeded(code)) {
        return refuse(why, code, "a manifest this build does not read");
    }
    if (header.backend_abi_version != cache::backend_abi_version) {
        return refuse(
            why,
            ErrorCode::cache_version_mismatch,
            "a cache prepared for a different backend",
            no_pass,
            header.backend_abi_version,
            cache::backend_abi_version);
    }
    if (header.shader_precision != static_cast<std::uint32_t>(request.precision)) {
        return refuse(
            why,
            ErrorCode::cache_configuration_mismatch,
            "a cache prepared at the other precision",
            no_pass,
            header.shader_precision,
            static_cast<std::uint64_t>(request.precision));
    }
    if (!same_configuration(cache.graph.config, request.config)) {
        return refuse(
            why, ErrorCode::cache_configuration_mismatch, "a cache prepared for another chain");
    }
    if (cache.passes.size() != header.pass_count) {
        return refuse(why, ErrorCode::cache_integrity_failure, "a manifest missing its modules");
    }

    for (std::uint32_t index = 0; index < cache.passes.size(); ++index) {
        const cache::LoadedPass& pass = cache.passes[index];
        if (!check_module(pass, index, request.limits, why)) {
            return false;
        }

        out.max_registers = std::max(out.max_registers, pass.entry.register_count);
        out.max_scratch_bytes_per_warp
            = std::max(out.max_scratch_bytes_per_warp, pass.entry.scratch_memory_bytes);
        out.max_shared_memory_bytes
            = std::max(out.max_shared_memory_bytes, pass.entry.shared_memory_bytes);
    }

    out.output = request.output;
    out.flow = graph::flow_extent(cache.graph.config, request.output);

    out.images.reserve(cache.graph.images.size());
    for (std::uint32_t index = 0; index < cache.graph.images.size(); ++index) {
        const graph::ImageDesc& desc = cache.graph.images[index];
        const graph::Extent extent = graph::evaluate(desc, request.output, out.flow);

        if (extent.width == 0 || extent.height == 0) {
            return refuse(
                why,
                ErrorCode::unsupported,
                "an output extent the chain rounds away to nothing",
                no_pass,
                index);
        }

        const auto format = static_cast<graph::Format>(desc.format);
        const auto role = static_cast<graph::ImageRole>(desc.role);
        const std::uint64_t bytes = static_cast<std::uint64_t>(extent.width) * extent.height
            * graph::bytes_per_pixel(format);

        out.images.push_back(ImagePlan{
            .extent = extent,
            .bytes = bytes,
            .format = format,
            .role = role,
        });

        if (role == graph::ImageRole::history || role == graph::ImageRole::generated) {
            ++out.imported_images;
        } else {
            ++out.owned_images;
            out.owned_image_bytes += bytes;
        }
    }

    if (out.owned_image_bytes > request.memory_budget_bytes) {
        return refuse(
            why,
            ErrorCode::out_of_memory,
            "a chain larger than the memory it is allowed",
            no_pass,
            out.owned_image_bytes,
            request.memory_budget_bytes);
    }

    out.dispatches.reserve(cache.graph.dispatches.size());
    for (std::uint32_t index = 0; index < cache.graph.dispatches.size(); ++index) {
        const graph::DispatchEntry& dispatch = cache.graph.dispatches[index];
        const std::uint32_t block_index = graph::block_index_of(cache.graph, dispatch);

        std::uint32_t pass = no_pass;
        for (std::uint32_t candidate = 0; candidate < cache.passes.size(); ++candidate) {
            if (cache.passes[candidate].entry.block_index == block_index) {
                pass = candidate;
                break;
            }
        }
        if (pass == no_pass) {
            return refuse(
                why,
                ErrorCode::shader_set_unknown,
                "a dispatch whose module the cache does not hold",
                no_pass,
                index);
        }

        if (dispatch.grid_image >= out.images.size()) {
            return refuse(
                why,
                ErrorCode::cache_integrity_failure,
                "a dispatch over an image the graph does not have",
                no_pass,
                index);
        }

        const graph::Extent extent = out.images[dispatch.grid_image].extent;
        const std::uint32_t tile = 1U << dispatch.grid_shift;

        out.dispatches.push_back(DispatchPlan{
            .pass = pass,
            .groups_x = groups_for(extent.width, tile),
            .groups_y = groups_for(extent.height, tile),
        });

        if (dispatch.stage == graph::prepass_stage) {
            ++out.prepass_dispatches;
        } else {
            ++out.generated_frame_dispatches;
        }
    }

    out.descriptor_sets = static_cast<std::uint32_t>(cache.graph.variants.size());
    out.uniform_buffers = cache.graph.uniform_buffer_count;
    return true;
}

bool load(
    const std::string_view root,
    const Digest& key,
    const Request& request,
    cache::Loaded& cache,
    Plan& out,
    Rejection& why) {
    const std::string directory = cache::directory_for(root, key);
    if (const ErrorCode code = cache::read(directory, cache); !succeeded(code)) {
        out = Plan{};
        return refuse(why, code, "no cache here this build can use");
    }
    return accept(cache, request, out, why);
}

}
