// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0-or-later

#include <lsfg/common/shader_set.hpp>

#include <lsfg/common/spirv_module.hpp>

#include <algorithm>
#include <array>

namespace lsfg::shaders {
namespace {

constexpr std::uint32_t minimum_block_size = 26;

constexpr std::array<ChainSlot, 26> chain{{
    {"mipmaps", 1, false},
    {"alpha.0", 13, true},
    {"alpha.1", 14, true},
    {"alpha.2", 15, true},
    {"alpha.3", 16, true},
    {"beta.0", 21, true},
    {"beta.1", 22, true},
    {"beta.2", 23, true},
    {"beta.3", 24, true},
    {"beta.4", 25, true},
    {"gamma.0", 3, true},
    {"gamma.1", 5, true},
    {"gamma.2", 6, true},
    {"gamma.3", 7, true},
    {"gamma.4", 8, true},
    {"delta.0", 3, true},
    {"delta.1", 9, true},
    {"delta.2", 10, true},
    {"delta.3", 11, true},
    {"delta.4", 12, true},
    {"delta.5", 4, true},
    {"delta.6", 17, true},
    {"delta.7", 18, true},
    {"delta.8", 19, true},
    {"delta.9", 20, true},
    {"generate", 2, false},
}};

struct SpirvResource {
    std::uint32_t id{};
    std::size_t index{};
};

}

std::span<const ChainSlot> chain_slots() noexcept {
    return chain;
}

ErrorCode identify(
    const std::span<const std::uint8_t> image,
    const std::span<const pe::Resource> resources,
    ShaderSet& out) {
    out = ShaderSet{};

    std::vector<SpirvResource> modules;
    for (std::size_t index = 0; index < resources.size(); ++index) {
        const pe::Resource& resource = resources[index];
        if (resource.type != pe::resource_type_rcdata) {
            continue;
        }
        if (!spirv::is_spirv(pe::resource_data(image, resource))) {
            continue;
        }
        modules.push_back(SpirvResource{.id = resource.id, .index = index});
    }

    if (modules.empty()) {
        return ErrorCode::shader_set_unknown;
    }

    std::ranges::sort(modules, [](const SpirvResource& left, const SpirvResource& right) {
        return left.id < right.id;
    });

    for (std::size_t index = 1; index < modules.size(); ++index) {
        if (modules[index].id != modules[index - 1U].id + 1U) {
            return ErrorCode::shader_set_unknown;
        }
    }

    if ((modules.size() % 2U) != 0) {
        return ErrorCode::shader_set_unknown;
    }

    const auto block_size = static_cast<std::uint32_t>(modules.size() / 2U);
    if (block_size < minimum_block_size) {
        return ErrorCode::shader_set_unknown;
    }

    std::array<bool, 2> declares_float16{false, false};
    for (std::size_t index = 0; index < modules.size(); ++index) {
        spirv::Inventory inventory;
        const ErrorCode result
            = spirv::inspect_bytes(pe::resource_data(image, resources[modules[index].index]), inventory);
        if (!succeeded(result)) {
            return result;
        }

        if (inventory.execution_model != static_cast<std::uint32_t>(spirv::ExecutionModel::gl_compute)) {
            return ErrorCode::shader_set_unknown;
        }

        const std::size_t block = index / block_size;
        if (spirv::has_capability(inventory, spirv::Capability::float16)) {
            declares_float16[block] = true;
        }
    }

    if (declares_float16[0] == declares_float16[1]) {
        return ErrorCode::shader_set_unknown;
    }

    out.first_resource_id = modules.front().id;
    out.block_size = block_size;
    out.low_precision_block = declares_float16[0] ? 0U : 1U;
    out.high_precision_block = declares_float16[0] ? 1U : 0U;
    return ErrorCode::ok;
}

std::uint32_t resource_id_for(
    const ShaderSet& set,
    const Precision precision,
    const std::uint32_t block_index) noexcept {
    const std::uint32_t block
        = precision == Precision::low ? set.low_precision_block : set.high_precision_block;
    return set.first_resource_id + (block * set.block_size) + block_index;
}

ErrorCode required_modules(
    const ShaderSet& set,
    const Precision precision,
    const bool performance,
    std::vector<ModuleRequest>& out) {
    out.clear();

    if (set.block_size < minimum_block_size) {
        return ErrorCode::shader_set_unknown;
    }

    for (const ChainSlot& slot : chain_slots()) {
        const std::uint32_t block_index
            = slot.block_index + ((performance && slot.has_performance_variant) ? performance_offset : 0U);
        if (block_index >= set.block_size) {
            return ErrorCode::shader_set_unknown;
        }

        const bool already_present = std::ranges::any_of(out, [block_index](const ModuleRequest& request) {
            return request.block_index == block_index;
        });
        if (already_present) {
            continue;
        }

        out.push_back(ModuleRequest{
            .name = slot.name,
            .block_index = block_index,
            .resource_id = resource_id_for(set, precision, block_index),
        });
    }

    std::ranges::sort(out, [](const ModuleRequest& left, const ModuleRequest& right) {
        return left.block_index < right.block_index;
    });
    return ErrorCode::ok;
}

std::string_view precision_name(const Precision precision) noexcept {
    return precision == Precision::low ? "low (declares Float16)" : "high (no Float16)";
}

}
