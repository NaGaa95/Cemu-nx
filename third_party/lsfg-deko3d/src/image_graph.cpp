// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0-or-later

#include <lsfg/common/image_graph.hpp>

#include <lsfg/common/shader_set.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>

namespace lsfg::graph {
namespace {

constexpr std::uint32_t pyramid_levels = 7;

constexpr std::uint32_t refined_levels = 3;

constexpr std::uint32_t base_temporal_depth = 3;
constexpr std::uint32_t temporal_depth = 2;

constexpr std::uint32_t invalid_slot = std::numeric_limits<std::uint32_t>::max();

struct VariantDraft {
    std::vector<std::uint32_t> textures;
    std::vector<std::uint32_t> storages;
    std::vector<Sampler> samplers;
    std::uint8_t uniform_buffer{no_uniform_buffer};
};

[[nodiscard]] ImageDesc halved(ImageDesc desc) noexcept {
    ++desc.ceil_halvings;
    return desc;
}

[[nodiscard]] ImageDesc shifted(ImageDesc desc) noexcept {
    if (desc.ceil_halvings == 0) {
        ++desc.shift_before;
    } else {
        ++desc.shift_after;
    }
    return desc;
}

[[nodiscard]] ImageDesc as_format(ImageDesc desc, const Format format) noexcept {
    desc.format = static_cast<std::uint8_t>(format);
    return desc;
}

[[nodiscard]] std::uint32_t slot_index(const std::string_view name) noexcept {
    const std::span<const shaders::ChainSlot> slots = shaders::chain_slots();
    for (std::size_t index = 0; index < slots.size(); ++index) {
        if (slots[index].name == name) {
            return static_cast<std::uint32_t>(index);
        }
    }
    return invalid_slot;
}

class Builder {
public:
    explicit Builder(Graph& graph) noexcept : graph_(&graph) {}

    [[nodiscard]] std::uint32_t add(const ImageDesc& desc) {
        const auto index = static_cast<std::uint32_t>(graph_->images.size());
        graph_->images.push_back(desc);
        return index;
    }

    [[nodiscard]] std::vector<std::uint32_t> add_many(const ImageDesc& desc, const std::uint32_t count) {
        std::vector<std::uint32_t> indices;
        indices.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index) {
            indices.push_back(add(desc));
        }
        return indices;
    }

    [[nodiscard]] ErrorCode dispatch(
        const std::string_view name,
        const std::uint8_t grid_shift,
        const std::uint8_t stage,
        const std::span<const VariantDraft> variants) {
        const std::uint32_t slot = slot_index(name);
        if (slot == invalid_slot || variants.empty()) {
            return ErrorCode::invalid_state;
        }

        const auto variant_first = static_cast<std::uint32_t>(graph_->variants.size());
        for (const VariantDraft& draft : variants) {
            if (draft.storages.empty() || draft.samplers.size() > max_dispatch_samplers) {
                return ErrorCode::invalid_state;
            }

            VariantEntry entry{
                .binding_first = static_cast<std::uint32_t>(graph_->bindings.size()),
                .texture_count = static_cast<std::uint16_t>(draft.textures.size()),
                .storage_count = static_cast<std::uint16_t>(draft.storages.size()),
                .sampler_count = static_cast<std::uint8_t>(draft.samplers.size()),
                .uniform_buffer = draft.uniform_buffer,
            };
            for (std::size_t index = 0; index < draft.samplers.size(); ++index) {
                entry.samplers[index] = static_cast<std::uint8_t>(draft.samplers[index]);
            }

            graph_->bindings.insert(graph_->bindings.end(), draft.textures.begin(), draft.textures.end());
            graph_->bindings.insert(graph_->bindings.end(), draft.storages.begin(), draft.storages.end());
            graph_->variants.push_back(entry);
        }

        graph_->dispatches.push_back(DispatchEntry{
            .slot = slot,
            .grid_image = variants.front().storages.front(),
            .variant_first = variant_first,
            .variant_count = static_cast<std::uint16_t>(variants.size()),
            .grid_shift = grid_shift,
            .stage = stage,
        });
        return ErrorCode::ok;
    }

private:
    Graph* graph_;
};

void append(std::vector<std::uint32_t>& target, const std::span<const std::uint32_t> source) {
    target.insert(target.end(), source.begin(), source.end());
}

}

