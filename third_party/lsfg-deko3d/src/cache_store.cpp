// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <lsfg/common/cache_store.hpp>

#include <lsfg/common/sha256.hpp>

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace lsfg::cache {
namespace {

template <typename T>
void append(std::vector<std::uint8_t>& out, const std::span<const T> values) {
    const auto* const bytes = reinterpret_cast<const std::uint8_t*>(values.data());
    out.insert(out.end(), bytes, bytes + (values.size() * sizeof(T)));
}

template <typename T>
[[nodiscard]] bool take(
    const std::span<const std::uint8_t> payload,
    std::size_t& offset,
    const std::size_t count,
    std::vector<T>& out) {
    const std::size_t bytes = count * sizeof(T);
    if (offset + bytes > payload.size()) {
        return false;
    }

    out.resize(count);
    if (count != 0) {
        std::memcpy(out.data(), payload.data() + offset, bytes);
    }
    offset += bytes;
    return true;
}

[[nodiscard]] std::string join(const std::string_view directory, const std::string_view name) {
    std::string path{directory};
    if (!path.empty() && path.back() != '/') {
        path.push_back('/');
    }
    path.append(name);
    return path;
}

void make_directories(const std::string_view directory) {
    std::string path;
    path.reserve(directory.size());

    for (std::size_t index = 0; index <= directory.size(); ++index) {
        const bool end = index == directory.size();
        if (!end && directory[index] != '/') {
            path.push_back(directory[index]);
            continue;
        }
        if (path.empty() || path.back() == ':') {
            path.push_back('/');
            continue;
        }

        static_cast<void>(::mkdir(path.c_str(), 0777));
        if (!end) {
            path.push_back('/');
        }
    }
}

[[nodiscard]] ErrorCode write_file(const std::string& path, const std::span<const std::uint8_t> bytes) {
    std::FILE* const file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        return ErrorCode::io_error;
    }

    const std::size_t written = bytes.empty() ? 0 : std::fwrite(bytes.data(), 1, bytes.size(), file);
    const bool flushed = std::fflush(file) == 0;
    if (flushed) {
        static_cast<void>(::fsync(::fileno(file)));
    }

    const bool closed = std::fclose(file) == 0;
    if (written != bytes.size() || !flushed || !closed) {
        static_cast<void>(std::remove(path.c_str()));
        return ErrorCode::io_error;
    }
    return ErrorCode::ok;
}

[[nodiscard]] ErrorCode read_file(const std::string& path, std::vector<std::uint8_t>& out) {
    out.clear();

    std::FILE* const file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return ErrorCode::cache_missing;
    }

    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size <= 0) {
        std::fclose(file);
        return ErrorCode::cache_integrity_failure;
    }

    out.resize(static_cast<std::size_t>(size));
    const std::size_t read = std::fread(out.data(), 1, out.size(), file);
    std::fclose(file);

    if (read != out.size()) {
        out.clear();
        return ErrorCode::io_error;
    }
    return ErrorCode::ok;
}

}

std::string directory_for(const std::string_view root, const Digest& key) {
    return join(root, to_hex(key).data());
}

std::string module_name(const std::uint32_t index) {
    std::array<char, 16> name{};
    const int written = std::snprintf(name.data(), name.size(), "%02u.dksh", index);
    if (written <= 0) {
        return {};
    }
    return std::string{name.data()};
}

ErrorCode write(const std::string_view directory, Contents& contents) {
    if (contents.passes.empty() || contents.passes.size() > max_passes) {
        return ErrorCode::invalid_argument;
    }

    std::vector<PassEntry> passes;
    std::vector<SlotEntry> slots;
    passes.reserve(contents.passes.size());

    for (PassInput& pass : contents.passes) {
        if (pass.dksh.empty() || pass.slots.empty()) {
            return ErrorCode::invalid_argument;
        }

        pass.entry.dksh_size = static_cast<std::uint32_t>(pass.dksh.size());
        pass.entry.dksh_hash = sha256(pass.dksh);
        pass.entry.slot_first = static_cast<std::uint32_t>(slots.size());
        pass.entry.slot_count = static_cast<std::uint32_t>(pass.slots.size());

        slots.insert(slots.end(), pass.slots.begin(), pass.slots.end());
        passes.push_back(pass.entry);
    }

    contents.header.magic = manifest_magic;
    contents.header.abi_version = abi_version;
    contents.header.extractor_version = extractor_version;
    contents.header.pass_count = static_cast<std::uint32_t>(passes.size());
    contents.header.slot_count = static_cast<std::uint32_t>(slots.size());
    describe(contents.header, contents.graph);

    if (const ErrorCode result = validate(contents.header, passes, slots, contents.graph);
        !succeeded(result)) {
        return result;
    }

    std::vector<std::uint8_t> payload;
    append<PassEntry>(payload, passes);
    append<SlotEntry>(payload, slots);
    append<graph::ImageDesc>(payload, contents.graph.images);
    append<graph::DispatchEntry>(payload, contents.graph.dispatches);
    append<graph::VariantEntry>(payload, contents.graph.variants);
    append<std::uint32_t>(payload, contents.graph.bindings);

    contents.header.payload_crc32 = crc32(payload);

    make_directories(directory);

    const std::string manifest_path = join(directory, manifest_name);
    static_cast<void>(std::remove(manifest_path.c_str()));

    for (std::uint32_t index = 0; index < contents.passes.size(); ++index) {
        const std::string path = join(directory, module_name(index));
        if (const ErrorCode result = write_file(path, contents.passes[index].dksh);
            !succeeded(result)) {
            return result;
        }
    }

    std::vector<std::uint8_t> manifest;
    manifest.reserve(sizeof(ManifestHeader) + payload.size());
    const auto* const header_bytes = reinterpret_cast<const std::uint8_t*>(&contents.header);
    manifest.insert(manifest.end(), header_bytes, header_bytes + sizeof(ManifestHeader));
    manifest.insert(manifest.end(), payload.begin(), payload.end());

    const std::string temporary_path = join(directory, manifest_temporary_name);
    if (const ErrorCode result = write_file(temporary_path, manifest); !succeeded(result)) {
        return result;
    }
    if (std::rename(temporary_path.c_str(), manifest_path.c_str()) != 0) {
        static_cast<void>(std::remove(temporary_path.c_str()));
        return ErrorCode::io_error;
    }

    return ErrorCode::ok;
}

