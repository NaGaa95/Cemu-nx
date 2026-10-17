// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DKFrameGenerator.h"
#include "platform/switch/SwitchLSFGDeko.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <deko3d.hpp>
#include <fmt/format.h>

#include <lsfg/backend/binding.hpp>
#include <lsfg/backend/cache_load.hpp>
#include <lsfg/backend/layout.hpp>
#include <lsfg/backend/schedule.hpp>
#include <lsfg/common/cache_format.hpp>
#include <lsfg/common/cache_store.hpp>
#include <lsfg/common/dksh.hpp>
#include <lsfg/common/error.hpp>
#include <lsfg/common/image_graph.hpp>

#include "Cafe/HW/Latte/Renderer/Deko3D/DKCommandBufferManager.h"
#include "Cafe/HW/Latte/Renderer/Deko3D/DKContext.h"

#include "Cafe/HW/Latte/Renderer/Deko3D/DKMemoryTracker.h"

namespace Deko3D
{
	namespace
	{
		namespace backend = lsfg::backend;
		namespace graph = lsfg::graph;

		constexpr u32 DESCRIPTOR_SIZE = sizeof(DkImageDescriptor);
		static_assert(sizeof(DkImageDescriptor) == sizeof(DkSamplerDescriptor));
		static_assert(backend::uniform_buffer_stride % DK_UNIFORM_BUF_ALIGNMENT == 0);
		static_assert(backend::max_texture_slots <= DK_NUM_TEXTURE_BINDINGS);
		static_assert(backend::max_storage_slots <= DK_NUM_IMAGE_BINDINGS);

