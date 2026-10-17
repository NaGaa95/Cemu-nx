// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0-or-later

#include <lsfg/common/spirv_module.hpp>

#include <algorithm>

namespace lsfg::spirv {
namespace {

constexpr std::uint32_t byte_swapped_magic = 0x0302'2307U;

constexpr std::uint32_t max_bound = 1U << 20U;

enum Op : std::uint16_t {
    op_entry_point = 15,
    op_execution_mode = 16,
    op_capability = 17,
    op_type_image = 25,
    op_type_sampler = 26,
    op_type_sampled_image = 27,
    op_type_array = 28,
    op_type_runtime_array = 29,
    op_type_struct = 30,
    op_type_pointer = 32,
    op_constant = 43,
    op_spec_constant_true = 48,
    op_spec_constant_false = 49,
    op_spec_constant = 50,
    op_spec_constant_composite = 51,
    op_spec_constant_op = 52,
    op_variable = 59,
    op_decorate = 71,
    op_execution_mode_id = 331,
};

enum Decoration : std::uint32_t {
    decoration_spec_id = 1,
    decoration_block = 2,
    decoration_buffer_block = 3,
    decoration_built_in = 11,
    decoration_non_writable = 24,
    decoration_non_readable = 25,
    decoration_binding = 33,
    decoration_descriptor_set = 34,
};

enum StorageClass : std::uint32_t {
    storage_class_uniform_constant = 0,
    storage_class_uniform = 2,
    storage_class_push_constant = 9,
    storage_class_storage_buffer = 12,
};

enum ExecutionMode : std::uint32_t {
    execution_mode_local_size = 17,
    execution_mode_local_size_id = 38,
};

constexpr std::uint32_t built_in_workgroup_size = 25;

[[nodiscard]] constexpr bool is_spec_constant(const std::uint16_t opcode) noexcept {
    return opcode == op_spec_constant_true || opcode == op_spec_constant_false
        || opcode == op_spec_constant || opcode == op_spec_constant_composite
        || opcode == op_spec_constant_op;
}

struct IdRecord {
    std::uint16_t opcode{};
    std::uint32_t operand0{};
    std::uint32_t operand1{};
    std::uint32_t operand2{};

    std::uint32_t set{};
    std::uint32_t binding{};
    std::uint32_t spec_id{};
    std::uint32_t constant{};

