// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0-or-later

#include <lsfg/backend/layout.hpp>

#include <cstddef>
#include <vector>

namespace lsfg::backend {
namespace {

struct Usage {
    bool sampled{};
    bool storage{};
};

}

ErrorCode describe(const graph::Graph& graph, DescriptorLayout& out) {
    out = DescriptorLayout{};

    std::vector<Usage> usage(graph.images.size());

    for (const graph::VariantEntry& variant : graph.variants) {
        const std::size_t first = variant.binding_first;
        const std::size_t count = static_cast<std::size_t>(variant.texture_count)
            + variant.storage_count;
        if (first > graph.bindings.size() || count > graph.bindings.size() - first) {
            return ErrorCode::cache_integrity_failure;
        }

        for (std::size_t index = 0; index < count; ++index) {
            const std::uint32_t image = graph.bindings[first + index];
            if (image >= usage.size()) {
                return ErrorCode::cache_integrity_failure;
            }

            if (index < variant.texture_count) {
                usage[image].sampled = true;
            } else {
                usage[image].storage = true;
            }
        }
    }

    out.images.assign(graph.images.size(), ImageDescriptors{});

    for (std::size_t index = 0; index < usage.size(); ++index) {
        if (usage[index].sampled) {
            out.images[index].sampled = out.image_descriptors++;
            ++out.sampled_images;
        }
        if (usage[index].storage) {
            out.images[index].storage = out.image_descriptors++;
            ++out.storage_images;
        }
    }

    return ErrorCode::ok;
}

std::uint32_t Arena::place(const std::uint64_t size, const std::uint32_t alignment) noexcept {
    if (alignment == 0 || (alignment & (alignment - 1U)) != 0) {
        overflowed_ = true;
        return 0;
    }

    const std::uint64_t mask = std::uint64_t{alignment} - 1U;
    const std::uint64_t offset = (used_ + mask) & ~mask;
    if (offset < used_ || size > max_memory_block_bytes - offset) {
        overflowed_ = true;
        return 0;
    }

    used_ = offset + size;
    return static_cast<std::uint32_t>(offset);
}

std::uint32_t Arena::block_size() const noexcept {
    if (overflowed_ || used_ == 0) {
        return 0;
    }

    const std::uint64_t mask = memory_block_alignment - 1U;
    return static_cast<std::uint32_t>((used_ + mask) & ~mask);
}

}
