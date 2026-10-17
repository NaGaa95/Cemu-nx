// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0-or-later

#include <lsfg/common/prepare.hpp>

#include <lsfg/common/dksh.hpp>
#include <lsfg/common/pe_resources.hpp>
#include <lsfg/common/sha256.hpp>
#include <lsfg/common/spirv_module.hpp>
#include <lsfg/common/version.hpp>

#include <utility>

namespace lsfg::prepare {
namespace {

static_assert(
    static_cast<std::uint8_t>(translate::SlotKind::uniform_buffer)
        == static_cast<std::uint8_t>(cache::SlotKind::uniform_buffer)
    && static_cast<std::uint8_t>(translate::SlotKind::texture)
        == static_cast<std::uint8_t>(cache::SlotKind::texture)
    && static_cast<std::uint8_t>(translate::SlotKind::storage_image)
        == static_cast<std::uint8_t>(cache::SlotKind::storage_image),
    "the manifest records translation's slot kinds unchanged");

const pe::Resource* find_resource(const pe::ResourceTable& table, const std::uint32_t id) noexcept {
    for (const pe::Resource& resource : table.resources) {
        if (resource.type == pe::resource_type_rcdata && resource.id == id) {
            return &resource;
        }
    }
    return nullptr;
}

[[nodiscard]] ErrorCode ordinal_of(
    const std::uint32_t binding,
    const std::uint32_t base,
    std::uint8_t& out) noexcept {
    if (binding < base || binding >= base + shaders::binding_range) {
        return ErrorCode::shader_interface_mismatch;
    }
    out = static_cast<std::uint8_t>(binding - base);
    return ErrorCode::ok;
}

[[nodiscard]] ErrorCode describe_slots(
    const translate::Module& module,
    std::vector<cache::SlotEntry>& out) {
    out.clear();
    out.reserve(module.slots.size());

    for (const translate::SlotAssignment& assignment : module.slots) {
        if (assignment.slot > 0xFFU) {
            return ErrorCode::shader_interface_mismatch;
        }

        cache::SlotEntry entry{
            .kind = static_cast<std::uint8_t>(assignment.kind),
            .slot = static_cast<std::uint8_t>(assignment.slot),
        };

        switch (assignment.kind) {
        case translate::SlotKind::uniform_buffer:
            if (const ErrorCode result = ordinal_of(
                    assignment.spirv_binding, shaders::binding_base_uniform_buffer, entry.ordinal);
                !succeeded(result)) {
                return result;
            }
            break;
        case translate::SlotKind::texture:
            if (const ErrorCode result
                = ordinal_of(assignment.spirv_binding, shaders::binding_base_image, entry.ordinal);
                !succeeded(result)) {
                return result;
            }
            if (assignment.uses_dummy_sampler) {
                entry.sampler_ordinal = cache::introduced_sampler;
            } else if (const ErrorCode result = ordinal_of(
                           assignment.spirv_sampler_binding,
                           shaders::binding_base_sampler,
                           entry.sampler_ordinal);
                       !succeeded(result)) {
                return result;
            }
            break;
        case translate::SlotKind::storage_image:
            if (const ErrorCode result = ordinal_of(
                    assignment.spirv_binding, shaders::binding_base_storage_image, entry.ordinal);
                !succeeded(result)) {
                return result;
            }
            break;
        default:
            return ErrorCode::shader_interface_mismatch;
        }

        out.push_back(entry);
    }

    return ErrorCode::ok;
}

}

ErrorCode run(
    const std::span<const std::uint8_t> image,
    const Options& options,
    Result& out,
    const Progress& progress) {
    out = Result{};

    if (image.empty()) {
        return ErrorCode::invalid_argument;
    }

    out.dll_hash = sha256(image);
    out.key = cache::cache_key(cache::CacheKeyInputs{
        .dll_bytes = image,
        .extractor_version = cache::extractor_version,
        .spirv_cross_revision = version::spirv_cross_revision,
        .uam_revision = version::uam_revision,
        .translation_options = options.translation.glsl_version,
        .graph_options = cache::pack_options(
            options.precision == shaders::Precision::high ? cache::Precision::high
                                                          : cache::Precision::low,
            options.graph),
        .backend_abi_version = cache::backend_abi_version,
    });

    pe::ResourceTable table;
    if (const ErrorCode result = pe::enumerate_resources(image, table); !succeeded(result)) {
        return result;
    }
    if (const ErrorCode result = shaders::identify(image, table.resources, out.set);
        !succeeded(result)) {
        return result;
    }

    std::vector<shaders::ModuleRequest> requests;
    if (const ErrorCode result = shaders::required_modules(
            out.set, options.precision, options.graph.performance, requests);
        !succeeded(result)) {
        return result;
    }

    if (const ErrorCode result = graph::build(options.graph, out.contents.graph);
        !succeeded(result)) {
        return result;
    }

    cache::initialize(out.contents.header);
    out.contents.header.dll_hash = out.dll_hash;
    out.contents.header.dll_size = image.size();
    out.contents.header.translation_options = options.translation.glsl_version;
    out.contents.header.shader_first_resource_id = out.set.first_resource_id;
    out.contents.header.shader_block_size = out.set.block_size;
    out.contents.header.shader_precision = static_cast<std::uint32_t>(
        options.precision == shaders::Precision::high ? cache::Precision::high : cache::Precision::low);

    const auto copy_revision = [](cache::Revision& target, const std::string_view text) {
        for (std::size_t index = 0; index < target.size() && index < text.size(); ++index) {
            target[index] = static_cast<std::uint8_t>(text[index]);
        }
    };
    copy_revision(out.contents.header.spirv_cross_revision, version::spirv_cross_revision);
    copy_revision(out.contents.header.uam_revision, version::uam_revision);

    out.modules.reserve(requests.size());
    out.reports.reserve(requests.size());
    out.contents.passes.reserve(requests.size());

    for (const shaders::ModuleRequest& request : requests) {
        const pe::Resource* const resource = find_resource(table, request.resource_id);
        if (resource == nullptr) {
            return ErrorCode::shader_set_unknown;
        }

        const std::span<const std::uint8_t> data = pe::resource_data(image, *resource);

        spirv::Inventory inventory;
        if (const ErrorCode result = spirv::inspect_bytes(data, inventory); !succeeded(result)) {
            return result;
        }

        translate::Module module;
        if (const ErrorCode result = translate::to_glsl(data, options.translation, module);
            !succeeded(result)) {
            return result;
        }

        dksh::Blob blob;
        if (const ErrorCode result = dksh::compile(module.glsl, blob); !succeeded(result)) {
            return result;
        }

        if (blob.program.block_dim_x != module.local_size_x
            || blob.program.block_dim_y != module.local_size_y
            || blob.program.block_dim_z != module.local_size_z) {
            return ErrorCode::shader_interface_mismatch;
        }

        const spirv::DescriptorCounts counts = spirv::count_descriptors(inventory);

        cache::PassInput pass;
        pass.entry.resource_id = request.resource_id;
        pass.entry.block_index = request.block_index;
        pass.entry.scratch_memory_bytes = blob.program.per_warp_scratch_bytes;
        pass.entry.workgroup_x = static_cast<std::uint16_t>(module.local_size_x);
        pass.entry.workgroup_y = static_cast<std::uint16_t>(module.local_size_y);
        pass.entry.workgroup_z = static_cast<std::uint16_t>(module.local_size_z);
        pass.entry.image_count = counts.sampled_images + counts.separate_images;
        pass.entry.storage_image_count = counts.storage_images;
        pass.entry.sampler_count = counts.samplers;
        pass.entry.uniform_buffer_count = counts.uniform_buffers;
        pass.entry.texture_slot_count = module.texture_count;
        pass.entry.register_count = blob.program.gprs;
        pass.entry.shared_memory_bytes = blob.program.shared_memory_bytes;

        if (const ErrorCode result = describe_slots(module, pass.slots); !succeeded(result)) {
            return result;
        }

        ModuleReport report;
        report.name = std::string{request.name};
        report.resource_id = request.resource_id;
        report.block_index = request.block_index;
        report.spirv_bytes = data.size();
        report.glsl_bytes = module.glsl.size();
        report.dksh_bytes = blob.bytes.size();
        report.registers = blob.program.gprs;
        report.scratch_bytes = blob.program.per_warp_scratch_bytes;
        report.shared_memory_bytes = blob.program.shared_memory_bytes;
        report.needed_introduced_sampler = module.needed_dummy_sampler;
        report.storage_images_formatted = module.patch.images_formatted;
        if (options.keep_glsl) {
            report.glsl = std::move(module.glsl);
        }

        out.modules.push_back(std::move(blob.bytes));
        out.contents.passes.push_back(std::move(pass));
        out.reports.push_back(std::move(report));

        if (progress) {
            progress(out.reports.back());
        }
    }

    for (std::size_t index = 0; index < out.contents.passes.size(); ++index) {
        out.contents.passes[index].dksh = out.modules[index];
    }

    return ErrorCode::ok;
}

std::string directory_for(const std::string_view root, const Result& result) {
    return cache::directory_for(root, result.key);
}

}
