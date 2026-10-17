#pragma once

#include "DKCommon.h"
#include "DKShaderCodePool.h"
#include "Cafe/HW/Latte/Renderer/RendererShader.h"

namespace Deko3D
{
	struct ShaderBindings
	{
		std::array<sint8, 128> uniforms;
		std::array<sint8, 128> textures;
		std::array<sint8, 128> storage;
		sint8 pushConstants = -1;
		ShaderBindings()
		{
			uniforms.fill(-1);
			textures.fill(-1);
			storage.fill(-1);
		}
	};

	struct DKShaderProgram
	{
		std::unique_ptr<DKShaderCode> code;
		DkShader shader{};
		ShaderBindings bindings;
		std::vector<u8> binary;
	};

	class RendererShaderDK final : public RendererShader
	{
	  public:
		RendererShaderDK(ShaderType type, u64 baseHash, u64 auxHash,
						 std::string source, bool compileAsync, bool isGfxPackSource);
		~RendererShaderDK() override;
		void PreponeCompilation(bool isRenderThread) override;
		bool IsCompiled() override;
		bool WaitForCompiled() override;
		const DKShaderProgram* GetProgram();
		static void InitializeCompiler();
		static void ShutdownCompiler();

	  private:
		std::shared_future<std::shared_ptr<DKShaderProgram>> m_job;
		std::shared_ptr<DKShaderProgram> m_program;
	};
} // namespace Deko3D
