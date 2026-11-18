// The parts of the emulator around the shader decompiler, as the Switch 2 drako3d core has them while it loads a
// game's transferable shader cache: the Deko3D renderer (NVIDIA, transform feedback through storage buffers),
// accurate shader multiplication (the game profile default, which the Switch launcher's profiles keep), no shader
// dumping and no graphic packs. Everything else the compiled Cemu sources refer to is inert here.

#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/Core/LattePerformanceMonitor.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/GameProfile/GameProfile.h"
#include "Cafe/GraphicPack/GraphicPack2.h"
#include "Cafe/CafeSystem.h"
#include "config/ActiveSettings.h"
#include "tool.h"

ToolCapture g_toolCapture;

namespace
{
	uint64 s_foregroundTitleId = 0;

	class ToolRendererShader final : public RendererShader
	{
	  public:
		ToolRendererShader(ShaderType type, uint64 baseHash, uint64 auxHash, bool isGfxPackSource)
			: RendererShader(type, baseHash, auxHash, baseHash != 0, isGfxPackSource) {}
		void PreponeCompilation(bool) override {}
		bool IsCompiled() override { return true; }
		bool WaitForCompiled() override { return true; }
	};

	// Deko3DRenderer as the shader code sees it (Deko3DRenderer.h/.cpp): its type, vendor and transform feedback mode.
	class ToolRenderer final : public Renderer
	{
	  public:
		ToolRenderer() : Renderer(RendererAPI::Deko3D)
		{
			m_vendor = GfxVendor::Nvidia;
		}
		bool UseTFViaSSBO() const override { return true; }

		RendererShader* shader_create(RendererShader::ShaderType type, uint64 baseHash, uint64 auxHash, const std::string& source, bool compileAsync, bool isGfxPackSource) override
		{
			g_toolCapture.created = true;
			g_toolCapture.type = type;
			g_toolCapture.baseHash = baseHash;
			g_toolCapture.auxHash = auxHash;
			g_toolCapture.source = source;
			return new ToolRendererShader(type, baseHash, auxHash, isGfxPackSource);
		}

		void Initialize() override {}
		void Shutdown() override {}
		bool IsPadWindowActive() override { return false; }
		bool GetVRAMInfo(int& usageInMB, int& totalInMB) const override { return false; }
		void ClearColorbuffer(bool) override {}
		void DrawEmptyFrame(bool) override {}
		void SwapBuffers(bool, bool) override {}
		void DrawBackbufferQuad(LatteTextureView*, RendererOutputShader*, bool, sint32, sint32, sint32, sint32, bool, bool) override {}
		bool BeginFrame(bool) override { return false; }
		void Flush(bool) override {}
		void NotifyLatteCommandProcessorIdle() override {}
		bool ImguiBegin(bool) override { return false; }
		void ImguiEnd() override {}
		ImTextureID GenerateTexture(const std::vector<uint8>&, const Vector2i&) override { return nullptr; }
		void DeleteTexture(ImTextureID) override {}
		void DeleteFontTextures() override {}
		void AppendOverlayDebugInfo() override {}
		void renderTarget_setViewport(float, float, float, float, float, float, bool) override {}
		void renderTarget_setScissor(sint32, sint32, sint32, sint32) override {}
		LatteCachedFBO* rendertarget_createCachedFBO(uint64) override { return nullptr; }
		void rendertarget_deleteCachedFBO(LatteCachedFBO*) override {}
		void rendertarget_bindFramebufferObject(LatteCachedFBO*) override {}
		void* texture_acquireTextureUploadBuffer(uint32) override { return nullptr; }
		void texture_releaseTextureUploadBuffer(uint8*) override {}
		TextureDecoder* texture_chooseDecodedFormat(Latte::E_GX2SURFFMT, bool, Latte::E_DIM, uint32, uint32) override { return nullptr; }
		void texture_clearSlice(LatteTexture*, sint32, sint32) override {}
		void texture_loadSlice(LatteTexture*, sint32, sint32, sint32, void*, sint32, sint32, uint32) override {}
		void texture_clearColorSlice(LatteTexture*, sint32, sint32, float, float, float, float) override {}
		void texture_clearDepthSlice(LatteTexture*, uint32, sint32, bool, bool, float, uint32) override {}
		LatteTexture* texture_createTextureEx(Latte::E_DIM, MPTR, MPTR, Latte::E_GX2SURFFMT, uint32, uint32, uint32, uint32, uint32, uint32, Latte::E_HWTILEMODE, bool) override { return nullptr; }
		void texture_setLatteTexture(LatteTextureView*, uint32) override {}
		void texture_copyImageSubData(LatteTexture*, sint32, sint32, sint32, sint32, LatteTexture*, sint32, sint32, sint32, sint32, sint32, sint32, sint32) override {}
		LatteTextureReadbackInfo* texture_createReadback(LatteTextureView*) override { return nullptr; }
		void surfaceCopy_copySurfaceWithFormatConversion(LatteTexture*, sint32, sint32, LatteTexture*, sint32, sint32, sint32, sint32) override {}
		void bufferCache_init(const sint32) override {}
		void bufferCache_upload(uint8*, sint32, uint32) override {}
		void bufferCache_copy(uint32, uint32, uint32) override {}
		void bufferCache_copyStreamoutToMainBuffer(uint32, uint32, uint32) override {}
		void buffer_bindVertexBuffers(std::span<BindBufferParam>) override {}
		void buffer_bindUniformBuffer(LatteConst::ShaderType, uint32, uint32, uint32) override {}
		void streamout_setupXfbBuffer(uint32, sint32, uint32, uint32) override {}
		void streamout_begin() override {}
		void streamout_rendererFinishDrawcall() override {}
		void draw_beginSequence() override {}
		void draw_execute(uint32, uint32, uint32, uint32, MPTR, Latte::LATTE_VGT_DMA_INDEX_TYPE::E_INDEX_TYPE, const LatteDrawcallContext&) override {}
		void draw_endSequence() override {}
		IndexAllocation indexData_reserveIndexMemory(uint32) override { return {}; }
		void indexData_releaseIndexMemory(IndexAllocation&) override {}
		void indexData_uploadIndexMemory(IndexAllocation&) override {}
		LatteQueryObject* occlusionQuery_create() override { return nullptr; }
		void occlusionQuery_destroy(LatteQueryObject*) override {}
		void occlusionQuery_flush() override {}
		void occlusionQuery_updateState() override {}
	};
} // namespace

