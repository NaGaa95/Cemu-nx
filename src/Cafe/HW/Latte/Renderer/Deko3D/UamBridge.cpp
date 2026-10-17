// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "tgsi/tgsi_text.h"
#include "tgsi/tgsi_dump.h"
#include "codegen/nv50_ir_driver.h"
#include "main/mtypes.h"
#include "glsl/program.h"
#include "glsl_frontend.h"
#include "nv_attributes.h"
#include "nv_shader_header.h"
#include "dksh.h"

#define class struct
#include "compiler_iface.h"
#undef class

#include "Cafe/HW/Latte/Renderer/Deko3D/UamBridge.h"

namespace
{
	thread_local FILE* s_uam_stderr = nullptr;

	constexpr uint32_t Align256(uint32_t x)
	{
		return (x + 0xFF) & ~0xFFu;
	}

	constexpr uint32_t SHADER_START_OFFSET = 0x80;
	constexpr uint32_t SPH_OFFSET = SHADER_START_OFFSET - sizeof(NvShaderHeader);

	void* BuildDksh(const DekoCompiler& compiler, size_t* out_size)
	{
		const bool is_compute = compiler.m_stage == pipeline_stage_compute;
		const uint32_t code_base = is_compute ? 0 : SHADER_START_OFFSET;
		const uint32_t data_off = Align256(code_base + compiler.m_codeSize);

		DkshHeader hdr = {};
		hdr.magic = DKSH_MAGIC;
		hdr.header_sz = sizeof(DkshHeader);
		hdr.control_sz = Align256(sizeof(DkshHeader) + sizeof(DkshProgramHeader));
		hdr.code_sz = data_off + Align256(compiler.m_dataSize);
		hdr.programs_off = sizeof(DkshHeader);
		hdr.num_programs = 1;

		const size_t total = hdr.control_sz + hdr.code_sz;
		auto* blob = static_cast<uint8_t*>(std::calloc(1, total));
		if (!blob)
			return nullptr;

		std::memcpy(blob, &hdr, sizeof(hdr));
		std::memcpy(blob + hdr.programs_off, &compiler.m_dkph, sizeof(DkshProgramHeader));

		uint8_t* code = blob + hdr.control_sz;
		if (!is_compute)
			std::memcpy(code + SPH_OFFSET, &compiler.m_nvsh, sizeof(NvShaderHeader));
		std::memcpy(code + code_base, compiler.m_code, compiler.m_codeSize);
		if (compiler.m_dataSize)
			std::memcpy(code + data_off, compiler.m_data, compiler.m_dataSize);

		*out_size = total;
		return blob;
	}

	void ReleaseCompilerOutput(DekoCompiler& compiler)
	{
		std::free(compiler.m_info.bin.code);
		std::free(compiler.m_info.bin.relocData);
		std::free(compiler.m_info.bin.fixupData);
		std::free(compiler.m_info.bin.syms);

		compiler.m_info.bin.code = nullptr;
		compiler.m_info.bin.relocData = nullptr;
		compiler.m_info.bin.fixupData = nullptr;
		compiler.m_info.bin.syms = nullptr;
		compiler.m_code = nullptr;
	}
} // namespace

extern "C" FILE* UamGetStderr()
{
	return s_uam_stderr ? s_uam_stderr : UamGetSystemStderr();
}

void UamFrontendInitOnce()
{
	static bool initialized = false;
	if (!initialized)
	{
		glsl_frontend_init();
		initialized = true;
	}
}

void UamFrontendExitOnce()
{
}

extern "C" void UamCompileShader(gl_context* context, gl_shader* shader, bool dump_ast, bool dump_hir, bool force_recompile)
{
	struct Restore
	{
		bool& flag;
		bool value;
		~Restore() { flag = value; }
	} restore{context->Const.GLSLOptimizeConservatively, context->Const.GLSLOptimizeConservatively};
	context->Const.GLSLOptimizeConservatively = true;
	_mesa_glsl_compile_shader(context, shader, dump_ast, dump_hir, force_recompile);
}

void* UamCompileGlsl(const char* glsl, int stage, size_t* out_size, char** out_log)
{
	static std::mutex mutex;
	std::lock_guard<std::mutex> lock(mutex);
	*out_size = 0;
	*out_log = nullptr;

	char* log_buffer = nullptr;
	size_t log_length = 0;
	FILE* log = open_memstream(&log_buffer, &log_length);
	if (!log)
		return nullptr;

	void* dksh = nullptr;
	s_uam_stderr = log;
	{
		DekoCompiler compiler{static_cast<pipeline_stage>(stage)};
		if (compiler.CompileGlsl(glsl))
			dksh = BuildDksh(compiler, out_size);
		ReleaseCompilerOutput(compiler);
	}
	s_uam_stderr = nullptr;

	std::fclose(log);
	*out_log = log_buffer;

	return dksh;
}
