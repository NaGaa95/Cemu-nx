// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lsfg/backend/layout.hpp>
#include <lsfg/common/cache_format.hpp>
#include <lsfg/common/cache_store.hpp>
#include <lsfg/common/error.hpp>
#include <lsfg/common/image_graph.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

namespace lsfg::backend {

struct Limits {
    std::uint32_t textures{max_texture_slots};
    std::uint32_t storage_images{max_storage_slots};
    std::uint32_t uniform_buffers{max_uniform_slots};
    std::uint32_t workgroup_invocations{1024};
    std::uint32_t shared_memory_bytes{48U * 1024U};
};

inline constexpr std::uint64_t default_memory_budget_bytes = 64ULL * 1024U * 1024U;

struct Request {
    graph::Config config;
    cache::Precision precision{cache::Precision::high};
    graph::Extent output{};
    std::uint64_t memory_budget_bytes{default_memory_budget_bytes};
    Limits limits{};
};

struct ImagePlan {
    graph::Extent extent;
    std::uint64_t bytes{};
    graph::Format format{};
    graph::ImageRole role{};
};

struct DispatchPlan {
    std::uint32_t pass{};
    std::uint32_t groups_x{};
    std::uint32_t groups_y{};
};

struct Plan {
    graph::Extent output;
    graph::Extent flow;

    std::vector<ImagePlan> images;
    std::vector<DispatchPlan> dispatches;

    std::uint64_t owned_image_bytes{};
    std::uint32_t owned_images{};
    std::uint32_t imported_images{};

    std::uint32_t descriptor_sets{};
    std::uint32_t uniform_buffers{};

    std::uint32_t prepass_dispatches{};
    std::uint32_t generated_frame_dispatches{};

    std::uint32_t max_registers{};
    std::uint32_t max_scratch_bytes_per_warp{};
    std::uint32_t max_shared_memory_bytes{};
};

inline constexpr std::uint32_t no_pass = 0xFFFF'FFFFU;

struct Rejection {
    ErrorCode code{ErrorCode::ok};
    std::string_view reason;
    std::uint32_t pass{no_pass};
    std::uint64_t observed{};
    std::uint64_t allowed{};
};

[[nodiscard]] bool accept(
    const cache::Loaded& cache,
    const Request& request,
    Plan& out,
    Rejection& why);

[[nodiscard]] bool load(
    std::string_view root,
    const Digest& key,
    const Request& request,
    cache::Loaded& cache,
    Plan& out,
    Rejection& why);

}