ErrorCode build(const Config& config, Graph& out) {
    out = Graph{};

    if (config.generated_frames == 0 || config.generated_frames > max_generated_frames) {
        return ErrorCode::invalid_argument;
    }
    if (config.flow_numerator == 0 || config.flow_denominator == 0
        || config.flow_numerator > config.flow_denominator) {
        return ErrorCode::invalid_argument;
    }

    out.config = config;
    out.uniform_buffer_count = 1U + config.generated_frames;

    const std::uint32_t width = config.performance ? 1U : 2U;
    const Format frame_format = config.hdr ? Format::rgba16f : Format::rgba8;

    Builder builder{out};

    const std::array<std::uint32_t, history_image_count> history{
        builder.add(ImageDesc{
            .base = static_cast<std::uint8_t>(ExtentBase::output),
            .format = static_cast<std::uint8_t>(frame_format),
            .role = static_cast<std::uint8_t>(ImageRole::history)}),
        builder.add(ImageDesc{
            .base = static_cast<std::uint8_t>(ExtentBase::output),
            .format = static_cast<std::uint8_t>(frame_format),
            .role = static_cast<std::uint8_t>(ImageRole::history)}),
    };

    std::vector<std::uint32_t> generated;
    generated.reserve(config.generated_frames);
    for (std::uint32_t frame = 0; frame < config.generated_frames; ++frame) {
        generated.push_back(builder.add(ImageDesc{
            .base = static_cast<std::uint8_t>(ExtentBase::output),
            .format = static_cast<std::uint8_t>(frame_format),
            .role = static_cast<std::uint8_t>(ImageRole::generated)}));
    }

    const std::uint32_t black = builder.add(ImageDesc{
        .base = static_cast<std::uint8_t>(ExtentBase::fixed),
        .fixed_width = 4,
        .fixed_height = 4,
        .format = static_cast<std::uint8_t>(Format::rgba8),
        .role = static_cast<std::uint8_t>(ImageRole::constant)});

    std::array<ImageDesc, pyramid_levels> mip_desc{};
    std::array<std::uint32_t, pyramid_levels> mip{};
    mip_desc[0] = ImageDesc{
        .base = static_cast<std::uint8_t>(ExtentBase::flow),
        .format = static_cast<std::uint8_t>(Format::r8),
        .role = static_cast<std::uint8_t>(ImageRole::internal)};
    for (std::uint32_t level = 1; level < pyramid_levels; ++level) {
        mip_desc[level] = shifted(mip_desc[level - 1U]);
    }
    for (std::uint32_t level = 0; level < pyramid_levels; ++level) {
        mip[level] = builder.add(mip_desc[level]);
    }

    {
        std::array<VariantDraft, 2> variants;
        for (std::size_t index = 0; index < variants.size(); ++index) {
            variants[index] = VariantDraft{
                .textures = {history[index]},
                .storages = {mip.begin(), mip.end()},
                .samplers = {Sampler::border_black},
                .uniform_buffer = 0,
            };
        }
        if (const ErrorCode result = builder.dispatch("mipmaps", 6, prepass_stage, variants);
            !succeeded(result)) {
            return result;
        }
    }

    struct AlphaLevel {
        ImageDesc quarter;
        std::vector<std::vector<std::uint32_t>> history;
    };
    std::array<AlphaLevel, pyramid_levels> alpha{};

    for (std::uint32_t step = 0; step < pyramid_levels; ++step) {
        const std::uint32_t level = pyramid_levels - 1U - step;
        const ImageDesc half = as_format(halved(mip_desc[level]), Format::rgba8);
        const ImageDesc quarter = halved(half);

        const std::vector<std::uint32_t> first = builder.add_many(half, width);
        const std::vector<std::uint32_t> second = builder.add_many(half, width);
        const std::vector<std::uint32_t> refined = builder.add_many(quarter, 2U * width);

        const std::array<VariantDraft, 1> extract{VariantDraft{
            .textures = {mip[level]},
            .storages = first,
            .samplers = {Sampler::border_black}}};
        if (const ErrorCode result = builder.dispatch("alpha.0", 3, prepass_stage, extract);
            !succeeded(result)) {
            return result;
        }

        const std::array<VariantDraft, 1> filter{VariantDraft{
            .textures = first,
            .storages = second,
            .samplers = {Sampler::border_black}}};
        if (const ErrorCode result = builder.dispatch("alpha.1", 3, prepass_stage, filter);
            !succeeded(result)) {
            return result;
        }

        const std::array<VariantDraft, 1> reduce{VariantDraft{
            .textures = second,
            .storages = refined,
            .samplers = {Sampler::border_black}}};
        if (const ErrorCode result = builder.dispatch("alpha.2", 3, prepass_stage, reduce);
            !succeeded(result)) {
            return result;
        }

        const std::uint32_t depth = level == 0 ? base_temporal_depth : temporal_depth;
        std::vector<VariantDraft> retain;
        retain.reserve(depth);
        for (std::uint32_t slot = 0; slot < depth; ++slot) {
            std::vector<std::uint32_t> images = builder.add_many(quarter, 2U * width);
            retain.push_back(VariantDraft{
                .textures = refined,
                .storages = images,
                .samplers = {Sampler::border_black}});
            alpha[level].history.push_back(std::move(images));
        }
        alpha[level].quarter = quarter;

        if (const ErrorCode result = builder.dispatch("alpha.3", 3, prepass_stage, retain);
            !succeeded(result)) {
            return result;
        }
    }

    const ImageDesc base_quarter = alpha[0].quarter;
    const auto& base_history = alpha[0].history;
    const auto base_depth = static_cast<std::uint32_t>(base_history.size());

    const std::vector<std::uint32_t> beta_first = builder.add_many(base_quarter, 2);
    {
        std::vector<VariantDraft> variants;
        variants.reserve(base_depth);
        for (std::uint32_t frame = 0; frame < base_depth; ++frame) {
            VariantDraft draft{
                .textures = {},
                .storages = beta_first,
                .samplers = {Sampler::border_white}};
            for (std::uint32_t age = 2; age > 0; --age) {
                append(draft.textures, base_history[(frame + base_depth - age) % base_depth]);
            }
            append(draft.textures, base_history[frame]);
            variants.push_back(std::move(draft));
        }
        if (const ErrorCode result = builder.dispatch("beta.0", 3, prepass_stage, variants);
            !succeeded(result)) {
            return result;
        }
    }

    const std::vector<std::uint32_t> beta_scratch0 = builder.add_many(base_quarter, 2);
    const std::vector<std::uint32_t> beta_scratch1 = builder.add_many(base_quarter, 2);

    std::vector<std::uint32_t> beta_pyramid;
    beta_pyramid.reserve(pyramid_levels - 1U);
    {
        ImageDesc desc = as_format(base_quarter, Format::r8);
        for (std::uint32_t level = 0; level + 1U < pyramid_levels; ++level) {
            beta_pyramid.push_back(builder.add(desc));
            desc = shifted(desc);
        }
    }

    {
        const std::array<VariantDraft, 1> first{VariantDraft{
            .textures = beta_first, .storages = beta_scratch0, .samplers = {Sampler::border_black}}};
        if (const ErrorCode result = builder.dispatch("beta.1", 3, prepass_stage, first);
            !succeeded(result)) {
            return result;
        }
        const std::array<VariantDraft, 1> second{VariantDraft{
            .textures = beta_scratch0, .storages = beta_scratch1, .samplers = {Sampler::border_black}}};
        if (const ErrorCode result = builder.dispatch("beta.2", 3, prepass_stage, second);
            !succeeded(result)) {
            return result;
        }
        const std::array<VariantDraft, 1> third{VariantDraft{
            .textures = beta_scratch1, .storages = beta_scratch0, .samplers = {Sampler::border_black}}};
        if (const ErrorCode result = builder.dispatch("beta.3", 3, prepass_stage, third);
            !succeeded(result)) {
            return result;
        }
        const std::array<VariantDraft, 1> reduce{VariantDraft{
            .textures = beta_scratch0,
            .storages = beta_pyramid,
            .samplers = {Sampler::border_black},
            .uniform_buffer = 0}};
        if (const ErrorCode result = builder.dispatch("beta.4", 5, prepass_stage, reduce);
            !succeeded(result)) {
            return result;
        }
    }

    for (std::uint32_t frame = 0; frame < config.generated_frames; ++frame) {
        const auto stage = static_cast<std::uint8_t>(1U + frame);
        const auto uniform_buffer = static_cast<std::uint8_t>(1U + frame);

        std::array<std::uint32_t, pyramid_levels> gamma_result{};
        std::array<std::uint32_t, refined_levels> delta_result0{};
        std::array<std::uint32_t, refined_levels> delta_result1{};

        for (std::uint32_t step = 0; step < pyramid_levels; ++step) {
            const std::uint32_t level = pyramid_levels - 1U - step;
            const ImageDesc quarter = alpha[level].quarter;
            const auto& retained = alpha[level].history;
            const auto depth = static_cast<std::uint32_t>(retained.size());

            const std::uint32_t coarser = step == 0 ? black : gamma_result[step - 1U];

            const std::uint32_t guide = beta_pyramid[step == 0 ? pyramid_levels - 2U : level];

            const std::vector<std::uint32_t> estimate = builder.add_many(quarter, 3);
            {
                std::vector<VariantDraft> variants;
                variants.reserve(depth);
                for (std::uint32_t phase = 0; phase < depth; ++phase) {
                    VariantDraft draft{
                        .textures = {},
                        .storages = estimate,
                        .samplers = {Sampler::border_white, Sampler::edge},
                        .uniform_buffer = uniform_buffer};
                    append(draft.textures, retained[(phase + depth - 1U) % depth]);
                    append(draft.textures, retained[phase]);
                    draft.textures.push_back(coarser);
                    variants.push_back(std::move(draft));
                }
                if (const ErrorCode result = builder.dispatch("gamma.0", 3, stage, variants);
                    !succeeded(result)) {
                    return result;
                }
            }

            const std::vector<std::uint32_t> scratch0 = builder.add_many(quarter, 2U * width);
            const std::vector<std::uint32_t> scratch1 = builder.add_many(quarter, 2U * width);
            const std::uint32_t result_image = builder.add(as_format(quarter, Format::rgba16f));

            const std::array<VariantDraft, 1> first{VariantDraft{
                .textures = estimate, .storages = scratch0, .samplers = {Sampler::border_black}}};
            if (const ErrorCode result = builder.dispatch("gamma.1", 3, stage, first);
                !succeeded(result)) {
                return result;
            }
            const std::array<VariantDraft, 1> second{VariantDraft{
                .textures = scratch0, .storages = scratch1, .samplers = {Sampler::border_black}}};
            if (const ErrorCode result = builder.dispatch("gamma.2", 3, stage, second);
                !succeeded(result)) {
                return result;
            }
            const std::array<VariantDraft, 1> third{VariantDraft{
                .textures = scratch1, .storages = scratch0, .samplers = {Sampler::border_black}}};
            if (const ErrorCode result = builder.dispatch("gamma.3", 3, stage, third);
                !succeeded(result)) {
                return result;
            }

            VariantDraft combine{
                .textures = scratch0,
                .storages = {result_image},
                .samplers = {Sampler::border_black, Sampler::edge},
                .uniform_buffer = uniform_buffer};
            combine.textures.push_back(coarser);
            combine.textures.push_back(guide);
            const std::array<VariantDraft, 1> combine_variants{std::move(combine)};
            if (const ErrorCode result = builder.dispatch("gamma.4", 3, stage, combine_variants);
                !succeeded(result)) {
                return result;
            }
            gamma_result[step] = result_image;

            if (step + refined_levels < pyramid_levels) {
                continue;
            }

            const std::uint32_t refined_step = step - (pyramid_levels - refined_levels);
            const std::uint32_t previous0 = refined_step == 0 ? black : delta_result0[refined_step - 1U];
            const std::uint32_t previous1 = refined_step == 0 ? black : delta_result1[refined_step - 1U];

            const std::vector<std::uint32_t> occlusion = builder.add_many(quarter, 3);
            const std::vector<std::uint32_t> mask = builder.add_many(quarter, width);
            {
                std::vector<VariantDraft> variants;
                std::vector<VariantDraft> mask_variants;
                variants.reserve(depth);
                mask_variants.reserve(depth);
                for (std::uint32_t phase = 0; phase < depth; ++phase) {
                    VariantDraft draft{
                        .textures = {},
                        .storages = occlusion,
                        .samplers = {Sampler::border_white, Sampler::edge},
                        .uniform_buffer = uniform_buffer};
                    append(draft.textures, retained[(phase + depth - 1U) % depth]);
                    append(draft.textures, retained[phase]);
                    draft.textures.push_back(previous0);
                    variants.push_back(std::move(draft));

                    VariantDraft second_draft{
                        .textures = {},
                        .storages = mask,
                        .samplers = {Sampler::border_white, Sampler::edge},
                        .uniform_buffer = uniform_buffer};
                    append(second_draft.textures, retained[(phase + depth - 1U) % depth]);
                    append(second_draft.textures, retained[phase]);
                    second_draft.textures.push_back(gamma_result[step - 1U]);
                    second_draft.textures.push_back(previous0);
                    mask_variants.push_back(std::move(second_draft));
                }
                if (const ErrorCode result = builder.dispatch("delta.0", 3, stage, variants);
                    !succeeded(result)) {
                    return result;
                }
                if (const ErrorCode result = builder.dispatch("delta.5", 3, stage, mask_variants);
                    !succeeded(result)) {
                    return result;
                }
            }

            const std::vector<std::uint32_t> delta_scratch0 = builder.add_many(quarter, 2U * width);
            const std::vector<std::uint32_t> delta_scratch1 = builder.add_many(quarter, 2U * width);
            const std::uint32_t delta_image0 = builder.add(as_format(quarter, Format::rgba16f));
            const std::uint32_t delta_image1 = builder.add(as_format(quarter, Format::rgba16f));

            const std::array<VariantDraft, 1> delta_first{VariantDraft{
                .textures = occlusion, .storages = delta_scratch0, .samplers = {Sampler::border_black}}};
            if (const ErrorCode result = builder.dispatch("delta.1", 3, stage, delta_first);
                !succeeded(result)) {
                return result;
            }
            const std::array<VariantDraft, 1> delta_second{VariantDraft{
                .textures = delta_scratch0,
                .storages = delta_scratch1,
                .samplers = {Sampler::border_black}}};
            if (const ErrorCode result = builder.dispatch("delta.2", 3, stage, delta_second);
                !succeeded(result)) {
                return result;
            }
            const std::array<VariantDraft, 1> delta_third{VariantDraft{
                .textures = delta_scratch1,
                .storages = delta_scratch0,
                .samplers = {Sampler::border_black}}};
            if (const ErrorCode result = builder.dispatch("delta.3", 3, stage, delta_third);
                !succeeded(result)) {
                return result;
            }

            VariantDraft delta_combine{
                .textures = delta_scratch0,
                .storages = {delta_image0},
                .samplers = {Sampler::border_black, Sampler::edge},
                .uniform_buffer = uniform_buffer};
            delta_combine.textures.push_back(previous0);
            delta_combine.textures.push_back(guide);
            const std::array<VariantDraft, 1> delta_combine_variants{std::move(delta_combine)};
            if (const ErrorCode result = builder.dispatch("delta.4", 3, stage, delta_combine_variants);
                !succeeded(result)) {
                return result;
            }

            const std::span<const std::uint32_t> narrow0{delta_scratch0.data(), width};
            const std::span<const std::uint32_t> narrow1{delta_scratch1.data(), width};

            const std::array<VariantDraft, 1> mask_first{VariantDraft{
                .textures = mask,
                .storages = {narrow0.begin(), narrow0.end()},
                .samplers = {Sampler::border_black}}};
            if (const ErrorCode result = builder.dispatch("delta.6", 3, stage, mask_first);
                !succeeded(result)) {
                return result;
            }
            const std::array<VariantDraft, 1> mask_second{VariantDraft{
                .textures = {narrow0.begin(), narrow0.end()},
                .storages = {narrow1.begin(), narrow1.end()},
                .samplers = {Sampler::border_black}}};
            if (const ErrorCode result = builder.dispatch("delta.7", 3, stage, mask_second);
                !succeeded(result)) {
                return result;
            }
            const std::array<VariantDraft, 1> mask_third{VariantDraft{
                .textures = {narrow1.begin(), narrow1.end()},
                .storages = {narrow0.begin(), narrow0.end()},
                .samplers = {Sampler::border_black}}};
            if (const ErrorCode result = builder.dispatch("delta.8", 3, stage, mask_third);
                !succeeded(result)) {
                return result;
            }

            VariantDraft mask_combine{
                .textures = {narrow0.begin(), narrow0.end()},
                .storages = {delta_image1},
                .samplers = {Sampler::border_black, Sampler::edge},
                .uniform_buffer = uniform_buffer};
            mask_combine.textures.push_back(previous1);
            const std::array<VariantDraft, 1> mask_combine_variants{std::move(mask_combine)};
            if (const ErrorCode result = builder.dispatch("delta.9", 3, stage, mask_combine_variants);
                !succeeded(result)) {
                return result;
            }

            delta_result0[refined_step] = delta_image0;
            delta_result1[refined_step] = delta_image1;
        }

        std::array<VariantDraft, 2> variants;
        for (std::size_t phase = 0; phase < variants.size(); ++phase) {
            variants[phase] = VariantDraft{
                .textures = {
                    history[1U - phase],
                    history[phase],
                    gamma_result[pyramid_levels - 1U],
                    delta_result0[refined_levels - 1U],
                    delta_result1[refined_levels - 1U],
                },
                .storages = {generated[frame]},
                .samplers = {Sampler::border_black, Sampler::edge},
                .uniform_buffer = uniform_buffer,
            };
        }
        if (const ErrorCode result = builder.dispatch("generate", 4, stage, variants);
            !succeeded(result)) {
            return result;
        }
    }

    return validate(out);
}

