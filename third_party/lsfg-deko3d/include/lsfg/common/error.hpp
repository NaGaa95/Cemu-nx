// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string_view>

namespace lsfg {

enum class ErrorCode : std::uint32_t {
    ok = 0x0000'0000,

    invalid_argument = 0x0000'0001,
    invalid_state = 0x0000'0002,
    out_of_memory = 0x0000'0003,
    io_error = 0x0000'0004,
    timed_out = 0x0000'0005,
    unsupported = 0x0000'0006,

    cache_missing = 0x0001'0001,
    cache_version_mismatch = 0x0001'0002,
    cache_integrity_failure = 0x0001'0003,
    cache_configuration_mismatch = 0x0001'0004,

    shader_set_unknown = 0x0002'0001,
    shader_interface_mismatch = 0x0002'0002,
    shader_compile_failed = 0x0002'0003,

    backend_unavailable = 0x0003'0001,
    backend_dispatch_failed = 0x0003'0002,
    image_layout_unsupported = 0x0003'0003,

    title_not_allowed = 0x0004'0001,
    build_not_allowed = 0x0004'0002,
    hook_install_failed = 0x0004'0003,

    presentation_deadline_missed = 0x0005'0001,
    presentation_sequence_invalid = 0x0005'0002,

    protocol_version_mismatch = 0x0006'0001,
    protocol_message_invalid = 0x0006'0002,

    emergency_bypass = 0x0007'0001,
};

[[nodiscard]] constexpr bool succeeded(const ErrorCode code) noexcept {
    return code == ErrorCode::ok;
}

[[nodiscard]] std::string_view error_name(ErrorCode code) noexcept;

}
