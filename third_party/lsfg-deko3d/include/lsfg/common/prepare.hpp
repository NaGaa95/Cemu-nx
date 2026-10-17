// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lsfg/common/cache_store.hpp>
#include <lsfg/common/error.hpp>
#include <lsfg/common/image_graph.hpp>
#include <lsfg/common/shader_set.hpp>
#include <lsfg/common/translate.hpp>

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lsfg::prepare {

struct Options {
    shaders::Precision precision{shaders::Precision::high};
    graph::Config graph{};
    translate::Options translation{};
    bool keep_glsl{};
};

struct ModuleReport {
    std::string name;
    std::uint32_t resource_id{};
    std::uint32_t block_index{};

    std::size_t spirv_bytes{};
    std::size_t glsl_bytes{};
    std::size_t dksh_bytes{};

    std::uint32_t registers{};
    std::uint32_t scratch_bytes{};
    std::uint32_t shared_memory_bytes{};

    bool needed_introduced_sampler{};
    std::uint32_t storage_images_formatted{};

    std::string glsl;
};

struct Result {
    cache::Contents contents;
    std::vector<std::vector<std::uint8_t>> modules;
    std::vector<ModuleReport> reports;

    shaders::ShaderSet set;
    Digest dll_hash{};
    Digest key{};
};

using Progress = std::function<void(const ModuleReport&)>;

[[nodiscard]] ErrorCode run(
    std::span<const std::uint8_t> image,
    const Options& options,
    Result& out,
    const Progress& progress = {});

[[nodiscard]] std::string directory_for(std::string_view root, const Result& result);

}
