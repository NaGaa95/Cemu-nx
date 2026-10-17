#include "DKRendererInternal.h"
#include "DKMemoryTracker.h"
#include "platform/switch/SwitchPlatform.h"

using namespace Deko3D;

LatteCachedFBO* Deko3DRenderer::rendertarget_createCachedFBO(uint64 key)
{
	return new LatteCachedFBO(key);
}
void Deko3DRenderer::rendertarget_deleteCachedFBO(LatteCachedFBO* fbo)
{
	if (m_state->framebuffer == fbo)
		m_state->framebuffer = nullptr;
}

void Deko3DRenderer::rendertarget_bindFramebufferObject(LatteCachedFBO* fbo)
{
	m_state->framebuffer = fbo;
	m_state->dirty = true;
}

void Deko3DRenderer::State::CopyVolumeSlice(LatteTextureViewDK& texture, bool store)
{
	auto volume = texture.Texture()->image->MakeCopyView(texture.firstMip);
	auto slice = texture.renderImage->MakeCopyView(0, 0, texture.numSlice);
	const DkImageRect volumeRect{0, 0, static_cast<u32>(texture.firstSlice), texture.renderImage->Width(), texture.renderImage->Height(), static_cast<u32>(texture.numSlice)};
	const DkImageRect sliceRect{0, 0, 0, volumeRect.width, volumeRect.height, volumeRect.depth};
	Barrier(DkBarrier_Full, DkInvalidateFlags_Image);
	if (store)
		dkCmdBufCopyImage(Cmd(), &slice, &sliceRect, &volume, &volumeRect, 0);
	else
		dkCmdBufCopyImage(Cmd(), &volume, &volumeRect, &slice, &sliceRect, 0);
	Barrier(DkBarrier_Full, DkInvalidateFlags_Image);
	++serial;
}

DkImageView Deko3DRenderer::State::RenderView(LatteTextureViewDK& texture)
{
	auto view = texture.MakeView();
	view.pImage = texture.Texture()->image->MakeCopyView().pImage;
	if (texture.Texture()->Is3DTexture())
	{
		if (!texture.renderImage)
		{
			sint32 width, height;
			texture.baseTexture->GetEffectiveSize(width, height, texture.firstMip);
			texture.renderImage = DKImage::Create(width, height,
												  view.format != DkImageFormat_None ? view.format : texture.Texture()->hostFormat.format,
												  DkImageFlags_UsageRender,
												  texture.numSlice > 1 ? DkImageType_2DArray : DkImageType_2D, texture.numSlice);
			if (!texture.renderImage)
				throw std::bad_alloc();
		}
		CopyVolumeSlice(texture, false);
		view = texture.renderImage->MakeView(0, 0, texture.numSlice);
	}
	view.type = texture.numSlice > 1 ? DkImageType_2DArray : DkImageType_2D;
	view.mipLevelCount = 1;
	return view;
}

void Deko3DRenderer::State::StoreFramebuffer()
{
	for (auto* texture : volumeTargets)
		CopyVolumeSlice(*texture, true);
}

void Deko3DRenderer::State::BindFramebuffer()
{
	std::array<DkImageView, 8> views{};
	std::array<const DkImageView*, 8> targets{};
	DkImageView depth{};
	const DkImageView* depthTarget = nullptr;
	u32 count = 0;
	colorTargetMask = 0;
	volumeTargets.clear();
	if (framebuffer)
	{
		for (u32 i = 0; i < targets.size(); ++i)
		{
			if (auto* texture = static_cast<LatteTextureViewDK*>(framebuffer->colorBuffer[i].texture))
			{
				TextureBarrier(*texture->Texture(), true);
				views[i] = RenderView(*texture);
				if (texture->Texture()->Is3DTexture())
					volumeTargets.push_back(texture);
				targets[i] = &views[i];
				colorTargetMask |= 0xfu << (i * 4);
				count = i + 1;
			}
		}
		if (auto* texture = static_cast<LatteTextureViewDK*>(framebuffer->depthBuffer.texture))
		{
			TextureBarrier(*texture->Texture(), true);
			depth = RenderView(*texture);
			if (texture->Texture()->Is3DTexture())
				volumeTargets.push_back(texture);
			depthTarget = &depth;
		}
	}
	for (u32 i = 0; i < count; ++i)
		if (!targets[i])
			targets[i] = targets[count - 1];
	std::array<DkImageView, 9> key{};
	std::copy(views.begin(), views.end(), key.begin());
	key.back() = depth;
	if (Changed(StateSlot::Framebuffer, key))
		dkCmdBufBindRenderTargets(Cmd(), targets.data(), count, depthTarget);
}