		constexpr std::chrono::nanoseconds DISPLAY_INTERVAL = std::chrono::nanoseconds(1'000'000'000 / 60);

		constexpr u64 MEMORY_BUDGET = 128 * 1024 * 1024;

		constexpr u32 NO_IMAGE = ~0u;

		DkImageFormat GetImageFormat(graph::Format format)
		{
			switch (format)
			{
			case graph::Format::rgba8:
				return DkImageFormat_RGBA8_Unorm;
			case graph::Format::r8:
				return DkImageFormat_R8_Unorm;
			case graph::Format::rgba16f:
				return DkImageFormat_RGBA16_Float;
			}
			return DkImageFormat_None;
		}

		void DescribeSampler(u32 index, DkSampler* sampler)
		{
			dkSamplerDefaults(sampler);
			sampler->minFilter = DkFilter_Linear;
			sampler->magFilter = DkFilter_Linear;
			sampler->mipFilter = DkMipFilter_Linear;

			const bool clamp_to_edge =
				index == static_cast<u32>(graph::Sampler::edge) || index == backend::introduced_sampler;
			for (DkWrapMode& mode : sampler->wrapMode)
				mode = clamp_to_edge ? DkWrapMode_ClampToEdge : DkWrapMode_ClampToBorder;

			if (index == static_cast<u32>(graph::Sampler::border_white))
			{
				for (auto& channel : sampler->borderColor)
					channel.value_f = 1.0f;
			}
		}

		dk::UniqueMemBlock CreateBlock(u64 size, u32 flags, std::string label)
		{
			const u64 rounded = AlignUp(size, DK_MEMBLOCK_ALIGNMENT);
			if (rounded == 0 || rounded > backend::max_memory_block_bytes)
				return {};

			dk::UniqueMemBlock block = dk::MemBlockMaker{g_dk_context->GetDevice(), static_cast<u32>(rounded)}
										   .setFlags(flags)
										   .create();
			if (block)
				MemoryTracker::RegisterMemBlock(block, std::move(label));
			return std::move(block);
		}

		void ReleaseBlock(dk::UniqueMemBlock& block)
		{
			if (!block)
				return;
			MemoryTracker::UnregisterMemBlock(block);
			DeferMemBlockDestruction(std::move(block));
		}

		std::unique_ptr<DKImage> CreateFrameTexture(u32 width, u32 height, bool storage,
													std::string_view)
		{
			return DKImage::Create(width, height, DkImageFormat_RGBA8_Unorm,
								   DkImageFlags_Usage2DEngine | (storage ? DkImageFlags_UsageLoadStore : 0));
		}

		class DKFrameGenerator final : public FrameGenerator
		{
		  public:
			explicit DKFrameGenerator(const FrameGenerationConfig& config);
			~DKFrameGenerator() override;

			DKFrameGenerator(const DKFrameGenerator&) = delete;
			DKFrameGenerator& operator=(const DKFrameGenerator&) = delete;

			bool Initialize();

			bool Generate(const DKImage* frame, const FrameRect& rect) override;
			const DKImage* GetGeneratedFrame(u32 index) const override;
			u32 GetGeneratedFrameCount() const override
			{
				return m_graph_config.generated_frames;
			}
			std::chrono::nanoseconds GetDisplayInterval() const override
			{
				return DISPLAY_INTERVAL;
			}
			void Reset() override
			{
				m_frames_since_reset = 0;
			}

		  private:
			bool LoadChain();
			bool LoadModules();

			bool Allocate(u32 width, u32 height);
			void ReleaseAllocation();
			bool AllocateImages();
			void WriteDescriptors();
			void WriteUniformBuffers();

			const DkImage* GetChainImage(u32 index) const;

			bool Record(DkCmdBuf cmdbuf, u32 dispatch, u32 phase);
			void Barrier(DkCmdBuf cmdbuf, u32 invalidate);
			void MarkImage(u32 image, u8 hazard);

			void Fail(std::string_view reason);

			static constexpr u8 HAZARD_READ = 1 << 0;
			static constexpr u8 HAZARD_WRITTEN = 1 << 1;

			graph::Config m_graph_config;
			lsfg::cache::Loaded m_cache;
			backend::Schedule m_schedule;

			dk::UniqueMemBlock m_code_memory;
			std::vector<DkShader> m_shaders;

			u32 m_width = 0;
			u32 m_height = 0;
			backend::Plan m_plan;
			backend::DescriptorLayout m_descriptors;
			dk::UniqueMemBlock m_image_memory;
			dk::UniqueMemBlock m_descriptor_memory;
			dk::UniqueMemBlock m_uniform_memory;
			std::vector<dk::Image> m_images;
			std::vector<bool> m_owned;
			std::array<std::unique_ptr<DKImage>, graph::history_image_count> m_history;
			std::vector<std::unique_ptr<DKImage>> m_generated;

			std::vector<u8> m_hazards;
			std::vector<u32> m_hazard_images;

			u32 m_frame = 0;
			u32 m_frames_since_reset = 0;
			bool m_failed = false;
		};

		DKFrameGenerator::DKFrameGenerator(const FrameGenerationConfig& config)
			: m_graph_config{
				  .performance = config.performance,
				  .generated_frames = config.multiplier - 1,
				  .flow_numerator = 1,
				  .flow_denominator = config.flow_scale,
			  }
		{
		}

		DKFrameGenerator::~DKFrameGenerator()
		{
			ReleaseAllocation();
			ReleaseBlock(m_code_memory);
		}

		void DKFrameGenerator::Fail(std::string_view reason)
		{
			SwitchLSFGDeko_SetAvailable(false);
			cemuLog_log(LogType::Force, "Frame generation: {}", reason);
			m_failed = true;
		}

		bool DKFrameGenerator::Initialize()
		{
			return LoadChain() && LoadModules();
		}

		bool DKFrameGenerator::LoadChain()
		{
			const std::string directory = FrameGeneration::GetCacheDirectory(m_graph_config.performance);
			if (const lsfg::ErrorCode code = lsfg::cache::read(directory, m_cache); !lsfg::succeeded(code))
			{
				Fail(code == lsfg::ErrorCode::cache_missing ? "its shaders are missing. Relaunch the game to rebuild them." : fmt::format("its shaders could not be read ({}). Relaunch the game to rebuild them.", lsfg::error_name(code)));
				return false;
			}

			if (const lsfg::ErrorCode code = graph::build(m_graph_config, m_cache.graph);
				!lsfg::succeeded(code))
			{
				Fail(fmt::format("this configuration has no chain ({})", lsfg::error_name(code)));
				return false;
			}
			lsfg::cache::describe(m_cache.header, m_cache.graph);

			std::vector<lsfg::cache::PassEntry> passes;
			std::vector<lsfg::cache::SlotEntry> slots;
			for (const lsfg::cache::LoadedPass& pass : m_cache.passes)
			{
				lsfg::cache::PassEntry entry = pass.entry;
				entry.slot_first = static_cast<u32>(slots.size());
				passes.push_back(entry);
				slots.insert(slots.end(), pass.slots.begin(), pass.slots.end());
			}

			if (const lsfg::ErrorCode code =
					lsfg::cache::validate(m_cache.header, passes, slots, m_cache.graph);
				!lsfg::succeeded(code))
			{
				Fail(fmt::format("the prepared shaders do not fit this configuration ({}). Relaunch the game "
								 "to rebuild them.",
								 lsfg::error_name(code)));
				return false;
			}

			if (const lsfg::ErrorCode code = backend::schedule(m_cache.graph, m_schedule);
				!lsfg::succeeded(code))
			{
				Fail(fmt::format("the chain cannot be scheduled ({})", lsfg::error_name(code)));
				return false;
			}

			return true;
		}

		bool DKFrameGenerator::LoadModules()
		{
			std::vector<u32> offsets;
			offsets.reserve(m_cache.passes.size());

			backend::Arena arena;
			for (const lsfg::cache::LoadedPass& pass : m_cache.passes)
			{
				lsfg::dksh::FileHeader header;
				std::memcpy(&header, pass.dksh.data(), sizeof(header));
				offsets.push_back(arena.place(header.code_size, DK_SHADER_CODE_ALIGNMENT));
			}
			static_cast<void>(arena.place(DK_SHADER_CODE_UNUSABLE_SIZE, 1));

			if (arena.overflowed())
			{
				Fail("its shaders do not fit in one code block");
				return false;
			}

			m_code_memory =
				CreateBlock(arena.block_size(),
							DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code,
							"frame generation shader code");
			if (!m_code_memory)
			{
				Fail("there is not enough memory for its shaders");
				return false;
			}

			u8* code = static_cast<u8*>(m_code_memory.getCpuAddr());
			m_shaders.assign(m_cache.passes.size(), DkShader{});
			for (size_t i = 0; i < m_cache.passes.size(); ++i)
			{
				const lsfg::cache::LoadedPass& pass = m_cache.passes[i];
				lsfg::dksh::FileHeader header;
				std::memcpy(&header, pass.dksh.data(), sizeof(header));
				std::memcpy(code + offsets[i], pass.dksh.data() + header.control_size, header.code_size);

				DkShaderMaker maker;
				dkShaderMakerDefaults(&maker, m_code_memory, offsets[i]);
				maker.control = pass.dksh.data();
				dkShaderInitialize(&m_shaders[i], &maker);
				if (!dkShaderIsValid(&m_shaders[i]))
				{
					Fail("one of its shaders would not load");
					return false;
				}
			}

			return true;
		}

		bool DKFrameGenerator::Allocate(u32 width, u32 height)
		{
			ReleaseAllocation();

			const backend::Request request{
				.config = m_graph_config,
				.precision = lsfg::cache::Precision::high,
				.output = {width, height},
				.memory_budget_bytes = MEMORY_BUDGET,
			};
			backend::Rejection why;
			if (!backend::accept(m_cache, request, m_plan, why))
			{
				Fail(why.code == lsfg::ErrorCode::out_of_memory ? fmt::format("a {}x{} frame needs more memory than it is allowed. Lower the internal "
																			  "resolution or the flow scale.",
																			  width, height)
																: fmt::format("the chain cannot run at {}x{} ({})", width, height, why.reason));
				return false;
			}

			if (m_plan.max_scratch_bytes_per_warp > PER_WARP_SCRATCH_MEMORY_SIZE)
			{
				Fail(fmt::format("its shaders need {} bytes of scratch per warp and the queue has {}",
								 m_plan.max_scratch_bytes_per_warp, PER_WARP_SCRATCH_MEMORY_SIZE));
				return false;
			}

			if (const lsfg::ErrorCode code = backend::describe(m_cache.graph, m_descriptors);
				!lsfg::succeeded(code))
			{
				Fail(fmt::format("its descriptors cannot be laid out ({})", lsfg::error_name(code)));
				return false;
			}

			for (u32 dispatch = 0; dispatch < m_cache.graph.dispatches.size(); ++dispatch)
			{
				for (u32 phase = 0; phase < m_cache.graph.dispatches[dispatch].variant_count; ++phase)
				{
					backend::DispatchBinding binding;
					if (const lsfg::ErrorCode code =
							backend::bind(m_cache, m_plan, m_descriptors, dispatch, phase, binding);
						!lsfg::succeeded(code))
					{
						Fail(fmt::format("dispatch {} cannot be bound ({})", dispatch, lsfg::error_name(code)));
						return false;
					}
				}
			}

			if (!AllocateImages())
				return false;

			m_descriptor_memory = CreateBlock(
				static_cast<u64>(m_descriptors.image_descriptors + backend::sampler_descriptor_count) *
					DESCRIPTOR_SIZE,
				DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, "frame generation descriptors");
			if (m_plan.uniform_buffers != 0)
			{
				m_uniform_memory =
					CreateBlock(static_cast<u64>(m_plan.uniform_buffers) * backend::uniform_buffer_stride,
								DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached,
								"frame generation uniform buffers");
			}
			if (!m_descriptor_memory || (m_plan.uniform_buffers != 0 && !m_uniform_memory))
			{
				Fail("there is not enough memory for its descriptors");
				return false;
			}

			WriteDescriptors();
			WriteUniformBuffers();

			m_hazards.assign(m_cache.graph.images.size(), 0);
			m_hazard_images.clear();
			m_hazard_images.reserve(m_cache.graph.images.size());

			m_width = width;
			m_height = height;
			m_frames_since_reset = 0;

			return true;
		}

		bool DKFrameGenerator::AllocateImages()
		{
			const size_t image_count = m_plan.images.size();
			std::vector<dk::ImageLayout> layouts(image_count);
			std::vector<u32> offsets(image_count, 0);
			m_images.assign(image_count, dk::Image{});
			m_owned.assign(image_count, false);

			u32 generated = 0;
			backend::Arena arena;
			for (u32 i = 0; i < image_count; ++i)
			{
				const backend::ImagePlan& image = m_plan.images[i];
				switch (image.role)
				{
				case graph::ImageRole::history:
					m_history[i] = CreateFrameTexture(image.extent.width, image.extent.height, false,
													  fmt::format("history {}", i));
					if (!m_history[i])
					{
						Fail("there is not enough memory for its frames");
						return false;
					}
					continue;
				case graph::ImageRole::generated:
					m_generated.push_back(CreateFrameTexture(image.extent.width, image.extent.height, true,
															 fmt::format("output {}", generated++)));
					if (!m_generated.back())
					{
						Fail("there is not enough memory for its frames");
						return false;
					}
					continue;
				default:
					break;
				}

				dk::ImageLayoutMaker{g_dk_context->GetDevice()}
					.setFlags(DkImageFlags_UsageLoadStore)
					.setFormat(GetImageFormat(image.format))
					.setDimensions(image.extent.width, image.extent.height)
					.initialize(layouts[i]);
				offsets[i] = arena.place(layouts[i].getSize(), layouts[i].getAlignment());
				m_owned[i] = true;
			}

			if (arena.overflowed())
			{
				Fail("its images do not fit in one memory block");
				return false;
			}

			m_image_memory = CreateBlock(
				arena.block_size(),
				DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image | DkMemBlockFlags_ZeroFillInit,
				fmt::format("frame generation images at {}x{}", m_plan.output.width, m_plan.output.height));
			if (!m_image_memory)
			{
				Fail("there is not enough memory for its images");
				return false;
			}

			for (u32 i = 0; i < image_count; ++i)
			{
				if (m_owned[i])
					m_images[i].initialize(layouts[i], m_image_memory, offsets[i]);
			}
			return true;
		}

		const DkImage* DKFrameGenerator::GetChainImage(u32 index) const
		{
			if (m_owned[index])
				return &m_images[index];

			if (index < graph::history_image_count)
				return &m_history[index]->GetImage();

			const u32 generated = index - graph::history_image_count;
			return generated < m_generated.size() ? &m_generated[generated]->GetImage() : nullptr;
		}

		void DKFrameGenerator::WriteDescriptors()
		{
			u8* table = static_cast<u8*>(m_descriptor_memory.getCpuAddr());

			for (u32 i = 0; i < m_descriptors.images.size(); ++i)
			{
				const backend::ImageDescriptors& entry = m_descriptors.images[i];
				const DkImage* image = GetChainImage(i);
				if (!image)
					continue;

				DkImageView view;
				dkImageViewDefaults(&view, image);
				if (entry.sampled != backend::no_descriptor)
				{
					dkImageDescriptorInitialize(
						reinterpret_cast<DkImageDescriptor*>(table + entry.sampled * DESCRIPTOR_SIZE), &view,
						false, false);
				}
				if (entry.storage != backend::no_descriptor)
				{
					dkImageDescriptorInitialize(
						reinterpret_cast<DkImageDescriptor*>(table + entry.storage * DESCRIPTOR_SIZE), &view,
						true, false);
				}
			}

			u8* samplers = table + m_descriptors.image_descriptors * DESCRIPTOR_SIZE;
			for (u32 i = 0; i < backend::sampler_descriptor_count; ++i)
			{
				DkSampler sampler;
				DescribeSampler(i, &sampler);
				dkSamplerDescriptorInitialize(
					reinterpret_cast<DkSamplerDescriptor*>(samplers + i * DESCRIPTOR_SIZE), &sampler);
			}
		}

		void DKFrameGenerator::WriteUniformBuffers()
		{
			if (!m_uniform_memory)
				return;

			u8* buffers = static_cast<u8*>(m_uniform_memory.getCpuAddr());
			for (u32 i = 0; i < m_plan.uniform_buffers; ++i)
			{
				const graph::ConstantBuffer contents = graph::constant_buffer(i, m_graph_config);
				std::memcpy(buffers + i * backend::uniform_buffer_stride, &contents, sizeof(contents));
			}
		}

		void DKFrameGenerator::ReleaseAllocation()
		{
			for (auto& texture : m_history)
				texture.reset();
			m_generated.clear();
			m_images.clear();
			m_owned.clear();

			ReleaseBlock(m_image_memory);
			ReleaseBlock(m_descriptor_memory);
			ReleaseBlock(m_uniform_memory);

			m_width = 0;
			m_height = 0;
		}

		void DKFrameGenerator::Barrier(DkCmdBuf cmdbuf, u32 invalidate)
		{
			dkCmdBufBarrier(cmdbuf, DkBarrier_Primitives, invalidate);
			for (const u32 image : m_hazard_images)
				m_hazards[image] = 0;
			m_hazard_images.clear();
		}

		void DKFrameGenerator::MarkImage(u32 image, u8 hazard)
		{
			if (m_hazards[image] == 0)
				m_hazard_images.push_back(image);
			m_hazards[image] |= hazard;
		}

		bool DKFrameGenerator::Record(DkCmdBuf cmdbuf, u32 dispatch, u32 phase)
		{
			backend::DispatchBinding binding;
			if (const lsfg::ErrorCode code =
					backend::bind(m_cache, m_plan, m_descriptors, dispatch, phase, binding);
				!lsfg::succeeded(code))
			{
				Fail(fmt::format("dispatch {} cannot be bound ({})", dispatch, lsfg::error_name(code)));
				return false;
			}

			bool wait = false;
			u32 invalidate = 0;
			for (u32 i = 0; i < binding.texture_count; ++i)
			{
				if (m_hazards[binding.textures[i].image] & HAZARD_WRITTEN)
				{
					wait = true;
					invalidate |= DkInvalidateFlags_Image;
				}
			}
			for (u32 i = 0; i < binding.storage_count; ++i)
			{
				if (m_hazards[binding.storages[i].image] != 0)
					wait = true;
			}
			if (wait)
				Barrier(cmdbuf, invalidate);

			for (u32 i = 0; i < binding.texture_count; ++i)
				MarkImage(binding.textures[i].image, HAZARD_READ);
			for (u32 i = 0; i < binding.storage_count; ++i)
				MarkImage(binding.storages[i].image, HAZARD_WRITTEN);

			const DkShader* shader = &m_shaders[binding.pass];
			dkCmdBufBindShaders(cmdbuf, DkStageFlag_Compute, &shader, 1);

			std::array<DkResHandle, backend::max_texture_slots> textures{};
			u32 texture_slots = 0;
			for (u32 i = 0; i < binding.texture_count; ++i)
			{
				const backend::TextureBinding& texture = binding.textures[i];
				textures[texture.slot] = dkMakeTextureHandle(texture.descriptor, texture.sampler);
				texture_slots = std::max(texture_slots, texture.slot + 1);
			}
			if (texture_slots != 0)
				dkCmdBufBindTextures(cmdbuf, DkStage_Compute, 0, textures.data(), texture_slots);

			std::array<DkResHandle, backend::max_storage_slots> storages{};
			u32 storage_slots = 0;
			for (u32 i = 0; i < binding.storage_count; ++i)
			{
				const backend::StorageBinding& storage = binding.storages[i];
				storages[storage.slot] = dkMakeImageHandle(storage.descriptor);
				storage_slots = std::max(storage_slots, storage.slot + 1);
			}
			if (storage_slots != 0)
				dkCmdBufBindImages(cmdbuf, DkStage_Compute, 0, storages.data(), storage_slots);

			if (binding.uniform_slot != backend::no_slot)
			{
				dkCmdBufBindUniformBuffer(cmdbuf, DkStage_Compute, binding.uniform_slot,
										  m_uniform_memory.getGpuAddr() +
											  binding.uniform_buffer * backend::uniform_buffer_stride,
										  backend::uniform_buffer_stride);
			}

			dkCmdBufDispatchCompute(cmdbuf, binding.groups_x, binding.groups_y, 1);
			return true;
		}

		bool DKFrameGenerator::Generate(const DKImage* frame, const FrameRect& rect)
		{
			if (m_failed || rect.GetWidth() <= 0 || rect.GetHeight() <= 0)
				return false;

			const u32 width = static_cast<u32>(rect.GetWidth());
			const u32 height = static_cast<u32>(rect.GetHeight());
			if ((width != m_width || height != m_height) && !Allocate(width, height))
			{
				ReleaseAllocation();
				return false;
			}

			const DkCmdBuf cmdbuf = g_dk_command_buffer_mgr->GetCurrentCommandBuffer();

			const DKImage* source = static_cast<const DKImage*>(frame);
			const DkImageView source_view = source->MakeView(0, 0, 1);
			const DkImageView history_view =
				m_history[m_frame % graph::history_image_count]->MakeView(0, 0, 1);
			const DkImageRect source_rect{
				static_cast<u32>(rect.left), static_cast<u32>(rect.top), 0, width, height, 1};
			const DkImageRect history_rect{0, 0, 0, width, height, 1};

			dkCmdBufBlitImage(cmdbuf, &source_view, &source_rect, &history_view, &history_rect,
							  DkBlitFlag_FilterNearest | DkBlitFlag_ModeBlit, 0);

			dkCmdBufBindImageDescriptorSet(cmdbuf, m_descriptor_memory.getGpuAddr(),
										   m_descriptors.image_descriptors);
			dkCmdBufBindSamplerDescriptorSet(
				cmdbuf, m_descriptor_memory.getGpuAddr() + m_descriptors.image_descriptors * DESCRIPTOR_SIZE,
				backend::sampler_descriptor_count);
			dkCmdBufBarrier(cmdbuf, DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors);
			for (const u32 image : m_hazard_images)
				m_hazards[image] = 0;
			m_hazard_images.clear();

			bool recorded = true;
			for (u32 dispatch = 0; recorded && dispatch < m_cache.graph.dispatches.size(); ++dispatch)
				recorded = Record(cmdbuf, dispatch, m_frame);

			dkCmdBufBarrier(cmdbuf, DkBarrier_Primitives,
							DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors);
			InvalidateCachedState();

			if (!recorded)
				return false;

			m_frame = (m_frame + 1) % m_schedule.cycle;
			++m_frames_since_reset;
			return m_frames_since_reset > m_schedule.warmup_frames;
		}

		const DKImage* DKFrameGenerator::GetGeneratedFrame(u32 index) const
		{
			return index < m_generated.size() ? m_generated[index].get() : nullptr;
		}
	} // namespace

	std::unique_ptr<FrameGenerator>
	CreateFrameGenerator(const FrameGenerationConfig& config)
	{
		auto generator = std::make_unique<DKFrameGenerator>(config);
		if (!generator->Initialize())
			return nullptr;

		return generator;
	}
} // namespace Deko3D
