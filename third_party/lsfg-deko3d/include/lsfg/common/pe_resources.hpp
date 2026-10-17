// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lsfg/common/error.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace lsfg::pe {

inline constexpr std::uint32_t resource_type_rcdata = 10;

inline constexpr std::uint32_t max_directory_depth = 3;
inline constexpr std::size_t max_resources = 8192;

struct Resource {
    std::uint32_t type{};
    std::uint32_t id{};
    std::uint32_t language{};
    std::uint32_t offset{};
    std::uint32_t size{};
};

struct ResourceTable {
    std::vector<Resource> resources;

    std::uint32_t named_entries_skipped{};
};

[[nodiscard]] ErrorCode enumerate_resources(std::span<const std::uint8_t> image, ResourceTable& out);

[[nodiscard]] std::span<const std::uint8_t> resource_data(
    std::span<const std::uint8_t> image,
    const Resource& resource) noexcept;

}