ErrorCode validate(const Graph& graph) noexcept {
    const std::uint32_t generated_frames = graph.config.generated_frames;
    if (generated_frames == 0 || generated_frames > max_generated_frames) {
        return ErrorCode::invalid_argument;
    }

    const auto image_count = static_cast<std::uint32_t>(graph.images.size());
    if (image_count <= history_image_count + generated_frames || graph.dispatches.empty()) {
        return ErrorCode::cache_integrity_failure;
    }

    for (std::uint32_t index = 0; index < image_count; ++index) {
        const auto role = static_cast<ImageRole>(graph.images[index].role);
        const bool is_history = index < history_image_count;
        const bool is_generated
            = !is_history && index < history_image_count + generated_frames;

        if (is_history != (role == ImageRole::history)) {
            return ErrorCode::cache_integrity_failure;
        }
        if (is_generated != (role == ImageRole::generated)) {
            return ErrorCode::cache_integrity_failure;
        }
        if (graph.images[index].base > static_cast<std::uint8_t>(ExtentBase::fixed)
            || graph.images[index].format > static_cast<std::uint8_t>(Format::rgba16f)) {
            return ErrorCode::cache_integrity_failure;
        }
    }

    const std::uint32_t generate_slot = slot_index("generate");
    if (generate_slot == invalid_slot) {
        return ErrorCode::invalid_state;
    }

    for (const DispatchEntry& dispatch : graph.dispatches) {
        if (dispatch.slot >= shaders::chain_slots().size() || dispatch.grid_image >= image_count) {
            return ErrorCode::cache_integrity_failure;
        }
        if (dispatch.variant_count == 0
            || dispatch.variant_first + dispatch.variant_count > graph.variants.size()) {
            return ErrorCode::cache_integrity_failure;
        }
        if (dispatch.stage > generated_frames) {
            return ErrorCode::cache_integrity_failure;
        }

        for (std::uint32_t offset = 0; offset < dispatch.variant_count; ++offset) {
            const VariantEntry& variant = graph.variants[dispatch.variant_first + offset];
            const std::uint32_t count
                = static_cast<std::uint32_t>(variant.texture_count) + variant.storage_count;
            if (variant.binding_first + count > graph.bindings.size()) {
                return ErrorCode::cache_integrity_failure;
            }
            if (variant.sampler_count > max_dispatch_samplers) {
                return ErrorCode::cache_integrity_failure;
            }
            if (variant.uniform_buffer != no_uniform_buffer
                && variant.uniform_buffer >= graph.uniform_buffer_count) {
                return ErrorCode::cache_integrity_failure;
            }

            for (std::uint32_t index = 0; index < count; ++index) {
                const std::uint32_t image = graph.bindings[variant.binding_first + index];
                if (image >= image_count) {
                    return ErrorCode::cache_integrity_failure;
                }

                const auto role = static_cast<ImageRole>(graph.images[image].role);
                const bool written = index >= variant.texture_count;

                if (written && role == ImageRole::history) {
                    return ErrorCode::presentation_sequence_invalid;
                }
                if (!written && role == ImageRole::generated) {
                    return ErrorCode::presentation_sequence_invalid;
                }
                if (written && role == ImageRole::generated && dispatch.slot != generate_slot) {
                    return ErrorCode::presentation_sequence_invalid;
                }
            }
        }
    }

    return ErrorCode::ok;
}