void* Deko3DRenderer::texture_acquireTextureUploadBuffer(uint32 size)
{
	UploadAllocation allocation;
	if (size <= m_state->uploads->GetCurrentSize() - DK_MEMBLOCK_ALIGNMENT)
		allocation = m_state->Reserve(*m_state->uploads, size, DK_MEMBLOCK_ALIGNMENT);
	else
	{
		const u32 bytes = AlignUp(size, DK_MEMBLOCK_ALIGNMENT);
		auto block = dk::MemBlockMaker{g_dk_context->GetDevice(), bytes}
						 .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
						 .create();
		if (!block)
			throw std::bad_alloc();
		allocation = {block.getCpuAddr(), block.getGpuAddr(), size};
		DeferMemBlockDestruction(std::move(block));
	}
	m_state->textureUploads.emplace(allocation.cpu, allocation);
	return allocation.cpu;
}

void Deko3DRenderer::texture_releaseTextureUploadBuffer(uint8* memory)
{
	m_state->textureUploads.erase(memory);
}

TextureDecoder* Deko3DRenderer::texture_chooseDecodedFormat(Latte::E_GX2SURFFMT format,
															bool depth, Latte::E_DIM dim, uint32 width, uint32 height)
{
	FormatInfo info;
	GetTextureFormatInfo(format, depth, dim, width, height, &info);
	return info.decoder;
}

LatteTexture* Deko3DRenderer::texture_createTextureEx(Latte::E_DIM dim, MPTR address, MPTR mipAddress,
													  Latte::E_GX2SURFFMT format, uint32 width, uint32 height, uint32 depth, uint32 pitch,
													  uint32 mipLevels, uint32 swizzle, Latte::E_HWTILEMODE tile, bool isDepth)
{
	return new LatteTextureDK(dim, address, mipAddress, format, width, height, depth, pitch,
							  mipLevels, swizzle, tile, isDepth);
}

void Deko3DRenderer::texture_setLatteTexture(LatteTextureView* view, uint32 unit)
{
	if (unit < m_state->textures.size())
		m_state->textures[unit] = static_cast<LatteTextureViewDK*>(view);
}

void Deko3DRenderer::texture_loadSlice(LatteTexture* base, sint32 width, sint32 height, sint32 depth,
									   void* pixels, sint32 slice, sint32 mip, uint32 compressedSize)
{
	auto& texture = *static_cast<LatteTextureDK*>(base);
	texture.AllocateOnHost();
	const auto found = m_state->textureUploads.find(pixels);
	if (found == m_state->textureUploads.end())
		throw std::runtime_error("Deko3D texture upload has no allocation");
	const bool compressed = IsCompressed(texture.hostFormat.format);
	const u32 rowBytes = (compressed ? (width + 3) / 4 : width) * FormatBytes(texture.hostFormat.format);
	const u64 required = static_cast<u64>(rowBytes) * (compressed ? (height + 3) / 4 : height);
	if (required > found->second.size)
		throw std::runtime_error("Deko3D texture upload exceeds its allocation");
	if (texture.hostFormat.format == DkImageFormat_Z24S8)
		for (auto& texel : std::span(static_cast<u32*>(pixels), static_cast<size_t>(width) * height))
			texel = std::rotl(texel, 8);
	m_state->TextureBarrier(texture, true, DkBarrier_Full);
	auto view = texture.image->MakeCopyView(mip, texture.Is3DTexture() ? 0 : slice);
	const DkImageRect region{0, 0, texture.Is3DTexture() ? static_cast<u32>(slice) : 0,
							 static_cast<u32>(width), static_cast<u32>(height), 1};
	const DkCopyBuf source{found->second.gpu, rowBytes, 0};
	dkCmdBufCopyBufferToImage(m_state->Cmd(), &source, &view, &region, 0);
	m_state->Barrier(DkBarrier_Full, DkInvalidateFlags_Image);
	++m_state->serial;
	m_state->dirty = true;
}

