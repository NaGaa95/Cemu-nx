// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0-or-later

#include <lsfg/common/pe_resources.hpp>

#include <algorithm>
#include <optional>

namespace lsfg::pe {
namespace {

constexpr std::uint16_t dos_magic = 0x5a4d;
constexpr std::uint32_t pe_signature = 0x0000'4550;
constexpr std::uint16_t optional_magic_pe32 = 0x010b;
constexpr std::uint16_t optional_magic_pe32_plus = 0x020b;

constexpr std::size_t dos_lfanew_offset = 0x3c;
constexpr std::size_t coff_header_size = 20;
constexpr std::size_t section_header_size = 40;
constexpr std::size_t directory_header_size = 16;
constexpr std::size_t directory_entry_size = 8;
constexpr std::size_t data_entry_size = 16;
constexpr std::size_t resource_directory_index = 2;

constexpr std::uint32_t subdirectory_flag = 0x8000'0000U;
constexpr std::uint32_t offset_mask = 0x7fff'ffffU;

struct Section {
    std::uint32_t virtual_address{};
    std::uint32_t virtual_size{};
    std::uint32_t raw_offset{};
    std::uint32_t raw_size{};
};

[[nodiscard]] std::optional<std::uint16_t> read_u16(
    const std::span<const std::uint8_t> image,
    const std::size_t offset) noexcept {
    if (offset + 2U > image.size()) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(image[offset]) | static_cast<std::uint16_t>(image[offset + 1U] << 8U));
}

[[nodiscard]] std::optional<std::uint32_t> read_u32(
    const std::span<const std::uint8_t> image,
    const std::size_t offset) noexcept {
    if (offset + 4U > image.size()) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(image[offset]) | (static_cast<std::uint32_t>(image[offset + 1U]) << 8U)
         | (static_cast<std::uint32_t>(image[offset + 2U]) << 16U)
         | (static_cast<std::uint32_t>(image[offset + 3U]) << 24U);
}

struct Walk {
    std::span<const std::uint8_t> image;
    std::size_t section_offset{};
    std::size_t section_size{};
    std::uint32_t section_rva{};
    ResourceTable* out{};
};

[[nodiscard]] ErrorCode walk_directory(
    Walk& walk,
    std::uint32_t relative_offset,
    std::uint32_t depth,
    std::uint32_t type,
    std::uint32_t id);

[[nodiscard]] ErrorCode walk_data_entry(
    Walk& walk,
    const std::uint32_t relative_offset,
    const std::uint32_t type,
    const std::uint32_t id,
    const std::uint32_t language) {
    if (relative_offset + data_entry_size > walk.section_size) {
        return ErrorCode::io_error;
    }

    const std::size_t offset = walk.section_offset + relative_offset;
    const std::optional<std::uint32_t> data_rva = read_u32(walk.image, offset);
    const std::optional<std::uint32_t> size = read_u32(walk.image, offset + 4U);
    if (!data_rva || !size) {
        return ErrorCode::io_error;
    }

    if (*data_rva < walk.section_rva) {
        return ErrorCode::io_error;
    }

    const std::uint64_t within_section = static_cast<std::uint64_t>(*data_rva) - walk.section_rva;
    const std::uint64_t end = within_section + *size;
    if (end > walk.section_size) {
        return ErrorCode::io_error;
    }

    if (walk.out->resources.size() >= max_resources) {
        return ErrorCode::unsupported;
    }

    walk.out->resources.push_back(Resource{
        .type = type,
        .id = id,
        .language = language,
        .offset = static_cast<std::uint32_t>(walk.section_offset + within_section),
        .size = *size,
    });
    return ErrorCode::ok;
}

ErrorCode walk_directory(
    Walk& walk,
    const std::uint32_t relative_offset,
    const std::uint32_t depth,
    const std::uint32_t type,
    const std::uint32_t id) {
    if (depth >= max_directory_depth) {
        return ErrorCode::unsupported;
    }
    if (relative_offset + directory_header_size > walk.section_size) {
        return ErrorCode::io_error;
    }

    const std::size_t header_offset = walk.section_offset + relative_offset;
    const std::optional<std::uint16_t> named_count = read_u16(walk.image, header_offset + 12U);
    const std::optional<std::uint16_t> id_count = read_u16(walk.image, header_offset + 14U);
    if (!named_count || !id_count) {
        return ErrorCode::io_error;
    }

    const std::size_t entry_count = static_cast<std::size_t>(*named_count) + *id_count;
    const std::size_t entries_offset = relative_offset + directory_header_size;
    if (entries_offset + (entry_count * directory_entry_size) > walk.section_size) {
        return ErrorCode::io_error;
    }

    for (std::size_t index = 0; index < entry_count; ++index) {
        const std::size_t entry_offset
            = walk.section_offset + entries_offset + (index * directory_entry_size);
        const std::optional<std::uint32_t> name = read_u32(walk.image, entry_offset);
        const std::optional<std::uint32_t> child = read_u32(walk.image, entry_offset + 4U);
        if (!name || !child) {
            return ErrorCode::io_error;
        }

        const bool is_named = index < *named_count;
        if (is_named) {
            ++walk.out->named_entries_skipped;
            continue;
        }

        const bool is_directory = (*child & subdirectory_flag) != 0U;
        const std::uint32_t child_offset = *child & offset_mask;

        ErrorCode result = ErrorCode::ok;
        switch (depth) {
        case 0:
            result = is_directory ? walk_directory(walk, child_offset, depth + 1U, *name, 0)
                                  : ErrorCode::io_error;
            break;
        case 1:
            result = is_directory ? walk_directory(walk, child_offset, depth + 1U, type, *name)
                                  : ErrorCode::io_error;
            break;
        default:
            result = is_directory ? ErrorCode::unsupported
                                  : walk_data_entry(walk, child_offset, type, id, *name);
            break;
        }

        if (!succeeded(result)) {
            return result;
        }
    }

    return ErrorCode::ok;
}

}

