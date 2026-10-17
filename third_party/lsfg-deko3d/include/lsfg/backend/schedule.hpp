// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lsfg/common/error.hpp>
#include <lsfg/common/image_graph.hpp>

#include <cstdint>
#include <vector>

namespace lsfg::backend {

struct StageRange {
    std::uint32_t first{};
    std::uint32_t count{};
};

inline constexpr std::uint32_t max_frame_cycle = 64;

struct Schedule {
    std::vector<StageRange> stages;

    std::uint32_t cycle{1};

    std::uint32_t warmup_frames{};

    [[nodiscard]] std::uint32_t dispatches() const noexcept;

    [[nodiscard]] std::uint32_t generated_frames() const noexcept;
};

[[nodiscard]] ErrorCode schedule(const graph::Graph& graph, Schedule& out);

}