void ToolInitEmulatorState(uint64 titleId)
{
	s_foregroundTitleId = titleId;
	g_renderer = std::make_unique<ToolRenderer>();
	// (LatteThread.cpp: the vendor the decompiler sees, from the renderer)
	LatteGPUState.glVendor = GLVENDOR_NVIDIA;
	g_current_game_profile = std::make_unique<GameProfile>();
	ActiveSettings::EnableDumpShaders(false);
}

// ---- globals the compiled sources expect ---------------------------------------------------------------------

std::unique_ptr<Renderer> g_renderer;
std::unique_ptr<GameProfile> g_current_game_profile;
LatteGPUState_t LatteGPUState = {};
performanceMonitor_t performanceMonitor{};
uint64 s_loggingFlagMask = 0;

TitleId CafeSystem::GetForegroundTitleId()
{
	return s_foregroundTitleId;
}

bool cemuLog_log(LogType type, std::string_view text)
{
	return false;
}

bool cemuLog_log(LogType type, std::u8string_view text)
{
	return false;
}

// Renderer.cpp (the base class members the vtable needs)
void Renderer::Initialize() {}
void Renderer::Shutdown() {}
bool Renderer::GetVRAMInfo(int& usageInMB, int& totalInMB) const { return false; }
bool Renderer::ImguiBegin(bool mainWindow) { return false; }

// no graphic packs
const std::string* GraphicPack2::FindCustomShaderSource(uint64 shaderBaseHash, uint64 shaderAuxHash, GP_SHADER_TYPE type, bool isVulkanRenderer, bool isMetalRenderer)
{
	return nullptr;
}

// (the decompiler's performance counters)
uint64 PPCTimer_getRawTsc()
{
	LARGE_INTEGER counter;
	QueryPerformanceCounter(&counter);
	return static_cast<uint64>(counter.QuadPart);
}

// (LatteFetchShader::FindByGPUState reads guest memory while drawing: never during cache loading)
uint8* memory_getPointerFromPhysicalOffset(uint32 physicalOffset)
{
	return nullptr;
}

// (FileCache's writer thread)
void SetThreadName(const char* name)
{
}
