#!/usr/bin/env python3
"""Copy definitions out of Cemu's sources into generated translation units for the host tool.

The console's shader-cache loader (LatteShaderCache.cpp), the Deko3D renderer's key, translation and validation
code (DKShader.cpp, DKShader.h, DKCommon.h) and a few helpers the decompiler calls (ActiveSettings.cpp,
LatteRenderTarget.cpp, LatteTextureLegacy.cpp) live in files that cannot be compiled on a PC as a whole (UI,
audio, deko3d, the rest of the emulator). Rather than keeping hand-made copies that could drift from the sources,
the build extracts the definitions it needs, verbatim, every time it runs; a definition that cannot be found stops
the build. LatteDecompilerAnalyzer.cpp is compiled from a copy with ANALYZER_REWRITES applied (see there).
"""
import argparse
import re
import sys
from pathlib import Path


class ExtractError(Exception):
    pass


def skip_literal(text, i):
    """Return the index after the string/char literal starting at text[i]."""
    quote = text[i]
    i += 1
    while i < len(text):
        if text[i] == "\\":
            i += 2
            continue
        if text[i] == quote:
            return i + 1
        if text[i] == "\n" and quote == "'":
            break
        i += 1
    raise ExtractError("unterminated literal")


def matching_brace(text, start):
    """text[start] is '{': return the index of its matching '}' (comments and literals skipped)."""
    depth = 0
    i = start
    while i < len(text):
        c = text[i]
        if text.startswith("//", i):
            i = text.find("\n", i)
            if i < 0:
                break
            continue
        if text.startswith("/*", i):
            i = text.find("*/", i) + 2
            continue
        if c == '"':
            i = skip_literal(text, i)
            continue
        # (a ' after a digit or letter is a digit separator, e.g. 48'000)
        if c == "'" and not (i > 0 and text[i - 1].isalnum()):
            i = skip_literal(text, i)
            continue
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    raise ExtractError("unbalanced braces")


def extract_block(text, pattern):
    """A definition starting at a line matching pattern and ending with its outermost brace block. A '};' or a
    '}name;' right after the block (struct definitions) is included."""
    # (the first match whose body follows: a ';' before the '{' means a declaration)
    for match in re.finditer(pattern, text, re.MULTILINE):
        brace = text.find("{", match.start())
        if brace >= 0 and ";" not in text[match.start():brace]:
            break
    else:
        raise ExtractError(f"definition not found: {pattern}")
    start = text.rfind("\n", 0, match.start()) + 1
    end = matching_brace(text, brace) + 1
    tail = re.match(r"[ \t]*[A-Za-z_0-9\[\]]*[ \t]*;", text[end:])
    if tail:
        end += tail.end()
    return start, text[start:end]


def extract_line(text, pattern):
    match = re.search(pattern, text, re.MULTILINE)
    if not match:
        raise ExtractError(f"line not found: {pattern}")
    start = text.rfind("\n", 0, match.start()) + 1
    end = text.find("\n", match.end())
    return start, text[start:end if end >= 0 else len(text)]


def extract_conditional(text, pattern):
    """A preprocessor #if block (from the line matching pattern to its #endif)."""
    match = re.search(pattern, text, re.MULTILINE)
    if not match:
        raise ExtractError(f"conditional not found: {pattern}")
    start = text.rfind("\n", 0, match.start()) + 1
    depth = 0
    pos = start
    for line in text[start:].splitlines(keepends=True):
        stripped = line.strip()
        if re.match(r"#\s*if", stripped):
            depth += 1
        elif re.match(r"#\s*endif", stripped):
            depth -= 1
            if depth == 0:
                return start, text[start:pos + len(line)].rstrip("\n")
        pos += len(line)
    raise ExtractError(f"unterminated conditional: {pattern}")