Extent flow_extent(const Config& config, const Extent output) noexcept {
    if (config.flow_denominator == 0) {
        return output;
    }
    return Extent{
        .width = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(output.width) * config.flow_numerator)
            / config.flow_denominator),
        .height = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(output.height) * config.flow_numerator)
            / config.flow_denominator),
    };
}

Extent evaluate(const ImageDesc& desc, const Extent output, const Extent flow) noexcept {
    Extent extent{};
    switch (static_cast<ExtentBase>(desc.base)) {
    case ExtentBase::output:
        extent = output;
        break;
    case ExtentBase::flow:
        extent = flow;
        break;
    case ExtentBase::fixed:
        return Extent{.width = desc.fixed_width, .height = desc.fixed_height};
    }

    const auto shift_down = [](const std::uint32_t value, const std::uint32_t amount) {
        return amount >= 32U ? 0U : value >> amount;
    };

    extent.width = shift_down(extent.width, desc.shift_before);
    extent.height = shift_down(extent.height, desc.shift_before);
    for (std::uint8_t step = 0; step < desc.ceil_halvings; ++step) {
        extent.width = (extent.width + 1U) >> 1U;
        extent.height = (extent.height + 1U) >> 1U;
    }
    extent.width = shift_down(extent.width, desc.shift_after);
    extent.height = shift_down(extent.height, desc.shift_after);
    return extent;
}

