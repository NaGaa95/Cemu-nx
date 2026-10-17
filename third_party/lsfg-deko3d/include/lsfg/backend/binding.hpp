// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lsfg/backend/cache_load.hpp>
#include <lsfg/backend/layout.hpp>
#include <lsfg/common/cache_store.hpp>
#include <lsfg/common/error.hpp>

#include <array>
#include <cstdint>

namespace lsfg::backend {

inline constexpr std::uint32_t no_slot = 0xFFFF'FFFFU;

struct TextureBinding {
    std::uint32_t slot{};
    std::uint32_t image{};
    std::uint32_t descriptor{};
    std::uint32_t sampler{};
};

struct StorageBinding {
    std::uint32_t slot{};
    std::uint32_t image{};
    std::uint32_t descriptor{};
};

struct DispatchBinding {
    std::uint32_t pass{};
    std::uint32_t variant{};
    std::uint32_t groups_x{};
    std::uint32_t groups_y{};

    std::array<TextureBinding, max_texture_slots> textures{};
    std::array<StorageBinding, max_storage_slots> storages{};
    std::uint32_t texture_count{};
    std::uint32_t storage_count{};

    std::uint32_t uniform_slot{no_slot};
    std::uint32_t uniform_buffer{};
};

[[nodiscard]] ErrorCode bind(
    const cache::Loaded& cache,
    const Plan& plan,
    const DescriptorLayout& descriptors,
    std::uint32_t dispatch,
    std::uint32_t phase,
    DispatchBinding& out) noexcept;

}
