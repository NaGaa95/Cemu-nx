// What gen_dkshader.cpp needs from the console's headers: libnx's integer types (switch/types.h) and drako3d's
// binding counts (drako3d.h, ouncebrew's ounce-devkit: the same values as deko3d.h).
#pragma once

#include <cstdint>

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s32 = int32_t;

#define DK_NUM_UNIFORM_BUFS 16
#define DK_NUM_STORAGE_BUFS 16
#define DK_NUM_TEXTURE_BINDINGS 32
