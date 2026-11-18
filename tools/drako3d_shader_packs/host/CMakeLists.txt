# Host (PC) build of the shader pack tool: Cemu's own shader decompiler and loader (compiled read-only from
# ../../../src) and the Deko3D renderer's GLSL translation (glslang + SPIRV-Cross at the commits and patches the
# Switch builds pin). Cemu's sources build on Windows with MSVC only (src/Common/precompiled.h), so this needs
# Visual Studio 2022 (Build Tools are enough: MSVC for the host architecture and a Windows SDK).
# ../make_packs.py build configures and builds it.
cmake_minimum_required(VERSION 3.24)
project(drako3d_shader_packs CXX C)

if(NOT MSVC)
	message(FATAL_ERROR "Build with MSVC (Visual Studio 2022): Cemu's sources need it on Windows")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

get_filename_component(_cemu_root "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(CEMU_ROOT "${_cemu_root}" CACHE PATH "Cemu-nx checkout whose sources the tool is built from")
set(CEMU_SRC "${CEMU_ROOT}/src")
if(NOT EXISTS "${CEMU_SRC}/Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.cpp")
	message(FATAL_ERROR "Cemu sources not found at ${CEMU_SRC}")
endif()

include(FetchContent)
find_package(Git REQUIRED)
set(FETCHCONTENT_QUIET OFF)

# fmt, glm and glslang as cmake/switch/SwitchDependencies.cmake pins them, SPIRV-Cross as
# cmake/switch/Deko3DDependencies.cmake does, Boost as dist/switch/deps/build_boost.sh fetches it (headers only).
FetchContent_Declare(fmt
	GIT_REPOSITORY https://github.com/fmtlib/fmt.git
	GIT_TAG        407c905e45ad75fc29bf0f9bb7c5c2fd3475976f # 12.1.0
	SOURCE_SUBDIR  __header_only__)
FetchContent_Declare(glm
	GIT_REPOSITORY https://github.com/g-truc/glm.git
	GIT_TAG        0af55ccecd98d4e5a8d1fad7de25ba429d60e863 # 1.0.1
	SOURCE_SUBDIR  __header_only__)
FetchContent_Declare(deko_spirv_cross
	GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Cross.git
	GIT_TAG        ebe2aa0cd80f5eb5cd8a605da604cacf72205f3b
	SOURCE_SUBDIR  __not_used__)
FetchContent_Declare(boost_headers
	URL      https://archives.boost.io/release/1.86.0/source/boost_1_86_0.tar.bz2
	URL_HASH SHA256=1bed88e40401b2cb7a1f76d4bab499e352fa4d0c5f31c0dbae64e24d34d7513b
	DOWNLOAD_EXTRACT_TIMESTAMP TRUE
	SOURCE_SUBDIR  __header_only__)
# zlib and zstd for Cemu's FileCache and its serialized register/shader state (compiled from source below)
FetchContent_Declare(zlib_src
	GIT_REPOSITORY https://github.com/madler/zlib.git
	GIT_TAG        v1.3.1
	SOURCE_SUBDIR  __not_used__)
FetchContent_Declare(zstd_src
	GIT_REPOSITORY https://github.com/facebook/zstd.git
	GIT_TAG        v1.5.6
	SOURCE_SUBDIR  __not_used__)
# (pugixml: only for headers that the compiled sources include)
FetchContent_Declare(pugixml_src
	GIT_REPOSITORY https://github.com/zeux/pugixml.git
	GIT_TAG        db78afc2b7d8f043b4bc6b185635d949ea2ed2a8 # v1.14
	SOURCE_SUBDIR  __not_used__)
FetchContent_MakeAvailable(fmt glm deko_spirv_cross boost_headers zlib_src zstd_src pugixml_src)

set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
set(ENABLE_PCH OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glslang
	GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
	GIT_TAG        1062752a891c95b2bfeed9e356562d88f9df84ac) # 15.1.0
FetchContent_MakeAvailable(glslang)

# The Switch builds patch SPIRV-Cross (cmake/switch/patches/spirv-cross-shadow1d.patch): apply the same file.
set(SPIRV_CROSS_PATCH "${CEMU_ROOT}/cmake/switch/patches/spirv-cross-shadow1d.patch")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SPIRV_CROSS_PATCH}")
execute_process(
	COMMAND "${GIT_EXECUTABLE}" apply --ignore-space-change --check "${SPIRV_CROSS_PATCH}"
	WORKING_DIRECTORY "${deko_spirv_cross_SOURCE_DIR}"
	RESULT_VARIABLE _patch_check OUTPUT_QUIET ERROR_QUIET)
