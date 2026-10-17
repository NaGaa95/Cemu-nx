// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0-or-later

#include <lsfg/backend/schedule.hpp>

#include <cstddef>
#include <numeric>

namespace lsfg::backend {
namespace {

[[nodiscard]] std::uint32_t combine_cycle(
    const std::uint32_t cycle,
    const std::uint32_t variants) noexcept {
    if (variants == 0) {
        return cycle;
    }
    const std::uint64_t combined = std::lcm<std::uint64_t, std::uint64_t>(cycle, variants);
    return combined > max_frame_cycle ? 0U : static_cast<std::uint32_t>(combined);
}

[[nodiscard]] ErrorCode lay_out_stages(const graph::Graph& graph, Schedule& out) {
    out.stages.assign(graph.config.generated_frames + 1U, StageRange{});

    std::uint32_t stage = 0;
    for (std::uint32_t index = 0; index < graph.dispatches.size(); ++index) {
        const std::uint32_t entry_stage = graph.dispatches[index].stage;
        if (entry_stage >= out.stages.size()) {
            return ErrorCode::cache_integrity_failure;
        }

        if (entry_stage != stage) {

            if (entry_stage != stage + 1U) {
                return ErrorCode::presentation_sequence_invalid;
            }
            stage = entry_stage;
            out.stages[stage].first = index;
        }
        ++out.stages[stage].count;
    }

    if (stage + 1U != out.stages.size()) {
        return ErrorCode::presentation_sequence_invalid;
    }
    for (const StageRange& range : out.stages) {
        if (range.count == 0) {
            return ErrorCode::presentation_sequence_invalid;
        }
    }
    return ErrorCode::ok;
}

[[nodiscard]] bool needs_writing(const graph::ImageDesc& desc) noexcept {
    return static_cast<graph::ImageRole>(desc.role) == graph::ImageRole::internal;
}

[[nodiscard]] bool run_frame(
    const graph::Graph& graph,
    const std::uint32_t frame,
    std::vector<bool>& written) {
    bool clean = true;

    for (const graph::DispatchEntry& entry : graph.dispatches) {
        const graph::VariantEntry& variant
            = graph.variants[entry.variant_first + (frame % entry.variant_count)];
        const std::uint32_t* const bindings = graph.bindings.data() + variant.binding_first;

        for (std::uint32_t index = 0; index < variant.texture_count; ++index) {
            const std::uint32_t image = bindings[index];
            clean = clean && (!needs_writing(graph.images[image]) || written[image]);
        }
        for (std::uint32_t index = 0; index < variant.storage_count; ++index) {
            written[bindings[variant.texture_count + index]] = true;
        }
    }

    return clean;
}

[[nodiscard]] ErrorCode measure_warmup(const graph::Graph& graph, Schedule& out) {
    std::vector<bool> written(graph.images.size(), false);

    const std::uint32_t limit = 2U * out.cycle;
    for (std::uint32_t frame = 0; frame < limit; ++frame) {
        if (run_frame(graph, frame, written)) {
            continue;
        }
        if (frame >= out.cycle) {
            return ErrorCode::cache_integrity_failure;
        }
        out.warmup_frames = frame + 1U;
    }
    return ErrorCode::ok;
}

}

std::uint32_t Schedule::dispatches() const noexcept {
    std::uint32_t total = 0;
    for (const StageRange& range : stages) {
        total += range.count;
    }
    return total;
}

std::uint32_t Schedule::generated_frames() const noexcept {
    return stages.empty() ? 0U : static_cast<std::uint32_t>(stages.size() - 1U);
}

ErrorCode schedule(const graph::Graph& graph, Schedule& out) {
    out = Schedule{};

    if (const ErrorCode code = graph::validate(graph); !succeeded(code)) {
        return code;
    }
    if (const ErrorCode code = lay_out_stages(graph, out); !succeeded(code)) {
        return code;
    }

    for (const graph::DispatchEntry& entry : graph.dispatches) {
        out.cycle = combine_cycle(out.cycle, entry.variant_count);
        if (out.cycle == 0) {
            return ErrorCode::cache_integrity_failure;
        }
    }

    return measure_warmup(graph, out);
}

}
