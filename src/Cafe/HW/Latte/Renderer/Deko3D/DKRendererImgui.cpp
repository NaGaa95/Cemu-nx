#include "DKRendererInternal.h"
#include "platform/switch/SwitchPlatform.h"
#include "imgui/imgui_extension.h"
#include <imgui.h>

using namespace Deko3D;

DKImage* Deko3DRenderer::State::CreateUITexture(const uint8* pixels, uint32 width, uint32 height)
{
	const auto upload = Upload({pixels, static_cast<size_t>(width) * height * 4}, 4);
	auto image = DKImage::Create(width, height, DkImageFormat_RGBA8_Unorm, DkImageFlags_Usage2DEngine);
	if (!image)
		throw std::bad_alloc();
	auto view = image->MakeView();
	const DkImageRect region{0, 0, 0, width, height, 1};
	const DkCopyBuf source{upload.gpu, width * 4, 0};
	dkCmdBufCopyBufferToImage(Cmd(), &source, &view, &region, 0);
	Barrier(DkBarrier_Full, DkInvalidateFlags_Image);
	auto* result = image.release();
	uiTextures.insert(result);
	return result;
}

ImTextureID Deko3DRenderer::GenerateTexture(const std::vector<uint8>& rgb, const Vector2i& size)
{
	if (size.x <= 0 || size.y <= 0 || rgb.size() != static_cast<size_t>(size.x) * size.y * 3)
		return nullptr;
	std::vector<u8> rgba(static_cast<size_t>(size.x) * size.y * 4);
	for (size_t i = 0; i < rgb.size() / 3; ++i)
	{
		std::memcpy(rgba.data() + i * 4, rgb.data() + i * 3, 3);
		rgba[i * 4 + 3] = 255;
	}
	return m_state->CreateUITexture(rgba.data(), size.x, size.y);
}

void Deko3DRenderer::DeleteTexture(ImTextureID id)
{
	auto* image = static_cast<DKImage*>(id);
	if (m_state->uiTextures.erase(image))
		delete image;
}

void Deko3DRenderer::DeleteFontTextures()
{
	if (!m_state || !m_state->fontTexture)
		return;
	DeleteTexture(m_state->fontTexture);
	m_state->fontTexture = nullptr;
	if (imguiFontAtlas)
		imguiFontAtlas->SetTexID(nullptr);
}

bool Deko3DRenderer::ImguiBegin(bool mainWindow)
{
	if (!BeginFrame(mainWindow) || !Renderer::ImguiBegin(mainWindow))
		return false;
	auto& state = *m_state;
	if (!state.fontTexture)
	{
		u8* pixels;
		int width, height;
		imguiFontAtlas->GetTexDataAsRGBA32(&pixels, &width, &height);
		state.fontTexture = state.CreateUITexture(pixels, width, height);
		imguiFontAtlas->SetTexID(state.fontTexture);
	}
	if (!state.imguiVertex)
	{
		state.imguiVertex = std::make_unique<RendererShaderDK>(RendererShader::ShaderType::kVertex, 0, 0, R"(#version 450
layout(location=0) in vec2 position;
layout(location=1) in vec2 uv;
layout(location=2) in vec4 color;
layout(location=0) out vec2 passUV;
layout(location=1) out vec4 passColor;
layout(binding=0, std140) uniform Transform { vec2 scale; vec2 translate; };
void main() { passUV=uv; passColor=color; gl_Position=vec4(position*scale+translate,0,1); }
)",
															   false, false);
		state.imguiFragment = std::make_unique<RendererShaderDK>(RendererShader::ShaderType::kFragment, 0, 0, R"(#version 450
layout(location=0) in vec2 passUV;
layout(location=1) in vec4 passColor;
layout(location=0) out vec4 outputColor;
layout(binding=0) uniform sampler2D source;
void main() { outputColor=passColor*texture(source,passUV); }
)",
																 false, false);
	}
	state.imguiPad = !mainWindow;
	auto& io = ImGui::GetIO();
	io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
	ImGui_UpdateWindowInformation(mainWindow);
	int x, y, width, height;
	SwitchPlatform_GetGamePadViewRegion(!mainWindow, state.display->Width(), state.display->Height(), x, y, width, height);
	if (mainWindow)
	{
		x = y = 0;
		width = state.display->Width();
		height = state.display->Height();
	}
	io.DisplaySize = {static_cast<float>(width), static_cast<float>(height)};
	ImGui::NewFrame();
	return true;
}

