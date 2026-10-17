// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace lsfg {

inline constexpr std::size_t sha256_digest_size = 32;
inline constexpr std::size_t sha256_block_size = 64;

using Digest = std::array<std::uint8_t, sha256_digest_size>;

class Sha256 final {
public:
    void update(std::span<const std::uint8_t> data) noexcept;
    void update(std::string_view text) noexcept;

    void update_field(std::span<const std::uint8_t> data) noexcept;
    void update_field(std::string_view text) noexcept;
    void update_field(std::uint64_t value) noexcept;

    [[nodiscard]] Digest finish() noexcept;

private:
    void compress(const std::uint8_t* block) noexcept;

    std::array<std::uint32_t, 8> state_{
        0x6a09'e667U,
        0xbb67'ae85U,
        0x3c6e'f372U,
        0xa54f'f53aU,
        0x510e'527fU,
        0x9b05'688cU,
        0x1f83'd9abU,
        0x5be0'cd19U};
    std::array<std::uint8_t, sha256_block_size> buffer_{};
    std::uint64_t length_{};
    std::size_t buffered_{};
    bool finished_{};
};

[[nodiscard]] Digest sha256(std::span<const std::uint8_t> data) noexcept;

inline constexpr std::size_t digest_hex_size = 2 * sha256_digest_size;

using DigestHex = std::array<char, digest_hex_size + 1>;

[[nodiscard]] DigestHex to_hex(const Digest& digest) noexcept;

}