void Deko3DRenderer::texture_clearSlice(LatteTexture* texture, sint32 slice, sint32 mip)
{
	if (texture->isDepth)
		texture_clearDepthSlice(texture, slice, mip, true, true, 0.0f, 0);
	else
		texture_clearColorSlice(texture, slice, mip, 0, 0, 0, 0);
}

void Deko3DRenderer::texture_clearColorSlice(LatteTexture* base, sint32 slice, sint32 mip,
											 float r, float g, float b, float a)
{
	auto& texture = *static_cast<LatteTextureDK*>(base);
	texture.AllocateOnHost();
	if (IsCompressed(texture.hostFormat.format))
		return;
	m_state->TextureBarrier(texture, true);
	auto view = texture.image->MakeCopyView(mip, slice);
	view.type = DkImageType_2D;
	LatteTextureViewDK* volume = nullptr;
	if (texture.Is3DTexture())
	{
		volume = static_cast<LatteTextureViewDK*>(texture.GetOrCreateView(Latte::E_DIM::DIM_2D, texture.format, mip, 1, slice, 1));
		view = m_state->RenderView(*volume);
	}
	const DkImageView* target = &view;
	sint32 width, height;
	texture.GetEffectiveSize(width, height, mip);
	const DkScissor scissor{0, 0, static_cast<u32>(width), static_cast<u32>(height)};
	dkCmdBufBindRenderTargets(m_state->Cmd(), &target, 1, nullptr);
	dkCmdBufSetScissors(m_state->Cmd(), 0, &scissor, 1);
	const float color[4]{r, g, b, a};
	dkCmdBufClearColor(m_state->Cmd(), 0, DkColorMask_RGBA, color);
	if (volume)
		m_state->CopyVolumeSlice(*volume, true);
	m_state->Invalidate();
}

void Deko3DRenderer::texture_clearDepthSlice(LatteTexture* base, uint32 slice, sint32 mip,
											 bool clearDepth, bool clearStencil, float depth, uint32 stencil)
{
	auto& texture = *static_cast<LatteTextureDK*>(base);
	texture.AllocateOnHost();
	m_state->TextureBarrier(texture, true);
	auto view = texture.image->MakeCopyView(mip, slice);
	view.type = DkImageType_2D;
	LatteTextureViewDK* volume = nullptr;
	if (texture.Is3DTexture())
	{
		volume = static_cast<LatteTextureViewDK*>(texture.GetOrCreateView(Latte::E_DIM::DIM_2D, texture.format, mip, 1, slice, 1));
		view = m_state->RenderView(*volume);
	}
	sint32 width, height;
	texture.GetEffectiveSize(width, height, mip);
	const DkScissor scissor{0, 0, static_cast<u32>(width), static_cast<u32>(height)};
	dkCmdBufBindRenderTargets(m_state->Cmd(), nullptr, 0, &view);
	dkCmdBufSetScissors(m_state->Cmd(), 0, &scissor, 1);
	dkCmdBufClearDepthStencil(m_state->Cmd(), clearDepth, depth, clearStencil && texture.hasStencil ? 0xff : 0, stencil);
	if (volume)
		m_state->CopyVolumeSlice(*volume, true);
	m_state->Invalidate();
}

