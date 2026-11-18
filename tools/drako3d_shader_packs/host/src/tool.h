// Shared declarations of the host tool (see ../../README.md).
#pragma once

#include "Cafe/HW/Latte/Renderer/RendererShader.h"

// What the renderer received from LatteShader_CreateRendererShader for the record being loaded (cemu_stubs.cpp).
struct ToolCapture
{
	bool created = false;
	RendererShader::ShaderType type = RendererShader::ShaderType::kVertex;
	uint64 baseHash = 0;
	uint64 auxHash = 0;
	std::string source;
};
extern ToolCapture g_toolCapture;

// Emulator state as the Switch 2 drako3d core has it while it loads a game's transferable cache (cemu_stubs.cpp).
void ToolInitEmulatorState(uint64 titleId);

// gen_shader_cache.cpp: LatteShaderCache.cpp's loader, extracted at build time.
extern const sint32 g_toolCompileQueueSize;
extern const uint32 g_toolGenericExtraVersion;
uint32 LatteShaderCache_getShaderCacheExtraVersion(uint64 titleId);
bool LatteShaderCache_readSeparableShader(uint8* shaderInfoData, sint32 shaderInfoSize);
void LatteShaderCache_initCompileQueue();
void LatteShaderCache_updateCompileQueue(sint32 maxRemainingEntries);

// gen_dkshader.cpp: DKShader.cpp's key, translation, validation and record layout, extracted at build time.
namespace Deko3D
{
	uint64 ToolSourceHash(std::string_view source, RendererShader::ShaderType type);
	int ToolUamStage(RendererShader::ShaderType type);
	uint32 ToolCacheVersion();
	size_t ToolBindingsSize();
	bool ToolTranslate(RendererShader::ShaderType type, const std::string& source, std::string& glsl,
					   std::vector<uint8>& bindings, std::string& error);
	bool ToolBuildRecord(uint64 hash, int stage, const std::vector<uint8>& bindings, const std::vector<uint8>& binary,
						 std::vector<uint8>& record, std::string& error);
} // namespace Deko3D