    bool has_set{};
    bool has_binding{};
    bool has_constant{};
    bool has_spec_id{};
    bool block{};
    bool buffer_block{};
    bool non_readable{};
    bool non_writable{};
    bool workgroup_size_built_in{};
};

[[nodiscard]] std::string decode_string(const std::span<const std::uint32_t> words) {
    std::string text;
    for (const std::uint32_t word : words) {
        for (std::uint32_t shift = 0; shift < 32U; shift += 8U) {
            const auto character = static_cast<char>((word >> shift) & 0xffU);
            if (character == '\0') {
                return text;
            }
            text.push_back(character);
        }
    }
    return text;
}

struct ResolvedType {
    std::uint32_t base_id{};
    std::uint32_t array_size{1};
    bool resolved{};
};

[[nodiscard]] ResolvedType resolve_array(
    const std::span<const IdRecord> ids,
    std::uint32_t type_id) {
    ResolvedType resolved{};

    for (std::uint32_t depth = 0; depth < 8U; ++depth) {
        if (type_id >= ids.size()) {
            return resolved;
        }

        const IdRecord& record = ids[type_id];
        if (record.opcode == op_type_array) {
            const std::uint32_t length_id = record.operand1;
            if (length_id < ids.size() && ids[length_id].has_constant) {
                resolved.array_size *= std::max(ids[length_id].constant, 1U);
            }
            type_id = record.operand0;
            continue;
        }
        if (record.opcode == op_type_runtime_array) {
            type_id = record.operand0;
            continue;
        }

        resolved.base_id = type_id;
        resolved.resolved = true;
        return resolved;
    }

    return resolved;
}

}

ErrorCode inspect(const std::span<const std::uint32_t> words, Inventory& out) {
    out = Inventory{};

    if (words.size() < header_words) {
        return ErrorCode::invalid_argument;
    }
    if (words[0] == byte_swapped_magic) {
        return ErrorCode::unsupported;
    }
    if (words[0] != magic) {
        return ErrorCode::invalid_argument;
    }

    const std::uint32_t version = words[1];
    out.version_major = (version >> 16U) & 0xffU;
    out.version_minor = (version >> 8U) & 0xffU;
    out.generator = words[2];
    out.bound = words[3];

    if (out.bound == 0 || out.bound > max_bound) {
        return ErrorCode::unsupported;
    }

    std::vector<IdRecord> ids(out.bound);
    std::vector<std::uint32_t> descriptor_variables;

    for (std::size_t offset = header_words; offset < words.size();) {
        const std::uint32_t instruction = words[offset];
        const auto word_count = static_cast<std::uint16_t>(instruction >> 16U);
        const auto opcode = static_cast<std::uint16_t>(instruction & 0xffffU);

        if (word_count == 0 || offset + word_count > words.size()) {
            return ErrorCode::invalid_argument;
        }

        const std::span<const std::uint32_t> operands = words.subspan(offset + 1U, word_count - 1U);

        const auto record_for = [&ids](const std::uint32_t id) -> IdRecord* {
            return id < ids.size() ? &ids[id] : nullptr;
        };

        switch (opcode) {
        case op_capability:
            if (!operands.empty()) {
                out.capabilities.push_back(operands[0]);
            }
            break;

        case op_entry_point:
            if (operands.size() >= 3) {
                out.execution_model = operands[0];
                out.entry_point = decode_string(operands.subspan(2));
            }
            break;

        case op_execution_mode:
            if (operands.size() >= 5 && operands[1] == execution_mode_local_size) {
                out.local_size_x = operands[2];
                out.local_size_y = operands[3];
                out.local_size_z = operands[4];
            }
            break;

        case op_execution_mode_id:
            if (operands.size() >= 2 && operands[1] == execution_mode_local_size_id) {
                out.local_size_is_specialised = true;
            }
            break;

        case op_type_image:
            if (operands.size() >= 8) {
                if (IdRecord* const record = record_for(operands[0]); record != nullptr) {
                    record->opcode = opcode;
                    record->operand0 = operands[2];
                    record->operand1 = operands[6];
                    record->operand2 = operands[7];
                }
            }
            break;

        case op_type_sampler:
            if (!operands.empty()) {
                if (IdRecord* const record = record_for(operands[0]); record != nullptr) {
                    record->opcode = opcode;
                }
            }
            break;

        case op_type_sampled_image:
        case op_type_runtime_array:
            if (operands.size() >= 2) {
                if (IdRecord* const record = record_for(operands[0]); record != nullptr) {
                    record->opcode = opcode;
                    record->operand0 = operands[1];
                }
            }
            break;

        case op_type_array:
            if (operands.size() >= 3) {
                if (IdRecord* const record = record_for(operands[0]); record != nullptr) {
                    record->opcode = opcode;
                    record->operand0 = operands[1];
                    record->operand1 = operands[2];
                }
            }
            break;

        case op_type_struct:
            if (!operands.empty()) {
                if (IdRecord* const record = record_for(operands[0]); record != nullptr) {
                    record->opcode = opcode;
                }
            }
            break;

        case op_type_pointer:
            if (operands.size() >= 3) {
                if (IdRecord* const record = record_for(operands[0]); record != nullptr) {
                    record->opcode = opcode;
                    record->operand0 = operands[1];
                    record->operand1 = operands[2];
                }
            }
            break;

        case op_constant:
        case op_spec_constant:
            if (operands.size() >= 3) {
                if (IdRecord* const record = record_for(operands[1]); record != nullptr) {
                    record->opcode = opcode;
                    record->has_constant = true;
                    record->constant = operands[2];
                }
            }
            break;

        case op_spec_constant_true:
        case op_spec_constant_false:
        case op_spec_constant_composite:
        case op_spec_constant_op:
            if (operands.size() >= 2) {
                if (IdRecord* const record = record_for(operands[1]); record != nullptr) {
                    record->opcode = opcode;
                }
            }
            break;

        case op_variable:
            if (operands.size() >= 3) {
                if (IdRecord* const record = record_for(operands[1]); record != nullptr) {
                    record->opcode = opcode;
                    record->operand0 = operands[0];
                    record->operand1 = operands[2];
                }
                if (operands[2] == storage_class_push_constant) {
                    out.uses_push_constants = true;
                }
                if (operands[1] < ids.size()) {
                    descriptor_variables.push_back(operands[1]);
                }
            }
            break;

        case op_decorate:
            if (operands.size() >= 2) {
                IdRecord* const record = record_for(operands[0]);
                if (record == nullptr) {
                    break;
                }
                switch (operands[1]) {
                case decoration_descriptor_set:
                    if (operands.size() >= 3) {
                        record->has_set = true;
                        record->set = operands[2];
                    }
                    break;
                case decoration_binding:
                    if (operands.size() >= 3) {
                        record->has_binding = true;
                        record->binding = operands[2];
                    }
                    break;
                case decoration_spec_id:
                    if (operands.size() >= 3) {
                        record->has_spec_id = true;
                        record->spec_id = operands[2];
                    }
                    break;
                case decoration_block:
                    record->block = true;
                    break;
                case decoration_buffer_block:
                    record->buffer_block = true;
                    break;
                case decoration_non_readable:
                    record->non_readable = true;
                    break;
                case decoration_non_writable:
                    record->non_writable = true;
                    break;
                case decoration_built_in:
                    if (operands.size() >= 3 && operands[2] == built_in_workgroup_size) {
                        record->workgroup_size_built_in = true;
                    }
                    break;
                default:
                    break;
                }
            }
            break;

        default:
            break;
        }

        offset += word_count;
    }

    for (const std::uint32_t variable_id : descriptor_variables) {
        const IdRecord& variable = ids[variable_id];
        if (!variable.has_set && !variable.has_binding) {
            continue;
        }

        Binding binding{
            .set = variable.set,
            .binding = variable.binding,
            .non_readable = variable.non_readable,
            .non_writable = variable.non_writable,
        };

        const std::uint32_t pointer_id = variable.operand0;
        if (pointer_id >= ids.size() || ids[pointer_id].opcode != op_type_pointer) {
            out.bindings.push_back(binding);
            continue;
        }

        const std::uint32_t storage_class = ids[pointer_id].operand0;
        const ResolvedType resolved = resolve_array(ids, ids[pointer_id].operand1);
        if (!resolved.resolved) {
            out.bindings.push_back(binding);
            continue;
        }
        binding.array_size = resolved.array_size;

        const IdRecord& base = ids[resolved.base_id];
        switch (base.opcode) {
        case op_type_image:
            binding.kind = base.operand1 == 2 ? ResourceKind::storage_image
                                              : ResourceKind::separate_image;
            binding.image_dim = base.operand0;
            binding.image_format = base.operand2;
            break;

        case op_type_sampled_image: {
            binding.kind = ResourceKind::sampled_image;
            const std::uint32_t image_id = base.operand0;
            if (image_id < ids.size() && ids[image_id].opcode == op_type_image) {
                binding.image_dim = ids[image_id].operand0;
                binding.image_format = ids[image_id].operand2;
            }
            break;
        }

        case op_type_sampler:
            binding.kind = ResourceKind::sampler;
            break;

        case op_type_struct:
            if (storage_class == storage_class_storage_buffer || base.buffer_block) {
                binding.kind = ResourceKind::storage_buffer;
            } else if (storage_class == storage_class_uniform && base.block) {
                binding.kind = ResourceKind::uniform_buffer;
            }
            break;

        default:
            if (storage_class == storage_class_uniform_constant) {
                binding.kind = ResourceKind::unknown;
            }
            break;
        }

        out.bindings.push_back(binding);
    }

    std::ranges::sort(out.bindings, [](const Binding& left, const Binding& right) {
        return left.set != right.set ? left.set < right.set : left.binding < right.binding;
    });

    for (const IdRecord& record : ids) {
        if (record.workgroup_size_built_in && is_spec_constant(record.opcode)) {
            out.local_size_is_specialised = true;
        }
    }

    for (std::uint32_t id = 0; id < ids.size(); ++id) {
        const IdRecord& record = ids[id];
        if (!record.has_spec_id) {
            continue;
        }
        out.spec_constants.push_back(SpecConstant{
            .spec_id = record.spec_id,
            .result_id = id,
            .value = record.constant,
            .has_value = record.has_constant,
        });
    }

    if (out.entry_point.empty()) {
        return ErrorCode::shader_interface_mismatch;
    }

    return ErrorCode::ok;
}

ErrorCode inspect_bytes(const std::span<const std::uint8_t> bytes, Inventory& out) {
    if (bytes.size() < header_words * sizeof(std::uint32_t) || (bytes.size() % sizeof(std::uint32_t)) != 0) {
        return ErrorCode::invalid_argument;
    }

    std::vector<std::uint32_t> words(bytes.size() / sizeof(std::uint32_t));
    for (std::size_t index = 0; index < words.size(); ++index) {
        const std::size_t offset = index * sizeof(std::uint32_t);
        words[index] = static_cast<std::uint32_t>(bytes[offset])
                     | (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U)
                     | (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U)
                     | (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
    }

    return inspect(words, out);
}

bool is_spirv(const std::span<const std::uint8_t> bytes) noexcept {
    if (bytes.size() < sizeof(std::uint32_t)) {
        return false;
    }
    const std::uint32_t word = static_cast<std::uint32_t>(bytes[0])
                             | (static_cast<std::uint32_t>(bytes[1]) << 8U)
                             | (static_cast<std::uint32_t>(bytes[2]) << 16U)
                             | (static_cast<std::uint32_t>(bytes[3]) << 24U);
    return word == magic;
}

bool has_capability(const Inventory& inventory, const Capability capability) noexcept {
    const auto value = static_cast<std::uint32_t>(capability);
    return std::ranges::find(inventory.capabilities, value) != inventory.capabilities.end();
}

DescriptorCounts count_descriptors(const Inventory& inventory) noexcept {
    DescriptorCounts counts{};
    for (const Binding& binding : inventory.bindings) {
        switch (binding.kind) {
        case ResourceKind::sampler: counts.samplers += binding.array_size; break;
        case ResourceKind::sampled_image: counts.sampled_images += binding.array_size; break;
        case ResourceKind::separate_image: counts.separate_images += binding.array_size; break;
        case ResourceKind::storage_image: counts.storage_images += binding.array_size; break;
        case ResourceKind::uniform_buffer: counts.uniform_buffers += binding.array_size; break;
        case ResourceKind::storage_buffer: counts.storage_buffers += binding.array_size; break;
        case ResourceKind::unknown: break;
        }
        counts.highest_set = std::max(counts.highest_set, binding.set);
        counts.highest_binding
            = std::max(counts.highest_binding, binding.binding + binding.array_size - 1U);
    }
    return counts;
}

std::string_view capability_name(const std::uint32_t capability) noexcept {
    switch (static_cast<Capability>(capability)) {
    case Capability::shader: return "Shader";
    case Capability::float16: return "Float16";
    case Capability::int16: return "Int16";
    case Capability::int8: return "Int8";
    case Capability::storage_image_extended_formats: return "StorageImageExtendedFormats";
    case Capability::image_query: return "ImageQuery";
    case Capability::derivative_control: return "DerivativeControl";
    case Capability::storage_image_read_without_format: return "StorageImageReadWithoutFormat";
    case Capability::storage_image_write_without_format: return "StorageImageWriteWithoutFormat";
    case Capability::variable_pointers: return "VariablePointers";
    }
    return "unnamed";
}

std::string_view image_format_name(const std::uint32_t format) noexcept {
    switch (static_cast<ImageFormat>(format)) {
    case ImageFormat::unknown: return "Unknown";
    case ImageFormat::rgba32f: return "Rgba32f";
    case ImageFormat::rgba16f: return "Rgba16f";
    case ImageFormat::r32f: return "R32f";
    case ImageFormat::rgba8: return "Rgba8";
    case ImageFormat::rgba8_snorm: return "Rgba8Snorm";
    case ImageFormat::rg32f: return "Rg32f";
    case ImageFormat::rg16f: return "Rg16f";
    case ImageFormat::r11f_g11f_b10f: return "R11fG11fB10f";
    case ImageFormat::r16f: return "R16f";
    case ImageFormat::rgba16: return "Rgba16";
    case ImageFormat::rgb10_a2: return "Rgb10A2";
    case ImageFormat::rg16: return "Rg16";
    case ImageFormat::rg8: return "Rg8";
    case ImageFormat::r16: return "R16";
    case ImageFormat::r8: return "R8";
    case ImageFormat::rgba16_snorm: return "Rgba16Snorm";
    case ImageFormat::rg16_snorm: return "Rg16Snorm";
    case ImageFormat::rg8_snorm: return "Rg8Snorm";
    case ImageFormat::r16_snorm: return "R16Snorm";
    case ImageFormat::r8_snorm: return "R8Snorm";
    }
    return "unnamed";
}

std::string_view resource_kind_name(const ResourceKind kind) noexcept {
    switch (kind) {
    case ResourceKind::unknown: return "unknown";
    case ResourceKind::sampler: return "sampler";
    case ResourceKind::sampled_image: return "sampled-image";
    case ResourceKind::separate_image: return "separate-image";
    case ResourceKind::storage_image: return "storage-image";
    case ResourceKind::uniform_buffer: return "uniform-buffer";
    case ResourceKind::storage_buffer: return "storage-buffer";
    }
    return "unknown";
}

ErrorCode patch_storage_image_format(
    const std::span<std::uint32_t> words,
    const ImageFormat format,
    PatchSummary& summary) noexcept {
    summary = PatchSummary{};

    if (words.size() < header_words || words[0] != magic) {
        return ErrorCode::invalid_argument;
    }

    for (std::size_t offset = header_words; offset < words.size();) {
        const std::uint32_t instruction = words[offset];
        const auto word_count = static_cast<std::uint16_t>(instruction >> 16U);
        const auto opcode = static_cast<std::uint16_t>(instruction & 0xffffU);

        if (word_count == 0 || offset + word_count > words.size()) {
            return ErrorCode::invalid_argument;
        }

        if (opcode == op_capability && word_count >= 2) {
            std::uint32_t& capability = words[offset + 1U];
            if (capability == static_cast<std::uint32_t>(Capability::storage_image_write_without_format)) {
                capability = static_cast<std::uint32_t>(Capability::shader);
                ++summary.capabilities_replaced;
            }
        }

        if (opcode == op_type_image && word_count >= 9) {
            const std::uint32_t sampled = words[offset + 7U];
            std::uint32_t& image_format = words[offset + 8U];
            if (sampled == 2 && image_format == static_cast<std::uint32_t>(ImageFormat::unknown)) {
                image_format = static_cast<std::uint32_t>(format);
                ++summary.images_formatted;
            }
        }

        offset += word_count;
    }

    return ErrorCode::ok;
}

}