void Deko3DRenderer::ImguiEnd()
{
	ImGui::Render();
	const auto* data = ImGui::GetDrawData();
	if (!data || !data->TotalVtxCount || data->DisplaySize.x <= 0 || data->DisplaySize.y <= 0)
		return;
	auto& state = *m_state;
	const auto* vertex = state.imguiVertex->GetProgram();
	const auto* fragment = state.imguiFragment->GetProgram();
	if (!vertex || !fragment)
		return;
	const u32 vertexBytes = data->TotalVtxCount * sizeof(ImDrawVert);
	const u32 indexOffset = AlignUp(vertexBytes, 4u);
	const u32 indexBytes = data->TotalIdxCount * sizeof(ImDrawIdx);
	const u32 uniformOffset = AlignUp(indexOffset + indexBytes, DK_UNIFORM_BUF_ALIGNMENT);
	u32 commandCount = 0;
	for (const auto* list : std::span(data->CmdLists, data->CmdListsCount))
		commandCount += list->CmdBuffer.Size;
	DkSampler sampler;
	dkSamplerDefaults(&sampler);
	sampler.minFilter = sampler.magFilter = DkFilter_Linear;
	for (auto& wrap : sampler.wrapMode)
		wrap = DkWrapMode_ClampToEdge;
	const u32 samplerID = state.Sampler(sampler);
	UploadAllocation allocation;
	while (true)
	{
		const u64 fence = g_dk_command_buffer_mgr->GetCurrentFenceCounter();
		state.descriptorBatch = state.Reserve(*state.descriptors, commandCount * sizeof(DkImageDescriptor), sizeof(DkImageDescriptor));
		state.descriptorCursor = 0;
		allocation = state.Reserve(*state.dynamic, uniformOffset + 16, DK_UNIFORM_BUF_ALIGNMENT);
		if (fence == g_dk_command_buffer_mgr->GetCurrentFenceCounter())
			break;
	}
	u32 vertices = 0, indices = 0;
	for (const auto* list : std::span(data->CmdLists, data->CmdListsCount))
	{
		std::memcpy(static_cast<u8*>(allocation.cpu) + vertices, list->VtxBuffer.Data, list->VtxBuffer.Size * sizeof(ImDrawVert));
		std::memcpy(static_cast<u8*>(allocation.cpu) + indexOffset + indices, list->IdxBuffer.Data, list->IdxBuffer.Size * sizeof(ImDrawIdx));
		vertices += list->VtxBuffer.Size * sizeof(ImDrawVert);
		indices += list->IdxBuffer.Size * sizeof(ImDrawIdx);
	}
	const float transform[]{2.0f / data->DisplaySize.x, 2.0f / data->DisplaySize.y,
							-1.0f - data->DisplayPos.x * (2.0f / data->DisplaySize.x), -1.0f - data->DisplayPos.y * (2.0f / data->DisplaySize.y)};
	std::memcpy(static_cast<u8*>(allocation.cpu) + uniformOffset, transform, sizeof(transform));
	int x, y, width, height;
	SwitchPlatform_GetGamePadViewRegion(state.imguiPad, state.display->Width(), state.display->Height(), x, y, width, height);
	if (!state.imguiPad)
	{
		x = y = 0;
		width = state.display->Width();
		height = state.display->Height();
	}
	auto setup = [&] {
		state.BindDisplay();
		state.BindUtility(vertex, fragment, true);
		const DkViewport viewport{static_cast<float>(x), static_cast<float>(y), static_cast<float>(width), static_cast<float>(height), 0, 1};
		SetViewport(state.Cmd(), viewport);
		std::array<DkVtxAttribState, 3> attributes{};
		attributes[0].offset = offsetof(ImDrawVert, pos);
		attributes[0].size = DkVtxAttribSize_2x32;
		attributes[0].type = DkVtxAttribType_Float;
		attributes[1].offset = offsetof(ImDrawVert, uv);
		attributes[1].size = DkVtxAttribSize_2x32;
		attributes[1].type = DkVtxAttribType_Float;
		attributes[2].offset = offsetof(ImDrawVert, col);
		attributes[2].size = DkVtxAttribSize_4x8;
		attributes[2].type = DkVtxAttribType_Unorm;
		dkCmdBufBindVtxAttribState(state.Cmd(), attributes.data(), attributes.size());
		const DkVtxBufferState buffer{sizeof(ImDrawVert), 0};
		dkCmdBufBindVtxBufferState(state.Cmd(), &buffer, 1);
		const DkBufExtents vertexBuffer{allocation.gpu, vertexBytes};
		state.BindVertexBuffers({&vertexBuffer, 1});
		dkCmdBufBindIdxBuffer(state.Cmd(), sizeof(ImDrawIdx) == 2 ? DkIdxFormat_Uint16 : DkIdxFormat_Uint32, allocation.gpu + indexOffset);
		const DkBufExtents uniforms{allocation.gpu + uniformOffset, 16};
		dkCmdBufBindUniformBuffers(state.Cmd(), DkStage_Vertex, vertex->bindings.uniforms[0], &uniforms, 1);
	};
	setup();
	u32 vertexOffset = 0, firstIndex = 0;
	for (const auto* list : std::span(data->CmdLists, data->CmdListsCount))
	{
		for (const auto& command : list->CmdBuffer)
		{
			if (command.UserCallback)
			{
				if (command.UserCallback != ImDrawCallback_ResetRenderState)
					command.UserCallback(list, &command);
				setup();
				continue;
			}
			const int left = std::clamp(static_cast<int>((command.ClipRect.x - data->DisplayPos.x) * data->FramebufferScale.x), 0, width);
			const int top = std::clamp(static_cast<int>((command.ClipRect.y - data->DisplayPos.y) * data->FramebufferScale.y), 0, height);
			const int right = std::clamp(static_cast<int>((command.ClipRect.z - data->DisplayPos.x) * data->FramebufferScale.x), left, width);
			const int bottom = std::clamp(static_cast<int>((command.ClipRect.w - data->DisplayPos.y) * data->FramebufferScale.y), top, height);
			if (left == right || top == bottom)
				continue;
			auto* image = static_cast<DKImage*>(command.GetTexID());
			if (!state.uiTextures.contains(image))
				continue;
			const DkResHandle handle = state.TextureHandle(image->MakeView(), samplerID);
			state.FlushDescriptors();
			dkCmdBufBindTextures(state.Cmd(), DkStage_Fragment, fragment->bindings.textures[0], &handle, 1);
			const DkScissor scissor{static_cast<u32>(x + left), static_cast<u32>(y + top), static_cast<u32>(right - left), static_cast<u32>(bottom - top)};
			dkCmdBufSetScissors(state.Cmd(), 0, &scissor, 1);
			dkCmdBufDrawIndexed(state.Cmd(), DkPrimitive_Triangles, command.ElemCount, 1,
								firstIndex + command.IdxOffset, vertexOffset + command.VtxOffset, 0);
		}
		vertexOffset += list->VtxBuffer.Size;
		firstIndex += list->IdxBuffer.Size;
	}
	state.descriptorBatch = {};
	state.Invalidate();
}