if(_patch_check EQUAL 0)
	execute_process(
		COMMAND "${GIT_EXECUTABLE}" apply --ignore-space-change "${SPIRV_CROSS_PATCH}"
		WORKING_DIRECTORY "${deko_spirv_cross_SOURCE_DIR}"
		RESULT_VARIABLE _patch_apply)
	if(NOT _patch_apply EQUAL 0)
		message(FATAL_ERROR "Failed to apply ${SPIRV_CROSS_PATCH}")
	endif()
else()
	execute_process(
		COMMAND "${GIT_EXECUTABLE}" apply --ignore-space-change --reverse --check "${SPIRV_CROSS_PATCH}"
		WORKING_DIRECTORY "${deko_spirv_cross_SOURCE_DIR}"
		RESULT_VARIABLE _patch_reverse OUTPUT_QUIET ERROR_QUIET)
	if(NOT _patch_reverse EQUAL 0)
		message(FATAL_ERROR "SPIRV-Cross source does not match ${SPIRV_CROSS_PATCH}")
	endif()
endif()

add_compile_options(/MP /EHsc /fp:precise /utf-8 /bigobj /W0)
add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)

# SPIRV-Cross as third_party/lsfg-deko3d builds it for the renderer (lsfg_spirv_cross)
add_library(deko_spirv_cross STATIC
	"${deko_spirv_cross_SOURCE_DIR}/spirv_cfg.cpp"
	"${deko_spirv_cross_SOURCE_DIR}/spirv_cross.cpp"
	"${deko_spirv_cross_SOURCE_DIR}/spirv_cross_parsed_ir.cpp"
	"${deko_spirv_cross_SOURCE_DIR}/spirv_glsl.cpp"
	"${deko_spirv_cross_SOURCE_DIR}/spirv_parser.cpp")
target_include_directories(deko_spirv_cross SYSTEM PUBLIC "${deko_spirv_cross_SOURCE_DIR}")

set(_zlib_sources adler32 compress crc32 deflate gzclose gzlib gzread gzwrite infback inffast inflate inftrees trees
	uncompr zutil)
list(TRANSFORM _zlib_sources PREPEND "${zlib_src_SOURCE_DIR}/")
list(TRANSFORM _zlib_sources APPEND ".c")
add_library(host_zlib STATIC ${_zlib_sources})
target_include_directories(host_zlib PUBLIC "${zlib_src_SOURCE_DIR}")

file(GLOB _zstd_sources
	"${zstd_src_SOURCE_DIR}/lib/common/*.c"
	"${zstd_src_SOURCE_DIR}/lib/compress/*.c"
	"${zstd_src_SOURCE_DIR}/lib/decompress/*.c"
	"${zstd_src_SOURCE_DIR}/lib/dictBuilder/*.c")
add_library(host_zstd STATIC ${_zstd_sources})
target_compile_definitions(host_zstd PRIVATE ZSTD_DISABLE_ASM)
target_include_directories(host_zstd PUBLIC "${zstd_src_SOURCE_DIR}/lib")

# Cemu's decompiler path, compiled as the Switch 2 drako3d core compiles it: ENABLE_DEKO3D as the only renderer,
# release asserts, precompiled.h forced in, char unsigned (/J) as on AArch64. cemu_stubs.cpp stands in for the
# rest of the emulator.
set(CEMU_SOURCES
	Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.cpp
	Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompilerEmitGLSL.cpp
	Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompilerEmitGLSLAttrDecoder.cpp
	Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompilerRegisterDataTypeTracker.cpp
	Cafe/HW/Latte/Core/FetchShader.cpp
	Cafe/HW/Latte/Core/LatteGSCopyShaderParser.cpp
	Cafe/HW/Latte/Core/LatteShader.cpp
	Cafe/HW/Latte/Common/RegisterSerializer.cpp
	Cafe/HW/Latte/Common/ShaderSerializer.cpp
	Cemu/FileCache/FileCache.cpp
	Common/windows/FileStream_win32.cpp
	util/helpers/Serializer.cpp
	# (LatteShader.cpp's experimental decompiler path, never used, but in the same file)
	util/Zir/Core/IR.cpp
	util/Zir/EmitterGLSL/ZpIREmitGLSL.cpp
	util/Zir/Passes/RegisterAllocatorForGLSL.cpp
	util/Zir/Passes/ZpIRRegisterAllocator.cpp
	Cafe/HW/Latte/Transcompiler/LatteTC.cpp
	Cafe/HW/Latte/Transcompiler/LatteTCGenIR.cpp
	Cafe/HW/Latte/ShaderInfo/ShaderDescription.cpp
	Cafe/HW/Latte/ShaderInfo/ShaderInstanceInfo.cpp
)
list(TRANSFORM CEMU_SOURCES PREPEND "${CEMU_SRC}/")

