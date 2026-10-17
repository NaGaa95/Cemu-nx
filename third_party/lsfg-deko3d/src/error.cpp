// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0-or-later

#include <lsfg/common/error.hpp>

namespace lsfg {

std::string_view error_name(const ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::ok: return "ok";
    case ErrorCode::invalid_argument: return "invalid_argument";
    case ErrorCode::invalid_state: return "invalid_state";
    case ErrorCode::out_of_memory: return "out_of_memory";
    case ErrorCode::io_error: return "io_error";
    case ErrorCode::timed_out: return "timed_out";
    case ErrorCode::unsupported: return "unsupported";
    case ErrorCode::cache_missing: return "cache_missing";
    case ErrorCode::cache_version_mismatch: return "cache_version_mismatch";
    case ErrorCode::cache_integrity_failure: return "cache_integrity_failure";
    case ErrorCode::cache_configuration_mismatch: return "cache_configuration_mismatch";
    case ErrorCode::shader_set_unknown: return "shader_set_unknown";
    case ErrorCode::shader_interface_mismatch: return "shader_interface_mismatch";
    case ErrorCode::shader_compile_failed: return "shader_compile_failed";
    case ErrorCode::backend_unavailable: return "backend_unavailable";
    case ErrorCode::backend_dispatch_failed: return "backend_dispatch_failed";
    case ErrorCode::image_layout_unsupported: return "image_layout_unsupported";
    case ErrorCode::title_not_allowed: return "title_not_allowed";
    case ErrorCode::build_not_allowed: return "build_not_allowed";
    case ErrorCode::hook_install_failed: return "hook_install_failed";
    case ErrorCode::presentation_deadline_missed: return "presentation_deadline_missed";
    case ErrorCode::presentation_sequence_invalid: return "presentation_sequence_invalid";
    case ErrorCode::protocol_version_mismatch: return "protocol_version_mismatch";
    case ErrorCode::protocol_message_invalid: return "protocol_message_invalid";
    case ErrorCode::emergency_bypass: return "emergency_bypass";
    }
    return "unknown_error";
}

}
