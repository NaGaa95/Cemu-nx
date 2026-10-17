// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0-or-later

#include <lsfg/common/translate.hpp>

#include <spirv_glsl.hpp>

#include <cstring>
#include <exception>
#include <string>
#include <utility>
#include <vector>

namespace lsfg::translate {
namespace {

std::string slot_name(const char* const prefix, const std::uint32_t slot) {
    return std::string(prefix) + std::to_string(slot);
}

std::uint32_t binding_of(const spirv_cross::CompilerGLSL& compiler, const spirv_cross::VariableID id) {
    if (!compiler.has_decoration(id, spv::DecorationBinding)) {
        return 0;
    }
    return compiler.get_decoration(id, spv::DecorationBinding);
}

void place(
    spirv_cross::CompilerGLSL& compiler,
    const spirv_cross::VariableID id,
    const std::uint32_t slot) {
    compiler.unset_decoration(id, spv::DecorationDescriptorSet);
    compiler.set_decoration(id, spv::DecorationBinding, slot);
}

ErrorCode check_before_translating(const spirv::Inventory& inventory, const Limits& limits) {
    if (inventory.execution_model != static_cast<std::uint32_t>(spirv::ExecutionModel::gl_compute)) {
        return ErrorCode::unsupported;
    }
    if (inventory.uses_push_constants) {
        return ErrorCode::unsupported;
    }
    if (inventory.local_size_is_specialised) {
        return ErrorCode::unsupported;
    }

    const spirv::DescriptorCounts counts = spirv::count_descriptors(inventory);
    if (counts.storage_buffers > limits.storage_buffers) {
        return ErrorCode::unsupported;
    }
    if (counts.storage_images > limits.storage_images) {
        return ErrorCode::unsupported;
    }
    if (counts.uniform_buffers > limits.uniform_buffers) {
        return ErrorCode::unsupported;
    }

    if (counts.sampled_images + counts.separate_images > limits.textures) {
        return ErrorCode::unsupported;
    }

    return ErrorCode::ok;
}

}

ErrorCode to_glsl(
    const std::span<const std::uint8_t> module_bytes,
    const Options& options,
    Module& out) {
    out = Module{};

    if (!spirv::is_spirv(module_bytes) || module_bytes.size() % sizeof(std::uint32_t) != 0) {
        return ErrorCode::invalid_argument;
    }

    spirv::Inventory inventory;
    if (const ErrorCode result = spirv::inspect_bytes(module_bytes, inventory);
        !succeeded(result)) {
        return result;
    }

    if (const ErrorCode result = check_before_translating(inventory, options.limits);
        !succeeded(result)) {
        return result;
    }

    std::vector<std::uint32_t> words(module_bytes.size() / sizeof(std::uint32_t));
    std::memcpy(words.data(), module_bytes.data(), module_bytes.size());

    if (const ErrorCode result = spirv::patch_storage_image_format(
            words, options.unformatted_storage_image_format, out.patch);
        !succeeded(result)) {
        return result;
    }

    try {
        spirv_cross::CompilerGLSL compiler(std::move(words));

        spirv_cross::CompilerGLSL::Options glsl_options = compiler.get_common_options();
        glsl_options.version = options.glsl_version;
        glsl_options.es = false;
        glsl_options.vulkan_semantics = false;
        compiler.set_common_options(glsl_options);

        const spirv_cross::VariableID dummy_sampler
            = compiler.build_dummy_sampler_for_combined_images();
        out.needed_dummy_sampler = dummy_sampler != 0;
        if (out.needed_dummy_sampler) {
            place(compiler, dummy_sampler, 0);
        }

        compiler.build_combined_image_samplers();

        for (const spirv_cross::CombinedImageSampler& combined :
             compiler.get_combined_image_samplers()) {
            const std::uint32_t slot = out.texture_count++;
            if (slot >= options.limits.textures) {
                return ErrorCode::unsupported;
            }

            const std::string name = slot_name("lsfg_tex", slot);
            compiler.set_name(combined.combined_id, name);
            place(compiler, combined.combined_id, slot);

            out.slots.push_back(SlotAssignment{
                .kind = SlotKind::texture,
                .slot = slot,
                .spirv_binding = binding_of(compiler, combined.image_id),
                .spirv_sampler_binding = binding_of(compiler, combined.sampler_id),
                .uses_dummy_sampler = combined.sampler_id == dummy_sampler,
                .name = name,
            });
        }

        const spirv_cross::ShaderResources resources = compiler.get_shader_resources();

        for (const spirv_cross::Resource& resource : resources.storage_images) {
            const std::uint32_t slot = out.storage_image_count++;
            if (slot >= options.limits.storage_images) {
                return ErrorCode::unsupported;
            }

            const std::string name = slot_name("lsfg_img", slot);
            compiler.set_name(resource.id, name);

            out.slots.push_back(SlotAssignment{
                .kind = SlotKind::storage_image,
                .slot = slot,
                .spirv_binding = binding_of(compiler, resource.id),
                .name = name,
            });
            place(compiler, resource.id, slot);
        }

        for (const spirv_cross::Resource& resource : resources.uniform_buffers) {
            const std::uint32_t slot = out.uniform_buffer_count++;
            if (slot >= options.limits.uniform_buffers) {
                return ErrorCode::unsupported;
            }

            out.slots.push_back(SlotAssignment{
                .kind = SlotKind::uniform_buffer,
                .slot = slot,
                .spirv_binding = binding_of(compiler, resource.id),
                .name = resource.name,
            });
            place(compiler, resource.id, slot);
        }

        out.glsl = compiler.compile();
        if (out.glsl.empty()) {
            return ErrorCode::shader_compile_failed;
        }
    } catch (const std::exception&) {
        return ErrorCode::shader_compile_failed;
    }

    out.local_size_x = inventory.local_size_x;
    out.local_size_y = inventory.local_size_y;
    out.local_size_z = inventory.local_size_z;

    return ErrorCode::ok;
}

std::string_view slot_kind_name(const SlotKind kind) noexcept {
    switch (kind) {
    case SlotKind::uniform_buffer:
        return "uniform buffer";
    case SlotKind::texture:
        return "texture";
    case SlotKind::storage_image:
        return "storage image";
    }
    return "unknown";
}

}