std::uint32_t bytes_per_pixel(const Format format) noexcept {
    switch (format) {
    case Format::rgba8:
        return 4;
    case Format::r8:
        return 1;
    case Format::rgba16f:
        return 8;
    }
    return 0;
}

std::uint64_t owned_memory_bytes(const Graph& graph, const Extent output) noexcept {
    const Extent flow = flow_extent(graph.config, output);

    std::uint64_t total = 0;
    for (const ImageDesc& desc : graph.images) {
        const auto role = static_cast<ImageRole>(desc.role);
        if (role == ImageRole::history || role == ImageRole::generated) {
            continue;
        }

        const Extent extent = evaluate(desc, output, flow);
        total += static_cast<std::uint64_t>(extent.width) * extent.height
            * bytes_per_pixel(static_cast<Format>(desc.format));
    }
    return total;
}

std::uint32_t block_index_of(const Graph& graph, const DispatchEntry& dispatch) noexcept {
    const std::span<const shaders::ChainSlot> slots = shaders::chain_slots();
    if (dispatch.slot >= slots.size()) {
        return invalid_block_index;
    }

    const shaders::ChainSlot& slot = slots[dispatch.slot];
    return slot.block_index
        + ((graph.config.performance && slot.has_performance_variant) ? shaders::performance_offset
                                                                     : 0U);
}