# LatteShaderCache.cpp's loader and DKShader.cpp's key/translation/record code, copied verbatim at build time
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_custom_command(
	OUTPUT "${GEN_DIR}/gen_shader_cache.cpp" "${GEN_DIR}/gen_dkshader.cpp" "${GEN_DIR}/gen_support.cpp"
		"${GEN_DIR}/gen_LatteDecompilerAnalyzer.cpp"
	COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/extract_sources.py" --src "${CEMU_SRC}" --out "${GEN_DIR}"
	DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/extract_sources.py"
		"${CEMU_SRC}/Cafe/HW/Latte/Core/LatteShaderCache.cpp"
		"${CEMU_SRC}/Cafe/HW/Latte/Renderer/Deko3D/DKShader.cpp"
		"${CEMU_SRC}/Cafe/HW/Latte/Renderer/Deko3D/DKShader.h"
		"${CEMU_SRC}/Cafe/HW/Latte/Renderer/Deko3D/DKCommon.h"
		"${CEMU_SRC}/config/ActiveSettings.cpp"
		"${CEMU_SRC}/Cafe/HW/Latte/Core/LatteRenderTarget.cpp"
		"${CEMU_SRC}/Cafe/HW/Latte/Core/LatteTextureLegacy.cpp"
		"${CEMU_SRC}/Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompilerAnalyzer.cpp"
	COMMENT "Extracting definitions from Cemu's sources")
# (the renderer includes <glslang/SPIRV/GlslangToSpv.h>, glslang's installed layout; the Switch builds map it the
# same way in dependencies/switch_shims)
file(WRITE "${GEN_DIR}/include/glslang/SPIRV/GlslangToSpv.h"
	"#pragma once\n#include \"${glslang_SOURCE_DIR}/SPIRV/GlslangToSpv.h\"\n")

add_executable(drako_shader_tool
	src/main.cpp
	src/cemu_stubs.cpp
	src/openssl_shim.cpp
	"${GEN_DIR}/gen_shader_cache.cpp"
	"${GEN_DIR}/gen_dkshader.cpp"
	"${GEN_DIR}/gen_support.cpp"
	"${GEN_DIR}/gen_LatteDecompilerAnalyzer.cpp"
	${CEMU_SOURCES})
target_include_directories(drako_shader_tool PRIVATE
	"${CMAKE_CURRENT_SOURCE_DIR}/src"
	"${CMAKE_CURRENT_SOURCE_DIR}/shim"
	"${GEN_DIR}/include"
	"${CEMU_SRC}"
	"${CEMU_SRC}/Cafe"
	"${fmt_SOURCE_DIR}/include")
target_include_directories(drako_shader_tool SYSTEM PRIVATE
	"${glm_SOURCE_DIR}"
	"${boost_headers_SOURCE_DIR}"
	"${glslang_SOURCE_DIR}"
	"${pugixml_src_SOURCE_DIR}/src")
target_compile_definitions(drako_shader_tool PRIVATE
	ENABLE_DEKO3D WIN32_LEAN_AND_MEAN BOOST_ALL_NO_LIB)
target_compile_options(drako_shader_tool PRIVATE /J)
target_precompile_headers(drako_shader_tool PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/tool_precompiled.h")
target_link_libraries(drako_shader_tool PRIVATE
	deko_spirv_cross glslang glslang-default-resource-limits SPIRV host_zlib host_zstd bcrypt)
# (glslang and SPIRV-Cross recurse deeply on the largest shaders: 64 MiB stacks for every thread, Windows' default
# being 1 MiB; the console's compiler thread has 2 MiB with GCC's leaner frames)
target_link_options(drako_shader_tool PRIVATE /STACK:67108864)
