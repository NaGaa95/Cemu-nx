// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0-or-later

#include <lsfg/common/sha256.hpp>

#include <algorithm>
#include <bit>

namespace lsfg {
namespace {

constexpr std::array<std::uint32_t, 64> round_constants{
    0x428a'2f98U, 0x7137'4491U, 0xb5c0'fbcfU, 0xe9b5'dba5U, 0x3956'c25bU, 0x59f1'11f1U,
    0x923f'82a4U, 0xab1c'5ed5U, 0xd807'aa98U, 0x1283'5b01U, 0x2431'85beU, 0x550c'7dc3U,
    0x72be'5d74U, 0x80de'b1feU, 0x9bdc'06a7U, 0xc19b'f174U, 0xe49b'69c1U, 0xefbe'4786U,
    0x0fc1'9dc6U, 0x240c'a1ccU, 0x2de9'2c6fU, 0x4a74'84aaU, 0x5cb0'a9dcU, 0x76f9'88daU,
    0x983e'5152U, 0xa831'c66dU, 0xb003'27c8U, 0xbf59'7fc7U, 0xc6e0'0bf3U, 0xd5a7'9147U,
    0x06ca'6351U, 0x1429'2967U, 0x27b7'0a85U, 0x2e1b'2138U, 0x4d2c'6dfcU, 0x5338'0d13U,
    0x650a'7354U, 0x766a'0abbU, 0x81c2'c92eU, 0x9272'2c85U, 0xa2bf'e8a1U, 0xa81a'664bU,
    0xc24b'8b70U, 0xc76c'51a3U, 0xd192'e819U, 0xd699'0624U, 0xf40e'3585U, 0x106a'a070U,
    0x19a4'c116U, 0x1e37'6c08U, 0x2748'774cU, 0x34b0'bcb5U, 0x391c'0cb3U, 0x4ed8'aa4aU,
    0x5b9c'ca4fU, 0x682e'6ff3U, 0x748f'82eeU, 0x78a5'636fU, 0x84c8'7814U, 0x8cc7'0208U,
    0x90be'fffaU, 0xa450'6cebU, 0xbef9'a3f7U, 0xc671'78f2U};

[[nodiscard]] constexpr std::uint32_t load_be32(const std::uint8_t* bytes) noexcept {
    return (static_cast<std::uint32_t>(bytes[0]) << 24U) | (static_cast<std::uint32_t>(bytes[1]) << 16U)
         | (static_cast<std::uint32_t>(bytes[2]) << 8U) | static_cast<std::uint32_t>(bytes[3]);
}

[[nodiscard]] constexpr std::uint32_t choose(
    const std::uint32_t x,
    const std::uint32_t y,
    const std::uint32_t z) noexcept {
    return (x & y) ^ (~x & z);
}

[[nodiscard]] constexpr std::uint32_t majority(
    const std::uint32_t x,
    const std::uint32_t y,
    const std::uint32_t z) noexcept {
    return (x & y) ^ (x & z) ^ (y & z);
}

}

void Sha256::compress(const std::uint8_t* const block) noexcept {
    std::array<std::uint32_t, 64> schedule{};
    for (std::size_t index = 0; index < 16; ++index) {
        schedule[index] = load_be32(block + (index * 4U));
    }
    for (std::size_t index = 16; index < schedule.size(); ++index) {
        const std::uint32_t previous = schedule[index - 15U];
        const std::uint32_t recent = schedule[index - 2U];
        const std::uint32_t s0 = std::rotr(previous, 7) ^ std::rotr(previous, 18) ^ (previous >> 3U);
        const std::uint32_t s1 = std::rotr(recent, 17) ^ std::rotr(recent, 19) ^ (recent >> 10U);
        schedule[index] = schedule[index - 16U] + s0 + schedule[index - 7U] + s1;
    }

    std::array<std::uint32_t, 8> working = state_;
    for (std::size_t index = 0; index < schedule.size(); ++index) {
        const std::uint32_t s1
            = std::rotr(working[4], 6) ^ std::rotr(working[4], 11) ^ std::rotr(working[4], 25);
        const std::uint32_t temp1 = working[7] + s1 + choose(working[4], working[5], working[6])
                                  + round_constants[index] + schedule[index];
        const std::uint32_t s0
            = std::rotr(working[0], 2) ^ std::rotr(working[0], 13) ^ std::rotr(working[0], 22);
        const std::uint32_t temp2 = s0 + majority(working[0], working[1], working[2]);

        working[7] = working[6];
        working[6] = working[5];
        working[5] = working[4];
        working[4] = working[3] + temp1;
        working[3] = working[2];
        working[2] = working[1];
        working[1] = working[0];
        working[0] = temp1 + temp2;
    }

    for (std::size_t index = 0; index < state_.size(); ++index) {
        state_[index] += working[index];
    }
}

void Sha256::update(const std::span<const std::uint8_t> data) noexcept {
    if (finished_) {
        return;
    }

    length_ += data.size();
    std::size_t consumed = 0;

    if (buffered_ != 0) {
        const std::size_t take = std::min(sha256_block_size - buffered_, data.size());
        std::copy_n(data.begin(), take, buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_));
        buffered_ += take;
        consumed = take;

        if (buffered_ < sha256_block_size) {
            return;
        }
        compress(buffer_.data());
        buffered_ = 0;
    }