def extract_statement(text, pattern):
    """A statement (from the line matching pattern to its ';')."""
    match = re.search(pattern, text, re.MULTILINE)
    if not match:
        raise ExtractError(f"statement not found: {pattern}")
    start = text.rfind("\n", 0, match.start()) + 1
    end = text.find(";", match.end())
    if end < 0:
        raise ExtractError(f"unterminated statement: {pattern}")
    return start, text[start:end + 1]


KINDS = {"block": extract_block, "line": extract_line, "conditional": extract_conditional,
         "statement": extract_statement}


def extract(path, items):
    text = path.read_text(encoding="utf-8").replace("\r\n", "\n")
    found = []
    for kind, pattern in items:
        try:
            found.append(KINDS[kind](text, pattern))
        except ExtractError as error:
            raise ExtractError(f"{path}: {error}") from None
    found.sort(key=lambda item: item[0])  # source order keeps definitions before their users
    return [body for _, body in found]


# --- what the tool takes from Cemu ---------------------------------------------------------------------------

SHADER_CACHE_ITEMS = [
    ("line", r"^#define SHADER_CACHE_COMPILE_QUEUE_SIZE\b"),
    ("block", r"^struct\s*\n\{\s*\n\s*sint32 compiledShaderCount;"),
    ("line", r"^#define SHADER_CACHE_GENERIC_EXTRA_VERSION\b"),
    ("line", r"^#define SHADER_CACHE_TYPE_VERTEX\b"),
    ("line", r"^#define SHADER_CACHE_TYPE_GEOMETRY\b"),
    ("line", r"^#define SHADER_CACHE_TYPE_PIXEL\b"),
    ("block", r"^struct\s*\n\{\s*\n\s*struct\s*\n\s*\{\s*\n\s*LatteDecompilerShader\* shader;"),
    ("block", r"^void LatteShaderCache_initCompileQueue\(\)"),
    ("block", r"^void LatteShaderCache_addToCompileQueue\("),
    ("block", r"^void LatteShaderCache_removeFromCompileQueue\("),
    ("block", r"^void LatteShaderCache_updateCompileQueue\("),
    ("line", r"^FileCache\* s_shaderCacheGeneric = nullptr;"),
    ("block", r"^uint32 LatteShaderCache_getShaderCacheExtraVersion\("),
    ("block", r"^uint64 LatteShaderCache_getShaderNameInTransferableCache\("),
    ("block", r"^void LatteShaderCache_writeSeparableVertexShader\("),
    ("block", r"^void LatteShaderCache_writeSeparableGeometryShader\("),
    ("block", r"^void LatteShaderCache_writeSeparablePixelShader\("),
    ("block", r"^void LatteShaderCache_loadOrCompileSeparableShader\("),
    ("block", r"^bool LatteShaderCache_readSeparableVertexShader\("),
    ("block", r"^bool LatteShaderCache_readSeparableGeometryShader\("),
    ("block", r"^bool LatteShaderCache_readSeparablePixelShader\("),
    ("block", r"^bool LatteShaderCache_readSeparableShader\("),
]

# settings the decompiler reads (title workarounds, shader dumping)
ACTIVE_SETTINGS_ITEMS = [
    ("block", r"^bool ActiveSettings::DumpShadersEnabled\(\)"),
    ("block", r"^void ActiveSettings::EnableDumpShaders\("),
    ("block", r"^bool ActiveSettings::ShaderPreventInfiniteLoopsEnabled\(\)"),
    ("block", r"^bool ActiveSettings::ForceSamplerRoundToPrecision\(\)"),
]

# render target state the analyzer reads (it only feeds Metal's framebuffer fetch, but stays as the console has it)
RENDER_TARGET_ITEMS = [
    ("block", r"^uint8 LatteMRT::GetActiveColorBufferMask\("),
    ("block", r"^bool LatteMRT::GetActiveDepthBufferMask\("),
    ("block", r"^const uint32 _colorBufferFormatBits\[\]"),
    ("block", r"^Latte::E_GX2SURFFMT LatteMRT::GetColorBufferFormat\("),
]