void Deko3DRenderer::texture_copyImageSubData(LatteTexture* source, sint32 srcMip, sint32 srcX,
											  sint32 srcY, sint32 srcSlice, LatteTexture* destination, sint32 dstMip, sint32 dstX,
											  sint32 dstY, sint32 dstSlice, sint32 width, sint32 height, sint32 depth)
{
	auto& src = *static_cast<LatteTextureDK*>(source);
	auto& dst = *static_cast<LatteTextureDK*>(destination);
	src.AllocateOnHost();
	dst.AllocateOnHost();
	auto srcView = src.image->MakeCopyView(srcMip, src.Is3DTexture() ? 0 : srcSlice, src.Is3DTexture() ? 1 : depth);
	auto dstView = dst.image->MakeCopyView(dstMip, dst.Is3DTexture() ? 0 : dstSlice, dst.Is3DTexture() ? 1 : depth);
	const DkImageRect srcRect{static_cast<u32>(srcX), static_cast<u32>(srcY),
							  src.Is3DTexture() ? static_cast<u32>(srcSlice) : 0,
							  static_cast<u32>(width), static_cast<u32>(height), static_cast<u32>(depth)};
	DkImageRect dstRect{static_cast<u32>(dstX), static_cast<u32>(dstY),
							  dst.Is3DTexture() ? static_cast<u32>(dstSlice) : 0,
							  static_cast<u32>(width), static_cast<u32>(height), static_cast<u32>(depth)};
	const bool srcCompressed = IsCompressed(src.hostFormat.format);
	const bool dstCompressed = IsCompressed(dst.hostFormat.format);
	if (srcCompressed != dstCompressed)
	{
		const u32 blockBytes = FormatBytes(src.hostFormat.format);
		if (blockBytes != FormatBytes(dst.hostFormat.format))
			throw std::runtime_error("Deko3D texture copy has incompatible block sizes");
		const u32 columns = srcCompressed ? (srcRect.width + 3) / 4 : srcRect.width;
		const u32 rows = srcCompressed ? (srcRect.height + 3) / 4 : srcRect.height;
		sint32 dstWidth, dstHeight;
		dst.GetEffectiveSize(dstWidth, dstHeight, dstMip);
		const u32 dstBlockSize = dstCompressed ? 4 : 1;
		if (dstX < 0 || dstY < 0 || dstX >= dstWidth || dstY >= dstHeight ||
			columns > (static_cast<u32>(dstWidth - dstX) + dstBlockSize - 1) / dstBlockSize ||
			rows > (static_cast<u32>(dstHeight - dstY) + dstBlockSize - 1) / dstBlockSize)
			throw std::runtime_error("Deko3D texture copy exceeds its destination");
		dstRect.width = std::min(columns * dstBlockSize, static_cast<u32>(dstWidth - dstX));
		dstRect.height = std::min(rows * dstBlockSize, static_cast<u32>(dstHeight - dstY));
		const u64 rowBytes = static_cast<u64>(columns) * blockBytes;
		const u64 sliceBytes = rowBytes * rows;
		const u64 bytes = sliceBytes * srcRect.depth;
		if (bytes > std::numeric_limits<u32>::max())
			throw std::bad_alloc();
		const auto allocation = m_state->Allocate(static_cast<u32>(bytes), DK_MEMBLOCK_ALIGNMENT);
		const DkCopyBuf buffer{allocation.gpu, static_cast<u32>(rowBytes), static_cast<u32>(sliceBytes)};
		m_state->TextureBarrier(src, false, DkBarrier_Full);
		m_state->TextureBarrier(dst, true, DkBarrier_Full);
		dkCmdBufCopyImageToBuffer(m_state->Cmd(), &srcView, &srcRect, &buffer, 0);
		m_state->Barrier(DkBarrier_Full, 0);
		dkCmdBufCopyBufferToImage(m_state->Cmd(), &buffer, &dstView, &dstRect, 0);
	}
	else
	{
		m_state->TextureBarrier(src, false, DkBarrier_Full);
		m_state->TextureBarrier(dst, true, DkBarrier_Full);
		dkCmdBufCopyImage(m_state->Cmd(), &srcView, &srcRect, &dstView, &dstRect, 0);
	}
	m_state->Barrier(DkBarrier_Full, DkInvalidateFlags_Image);
	++m_state->serial;
	m_state->dirty = true;
}