ErrorCode check_against_modules(
    const Graph& graph,
    const std::span<const ModuleInterface> modules) {
    for (const DispatchEntry& dispatch : graph.dispatches) {
        const std::uint32_t block_index = block_index_of(graph, dispatch);
        if (block_index == invalid_block_index) {
            return ErrorCode::cache_integrity_failure;
        }

        const auto module = std::ranges::find_if(modules, [block_index](const ModuleInterface& entry) {
            return entry.block_index == block_index;
        });
        if (module == modules.end()) {
            return ErrorCode::shader_set_unknown;
        }

        for (std::uint32_t offset = 0; offset < dispatch.variant_count; ++offset) {
            const VariantEntry& variant = graph.variants[dispatch.variant_first + offset];
            const std::uint32_t uniform_buffers = variant.uniform_buffer == no_uniform_buffer ? 0U : 1U;

            if (variant.texture_count != module->textures
                || variant.storage_count != module->storage_images
                || variant.sampler_count != module->samplers
                || uniform_buffers != module->uniform_buffers) {
                return ErrorCode::shader_interface_mismatch;
            }
        }
    }

    return ErrorCode::ok;
}

ConstantBuffer constant_buffer(const std::uint32_t index, const Config& config) noexcept {
    const std::uint32_t total = index == 0 ? 1U : config.generated_frames;
    const std::uint32_t position = index == 0 ? 0U : index - 1U;

    return ConstantBuffer{
        .resolution_inverse_scale = config.flow_numerator == 0
            ? 1.0F
            : static_cast<float>(config.flow_denominator) / static_cast<float>(config.flow_numerator),
        .timestamp = static_cast<float>(position + 1U) / static_cast<float>(total + 1U),
        .ui_threshold = 0.5F,
    };
}

std::string_view format_name(const Format format) noexcept {
    switch (format) {
    case Format::rgba8:
        return "rgba8";
    case Format::r8:
        return "r8";
    case Format::rgba16f:
        return "rgba16f";
    }
    return "unknown";
}

std::string_view role_name(const ImageRole role) noexcept {
    switch (role) {
    case ImageRole::internal:
        return "internal";
    case ImageRole::history:
        return "history";
    case ImageRole::generated:
        return "generated";
    case ImageRole::constant:
        return "constant";
    }
    return "unknown";
}

std::string_view sampler_name(const Sampler sampler) noexcept {
    switch (sampler) {
    case Sampler::border_black:
        return "border-black";
    case Sampler::border_white:
        return "border-white";
    case Sampler::edge:
        return "edge";
    }
    return "unknown";
}

}