ErrorCode read(const std::string_view directory, Loaded& out) {
    out = Loaded{};

    std::vector<std::uint8_t> manifest;
    if (const ErrorCode result = read_file(join(directory, manifest_name), manifest);
        !succeeded(result)) {
        return result;
    }
    if (manifest.size() < sizeof(ManifestHeader)) {
        return ErrorCode::cache_integrity_failure;
    }

    std::memcpy(&out.header, manifest.data(), sizeof(ManifestHeader));
    if (const ErrorCode result = validate(out.header); !succeeded(result)) {
        return result;
    }

    const std::span<const std::uint8_t> payload{
        manifest.data() + sizeof(ManifestHeader), manifest.size() - sizeof(ManifestHeader)};
    if (crc32(payload) != out.header.payload_crc32) {
        return ErrorCode::cache_integrity_failure;
    }

    std::vector<PassEntry> passes;
    std::vector<SlotEntry> slots;
    std::size_t offset = 0;
    if (!take(payload, offset, out.header.pass_count, passes)
        || !take(payload, offset, out.header.slot_count, slots)
        || !take(payload, offset, out.header.image_count, out.graph.images)
        || !take(payload, offset, out.header.dispatch_count, out.graph.dispatches)
        || !take(payload, offset, out.header.variant_count, out.graph.variants)
        || !take(payload, offset, out.header.binding_count, out.graph.bindings)) {
        return ErrorCode::cache_integrity_failure;
    }
    if (offset != payload.size()) {
        return ErrorCode::cache_integrity_failure;
    }

    out.graph.config = configuration(out.header);
    out.graph.uniform_buffer_count = out.header.uniform_buffer_count;

    if (const ErrorCode result = validate(out.header, passes, slots, out.graph); !succeeded(result)) {
        return result;
    }

    out.passes.reserve(passes.size());
    for (std::uint32_t index = 0; index < passes.size(); ++index) {
        const PassEntry& entry = passes[index];

        LoadedPass pass;
        pass.entry = entry;
        pass.slots.assign(
            slots.begin() + entry.slot_first,
            slots.begin() + entry.slot_first + entry.slot_count);

        if (const ErrorCode result = read_file(join(directory, module_name(index)), pass.dksh);
            !succeeded(result)) {
            return result;
        }
        if (pass.dksh.size() != entry.dksh_size || sha256(pass.dksh) != entry.dksh_hash) {
            return ErrorCode::cache_integrity_failure;
        }

        out.passes.push_back(std::move(pass));
    }

    return ErrorCode::ok;
}

Comparison compare(const Loaded& left, const Contents& right) noexcept {
    Comparison result;
    result.modules = static_cast<std::uint32_t>(right.passes.size());
    result.same_shape = left.passes.size() == right.passes.size();

    const std::size_t count = std::min(left.passes.size(), right.passes.size());
    for (std::size_t index = 0; index < count; ++index) {
        const std::vector<std::uint8_t>& cached = left.passes[index].dksh;
        const std::span<const std::uint8_t> fresh = right.passes[index].dksh;

        if (left.passes[index].entry.resource_id != right.passes[index].entry.resource_id
            || cached.size() != fresh.size()) {
            result.same_shape = false;
            ++result.differing_modules;
            continue;
        }

        std::uint32_t differing = 0;
        for (std::size_t offset = 0; offset < cached.size(); ++offset) {
            differing += cached[offset] != fresh[offset] ? 1U : 0U;
        }
        if (differing != 0) {
            ++result.differing_modules;
            result.differing_bytes += differing;
        }
    }

    return result;
}

bool same_modules(const Loaded& left, const Contents& right) noexcept {
    return compare(left, right).identical();
}

}