class Deko3DRenderer::State::Readback final : public LatteTextureReadbackInfo
{
  public:
	Readback(State& state, LatteTextureViewDK* view) : LatteTextureReadbackInfo(view, view->firstMip), m_state(state)
	{
		m_rowPitch = GetReadbackRowPitch(view);
		m_image_size = GetReadbackImageSize(view, m_rowPitch);
		m_memory = dk::MemBlockMaker{g_dk_context->GetDevice(), AlignUp(m_image_size, DK_MEMBLOCK_ALIGNMENT)}
					   .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
					   .create();
		if (!m_memory)
			throw std::bad_alloc();
		MemoryTracker::RegisterMemBlock(m_memory, "readback");
	}
	~Readback() override
	{
		MemoryTracker::UnregisterMemBlock(m_memory);
		DeferMemBlockDestruction(std::move(m_memory));
	}
	void StartTransfer() override
	{
		auto& texture = *static_cast<LatteTextureDK*>(m_textureView->baseTexture);
		m_state.TextureBarrier(texture, false, DkBarrier_Full);
		auto view = texture.image->MakeCopyView(m_firstMip, m_firstSlice);
		const DkImageRect region{0, 0, texture.Is3DTexture() ? static_cast<u32>(m_firstSlice) : 0,
								 static_cast<u32>(texture.GetMipWidth(m_firstMip)), static_cast<u32>(texture.GetMipHeight(m_firstMip)), 1};
		const DkCopyBuf target{m_memory.getGpuAddr(), m_rowPitch, 0};
		dkCmdBufCopyImageToBuffer(m_state.Cmd(), &view, &region, &target, 0);
		m_state.Barrier(DkBarrier_Full, DkInvalidateFlags_Image);
		++m_state.serial;
		g_dk_command_buffer_mgr->NotifyCpuReadback();
		m_fence = g_dk_command_buffer_mgr->GetCurrentFenceCounter();
		m_state.Submit(false);
		m_textureView = nullptr;
	}
	bool IsFinished() override
	{
		if (g_dk_command_buffer_mgr->GetCompletedFenceCounter() < m_fence)
			g_dk_command_buffer_mgr->PollCompletedFences();
		return m_fence && g_dk_command_buffer_mgr->GetCompletedFenceCounter() >= m_fence;
	}
	void ForceFinish() override
	{
		g_dk_command_buffer_mgr->WaitForFenceCounter(m_fence);
	}
	uint8* GetData() override
	{
		if (!m_converted && hostTextureCopy.isDepth && hostTextureCopy.format == Latte::E_GX2SURFFMT::D24_S8_UNORM)
		{
			for (auto& texel : std::span(static_cast<u32*>(m_memory.getCpuAddr()), m_image_size / 4))
				texel = std::rotr(texel, 8);
			m_converted = true;
		}
		return static_cast<uint8*>(m_memory.getCpuAddr());
	}

  private:
	State& m_state;
	dk::UniqueMemBlock m_memory;
	u64 m_fence = 0;
	bool m_converted = false;
};

LatteTextureReadbackInfo* Deko3DRenderer::texture_createReadback(LatteTextureView* view)
{
	auto* texture = static_cast<LatteTextureDK*>(view->baseTexture);
	if (texture->hostFormat.isAlternateFormat || texture->IsCompressedFormat())
		return nullptr;
	return new State::Readback(*m_state, static_cast<LatteTextureViewDK*>(view));
}

void Deko3DRenderer::HandleScreenshotRequest(LatteTextureView* base, bool padView)
{
	if (!m_screenshot_requested || padView != SwitchPlatform_IsGamePadOnly())
		return;
	auto* texture = static_cast<LatteTextureViewDK*>(base);
	auto readback = std::unique_ptr<LatteTextureReadbackInfo>(texture_createReadback(texture));
	if (!readback || (texture->Texture()->hostFormat.format != DkImageFormat_RGBA8_Unorm &&
					  texture->Texture()->hostFormat.format != DkImageFormat_RGBA8_Unorm_sRGB))
		return;
	readback->StartTransfer();
	readback->ForceFinish();
	const u32 width = texture->baseTexture->GetMipWidth(texture->firstMip);
	const u32 height = texture->baseTexture->GetMipHeight(texture->firstMip);
	std::vector<u8> rgb(width * height * 3);
	const u8* pixels = readback->GetData();
	for (u32 y = 0; y < height; ++y)
		for (u32 x = 0; x < width; ++x)
			for (u32 c = 0; c < 3; ++c)
				rgb[(y * width + x) * 3 + c] = pixels[y * readback->m_rowPitch + x * 4 + c];
	SaveScreenshot(rgb, width, height, !padView);
}
