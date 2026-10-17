// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0-or-later

#include <lsfg/common/dksh.hpp>

#include <UamBridge.h>

#include <cstdlib>
#include <string>

namespace lsfg::dksh {

bool compiler_available() noexcept {
    return true;
}

ErrorCode compile(const std::string_view glsl, Blob& out) {
    out = Blob{};

    if (glsl.empty()) {
        return ErrorCode::invalid_argument;
    }

    const std::string source(glsl);

    std::size_t size = 0;
    char* log = nullptr;
    void* const dksh = UamCompileGlsl(source.c_str(), UamStage_Compute, &size, &log);

    if (log != nullptr) {
        out.log.assign(log);
        std::free(log);
    }

    if (dksh == nullptr || size == 0) {
        std::free(dksh);
        return ErrorCode::shader_compile_failed;
    }

    const auto* const bytes = static_cast<const std::uint8_t*>(dksh);
    out.bytes.assign(bytes, bytes + size);
    std::free(dksh);

    return validate(out.bytes, out.program);
}

}
