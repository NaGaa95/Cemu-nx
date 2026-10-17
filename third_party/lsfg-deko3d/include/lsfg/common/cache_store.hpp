// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lsfg/common/cache_format.hpp>
#include <lsfg/common/error.hpp>
#include <lsfg/common/image_graph.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace lsfg::cache {

inline constexpr std::string_view manifest_name = "manifest.bin";
inline constexpr std::string_view manifest_temporary_name = "manifest.new";

struct PassInput {

    PassEntry entry;
    std::vector<SlotEntry> slots;
    std::span<const std::uint8_t> dksh;
};

struct Contents {
    ManifestHeader header;
    std::vector<PassInput> passes;
    graph::Graph graph;
};

struct LoadedPass {
    PassEntry entry;
    std::vector<SlotEntry> slots;
    std::vector<std::uint8_t> dksh;
};

struct Loaded {
    ManifestHeader header;
    std::vector<LoadedPass> passes;
    graph::Graph graph;
};

[[nodiscard]] std::string directory_for(std::string_view root, const Digest& key);

[[nodiscard]] std::string module_name(std::uint32_t index);

[[nodiscard]] ErrorCode write(std::string_view directory, Contents& contents);

[[nodiscard]] ErrorCode read(std::string_view directory, Loaded& out);

struct Comparison {
    std::uint32_t modules{};
    std::uint32_t differing_modules{};
    std::uint32_t differing_bytes{};
    bool same_shape{};

    [[nodiscard]] bool identical() const noexcept {
        return same_shape && differing_modules == 0;
    }
};

[[nodiscard]] Comparison compare(const Loaded& left, const Contents& right) noexcept;

[[nodiscard]] bool same_modules(const Loaded& left, const Contents& right) noexcept;

}
