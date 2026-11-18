// The tool's precompiled header: Cemu's src/Common/precompiled.h, with DEBUG_BREAK behaving as on the console.
// There DEBUG_BREAK is raise(SIGTRAP), and newlib's raise() on Horizon has no handler to call: it ends in _kill_r,
// which only sets errno (ENOSYS) and returns (checked in the Switch 2 build's code). A failed cemu_assert() is thus
// ignored on the console, and the decompiler goes on; MSVC's __debugbreak() would stop the process instead.
#pragma once

#include <intrin.h>
#define __debugbreak() ((void)0)

#include "Common/precompiled.h"