    while (data.size() - consumed >= sha256_block_size) {
        compress(data.data() + consumed);
        consumed += sha256_block_size;
    }

    const std::size_t remainder = data.size() - consumed;
    if (remainder != 0) {
        std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(consumed), remainder, buffer_.begin());
        buffered_ = remainder;
    }
}

void Sha256::update(const std::string_view text) noexcept {
    update(std::span{reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

void Sha256::update_field(const std::span<const std::uint8_t> data) noexcept {
    update_field(static_cast<std::uint64_t>(data.size()));
    update(data);
}

void Sha256::update_field(const std::string_view text) noexcept {
    update_field(std::span{reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

void Sha256::update_field(const std::uint64_t value) noexcept {
    std::array<std::uint8_t, 8> encoded{};
    for (std::size_t index = 0; index < encoded.size(); ++index) {
        encoded[index] = static_cast<std::uint8_t>((value >> (8U * (7U - index))) & 0xffU);
    }
    update(std::span<const std::uint8_t>{encoded});
}

Digest Sha256::finish() noexcept {
    if (!finished_) {
        const std::uint64_t bit_length = length_ * 8U;

        constexpr std::array<std::uint8_t, 1> padding_start{0x80};
        update(std::span<const std::uint8_t>{padding_start});

        constexpr std::array<std::uint8_t, 1> padding_zero{0x00};
        while (buffered_ != sha256_block_size - 8U) {
            update(std::span<const std::uint8_t>{padding_zero});
        }

        std::array<std::uint8_t, 8> encoded_length{};
        for (std::size_t index = 0; index < encoded_length.size(); ++index) {
            encoded_length[index] = static_cast<std::uint8_t>((bit_length >> (8U * (7U - index))) & 0xffU);
        }
        std::copy_n(
            encoded_length.begin(),
            encoded_length.size(),
            buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_));
        compress(buffer_.data());
        buffered_ = 0;
        finished_ = true;
    }

    Digest digest{};
    for (std::size_t index = 0; index < state_.size(); ++index) {
        const std::uint32_t word = state_[index];
        digest[(index * 4U) + 0U] = static_cast<std::uint8_t>((word >> 24U) & 0xffU);
        digest[(index * 4U) + 1U] = static_cast<std::uint8_t>((word >> 16U) & 0xffU);
        digest[(index * 4U) + 2U] = static_cast<std::uint8_t>((word >> 8U) & 0xffU);
        digest[(index * 4U) + 3U] = static_cast<std::uint8_t>(word & 0xffU);
    }
    return digest;
}

Digest sha256(const std::span<const std::uint8_t> data) noexcept {
    Sha256 hasher;
    hasher.update(data);
    return hasher.finish();
}

DigestHex to_hex(const Digest& digest) noexcept {
    constexpr std::string_view alphabet{"0123456789abcdef"};

    DigestHex text{};
    for (std::size_t index = 0; index < digest.size(); ++index) {
        text[index * 2U] = alphabet[(digest[index] >> 4U) & 0x0fU];
        text[(index * 2U) + 1U] = alphabet[digest[index] & 0x0fU];
    }
    text[digest_hex_size] = '\0';
    return text;
}

}