TEXTURE_LEGACY_ITEMS = [
    ("block", r"^Latte::E_GX2SURFFMT LatteTexture_ReconstructGX2Format\("),
]

DK_SHADER_H_ITEMS = [
    ("block", r"^\s*struct ShaderBindings\b"),
]

DK_COMMON_ITEMS = [
    ("line", r"^\s*constexpr u32 PER_WARP_SCRATCH_MEMORY_SIZE\b"),
]

DK_SHADER_ITEMS = [
    ("conditional", r"^\s*#if defined\(__SWITCH2__\)\s*\n(\s*//.*\n)*\s*constexpr int kMaxUniformBindings"),
    ("line", r"^\s*constexpr u32 CACHE_VERSION\b"),
    ("line", r"^\s*constexpr u32 DKSH_MAGIC\b"),
    ("block", r"^\s*struct DkshHeader\b"),
    ("block", r"^\s*struct DkshProgram\b"),
    ("block", r"^\s*struct CacheHeader\b"),
    ("line", r"^\s*constexpr u32 MAX_BINARY_SIZE\b"),
    ("block", r"^\s*u64 SourceHash\("),
    ("block", r"^\s*bool ValidateBinary\("),
    ("block", r"^\s*bool ValidateBindings\("),
    ("block", r"^\s*std::string Translate\("),
]

# Compile()'s renderer stage -> UAM stage mapping (the second half of the cache key)
DK_SHADER_STAGE_ITEM = [
    ("statement", r"^\s*const int stage = type == RendererShader::ShaderType::kVertex"),
]

# Undefined behavior the console's compiler resolves one way and MSVC another. SET_GRADIENTS_H/V instructions have
# dstGpr = -1 (LatteDecompiler.cpp), and LatteDecompiler_analyzeTEXClause then shifts by -1 % 8 = -1. devkitA64's
# GCC compiles the shift count as dstGpr & 7 (checked in the Switch build's code: "and w2, w2, #0x7; lsl w2, w0,
# w2"), so the console marks R7 as used and declares it in such shaders; MSVC's shift by -1 sets nothing. The tool
# compiles a copy of the file with the console's result spelled out. If the source changes there, the original is
# compiled instead (and the key comparison with the deko3d packs tells whether that still matches the console).
ANALYZER_REWRITES = [
    ("shaderContext->analyzer.gprUseMask[texInstruction.dstGpr/8] |= (1<<(texInstruction.dstGpr%8));",
     "shaderContext->analyzer.gprUseMask[texInstruction.dstGpr/8] |= (1<<(texInstruction.dstGpr&7)); "
     "/* drako3d_shader_packs: GCC's shift count for the console (extract_sources.py) */"),
]


def rewritten_copy(path, rewrites):
    text = path.read_text(encoding="utf-8")
    for old, new in rewrites:
        count = text.count(old)
        if count == 1:
            text = text.replace(old, new)
        else:
            print(f"extract_sources: warning: {path.name}: expected one '{old}', found {count}; compiling it as is",
                  file=sys.stderr)
    return text


def banner(sources):
    names = ", ".join(sources)
    return (f"// Generated by tools/drako3d_shader_packs/host/extract_sources.py from {names}.\n"
            "// Do not edit: the definitions below are verbatim copies of Cemu's sources, refreshed on every build.\n")