ErrorCode enumerate_resources(const std::span<const std::uint8_t> image, ResourceTable& out) {
    out.resources.clear();
    out.named_entries_skipped = 0;

    const std::optional<std::uint16_t> magic = read_u16(image, 0);
    if (!magic || *magic != dos_magic) {
        return ErrorCode::unsupported;
    }

    const std::optional<std::uint32_t> pe_offset = read_u32(image, dos_lfanew_offset);
    if (!pe_offset) {
        return ErrorCode::io_error;
    }

    const std::optional<std::uint32_t> signature = read_u32(image, *pe_offset);
    if (!signature || *signature != pe_signature) {
        return ErrorCode::unsupported;
    }

    const std::size_t coff_offset = static_cast<std::size_t>(*pe_offset) + 4U;
    const std::optional<std::uint16_t> section_count = read_u16(image, coff_offset + 2U);
    const std::optional<std::uint16_t> optional_size = read_u16(image, coff_offset + 16U);
    if (!section_count || !optional_size) {
        return ErrorCode::io_error;
    }

    const std::size_t optional_offset = coff_offset + coff_header_size;
    const std::optional<std::uint16_t> optional_magic = read_u16(image, optional_offset);
    if (!optional_magic) {
        return ErrorCode::io_error;
    }

    std::size_t directory_count_offset = 0;
    std::size_t directories_offset = 0;
    switch (*optional_magic) {
    case optional_magic_pe32:
        directory_count_offset = optional_offset + 92U;
        directories_offset = optional_offset + 96U;
        break;
    case optional_magic_pe32_plus:
        directory_count_offset = optional_offset + 108U;
        directories_offset = optional_offset + 112U;
        break;
    default:
        return ErrorCode::unsupported;
    }

    const std::optional<std::uint32_t> directory_count = read_u32(image, directory_count_offset);
    if (!directory_count || *directory_count <= resource_directory_index) {
        return ErrorCode::unsupported;
    }

    const std::size_t resource_directory_offset
        = directories_offset + (resource_directory_index * 8U);
    const std::optional<std::uint32_t> resource_rva = read_u32(image, resource_directory_offset);
    const std::optional<std::uint32_t> resource_size = read_u32(image, resource_directory_offset + 4U);
    if (!resource_rva || !resource_size) {
        return ErrorCode::io_error;
    }
    if (*resource_rva == 0 || *resource_size == 0) {
        return ErrorCode::unsupported;
    }

    const std::size_t sections_offset = optional_offset + *optional_size;
    std::optional<Section> resource_section;
    for (std::size_t index = 0; index < *section_count; ++index) {
        const std::size_t header = sections_offset + (index * section_header_size);
        const std::optional<std::uint32_t> virtual_size = read_u32(image, header + 8U);
        const std::optional<std::uint32_t> virtual_address = read_u32(image, header + 12U);
        const std::optional<std::uint32_t> raw_size = read_u32(image, header + 16U);
        const std::optional<std::uint32_t> raw_offset = read_u32(image, header + 20U);
        if (!virtual_size || !virtual_address || !raw_size || !raw_offset) {
            return ErrorCode::io_error;
        }

        const std::uint64_t section_end
            = static_cast<std::uint64_t>(*virtual_address) + std::max(*virtual_size, *raw_size);
        if (*resource_rva < *virtual_address || *resource_rva >= section_end) {
            continue;
        }

        resource_section = Section{
            .virtual_address = *virtual_address,
            .virtual_size = *virtual_size,
            .raw_offset = *raw_offset,
            .raw_size = *raw_size,
        };
        break;
    }

    if (!resource_section) {
        return ErrorCode::unsupported;
    }

    const std::uint64_t section_end
        = static_cast<std::uint64_t>(resource_section->raw_offset) + resource_section->raw_size;
    if (section_end > image.size()) {
        return ErrorCode::io_error;
    }

    if (*resource_rva < resource_section->virtual_address) {
        return ErrorCode::io_error;
    }
    const std::uint64_t directory_within_section
        = static_cast<std::uint64_t>(*resource_rva) - resource_section->virtual_address;
    if (directory_within_section >= resource_section->raw_size) {
        return ErrorCode::io_error;
    }

    Walk walk{
        .image = image,
        .section_offset = static_cast<std::size_t>(resource_section->raw_offset + directory_within_section),
        .section_size = static_cast<std::size_t>(resource_section->raw_size - directory_within_section),
        .section_rva = *resource_rva,
        .out = &out,
    };

    const ErrorCode result = walk_directory(walk, 0, 0, 0, 0);
    if (!succeeded(result)) {
        out.resources.clear();
        out.named_entries_skipped = 0;
    }
    return result;
}

std::span<const std::uint8_t> resource_data(
    const std::span<const std::uint8_t> image,
    const Resource& resource) noexcept {
    const std::uint64_t end = static_cast<std::uint64_t>(resource.offset) + resource.size;
    if (end > image.size()) {
        return {};
    }
    return image.subspan(resource.offset, resource.size);
}

}