def write_if_changed(path, content):
    if path.exists() and path.read_text(encoding="utf-8") == content:
        return
    path.write_text(content, encoding="utf-8", newline="\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--src", type=Path, required=True, help="Cemu's src directory")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    try:
        cache_items = extract(args.src / "Cafe/HW/Latte/Core/LatteShaderCache.cpp", SHADER_CACHE_ITEMS)
        bindings = extract(args.src / "Cafe/HW/Latte/Renderer/Deko3D/DKShader.h", DK_SHADER_H_ITEMS)
        common = extract(args.src / "Cafe/HW/Latte/Renderer/Deko3D/DKCommon.h", DK_COMMON_ITEMS)
        dk_items = extract(args.src / "Cafe/HW/Latte/Renderer/Deko3D/DKShader.cpp", DK_SHADER_ITEMS)
        dk_stage = extract(args.src / "Cafe/HW/Latte/Renderer/Deko3D/DKShader.cpp", DK_SHADER_STAGE_ITEM)
        settings = extract(args.src / "config/ActiveSettings.cpp", ACTIVE_SETTINGS_ITEMS)
        render_target = extract(args.src / "Cafe/HW/Latte/Core/LatteRenderTarget.cpp", RENDER_TARGET_ITEMS)
        texture_legacy = extract(args.src / "Cafe/HW/Latte/Core/LatteTextureLegacy.cpp", TEXTURE_LEGACY_ITEMS)
    except ExtractError as error:
        print(f"extract_sources: {error}", file=sys.stderr)
        return 1

    shader_cache = banner(["src/Cafe/HW/Latte/Core/LatteShaderCache.cpp"]) + """
#include "Cafe/HW/Latte/Core/LatteConst.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.h"
#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "Cemu/FileCache/FileCache.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/Common/RegisterSerializer.h"
#include "Cafe/HW/Latte/Common/ShaderSerializer.h"
#include "util/helpers/Serializer.h"

""" + "\n\n".join(cache_items) + """

#include "shader_cache_exports.inc"
"""
    dk_shader = banner(["src/Cafe/HW/Latte/Renderer/Deko3D/DKShader.cpp", "DKShader.h", "DKCommon.h"]) + """
#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/SPIRV/GlslangToSpv.h>
#include <spirv_glsl.hpp>
#include "Cafe/HW/Latte/Renderer/RendererShader.h"
#include "Cafe/HW/Latte/Renderer/Deko3D/UamBridge.h"
#include "drako3d_defs.h"

// The definitions below are taken as the Switch 2 (drako3d) build compiles them.
#define __SWITCH2__ 1

namespace Deko3D
{
""" + "\n".join(bindings) + "\n" + "\n".join(common) + """
	namespace
	{
""" + "\n".join(dk_items) + """

		int UamStageOf(RendererShader::ShaderType type)
		{
""" + dk_stage[0] + """
			return stage;
		}
	} // namespace

#include "dkshader_exports.inc"
} // namespace Deko3D
"""
    support = banner(["src/config/ActiveSettings.cpp", "src/Cafe/HW/Latte/Core/LatteRenderTarget.cpp",
                      "src/Cafe/HW/Latte/Core/LatteTextureLegacy.cpp"]) + """
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/Core/LatteCachedFBO.h"
#include "Cafe/CafeSystem.h"
#include "config/ActiveSettings.h"

// src/config/ActiveSettings.cpp
""" + "\n\n".join(settings) + """

// src/Cafe/HW/Latte/Core/LatteRenderTarget.cpp
""" + "\n\n".join(render_target) + """

// src/Cafe/HW/Latte/Core/LatteTextureLegacy.cpp
""" + "\n\n".join(texture_legacy) + "\n"
    analyzer_path = args.src / "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompilerAnalyzer.cpp"
    analyzer = ("// Generated by tools/drako3d_shader_packs/host/extract_sources.py: a copy of "
                "src/Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompilerAnalyzer.cpp\n"
                "// with the rewrites listed in ANALYZER_REWRITES there. Do not edit.\n"
                + rewritten_copy(analyzer_path, ANALYZER_REWRITES))
    write_if_changed(args.out / "gen_shader_cache.cpp", shader_cache)
    write_if_changed(args.out / "gen_support.cpp", support)
    write_if_changed(args.out / "gen_LatteDecompilerAnalyzer.cpp", analyzer)
    write_if_changed(args.out / "gen_dkshader.cpp", dk_shader)
    return 0


if __name__ == "__main__":
    sys.exit(main())
