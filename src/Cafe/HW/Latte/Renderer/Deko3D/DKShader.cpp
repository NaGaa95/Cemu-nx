// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DKShader.h"
#include "DKShaderThread.h"
#include "UamBridge.h"
#include "Cafe/CafeSystem.h"
#include "Cemu/FileCache/FileCache.h"
#include "config/ActiveSettings.h"
#include "platform/switch/SwitchThread.h"
#include "util/helpers/helpers.h"

#include <condition_variable>
#include <deque>
#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/SPIRV/GlslangToSpv.h>
#include <spirv_glsl.hpp>
#include <cerrno>
#include <sys/stat.h>

extern std::atomic_int g_compiled_shaders_total;
extern std::atomic_int g_compiled_shaders_async;

namespace Deko3D
{
	namespace
	{
		constexpr u32 CACHE_VERSION = 1;
		constexpr u32 DKSH_MAGIC = 0x48534b44;
		struct DkshHeader
		{
			u32 magic, headerSize, controlSize, codeSize, programsOffset, programs;
		};
		struct DkshProgram
		{
			u32 type, entry, gprs, constantsOffset, constantsSize, scratchSize;
		};
		struct CacheHeader
		{
			u32 magic = 0x434b4443;
			u32 version = CACHE_VERSION;
			u32 stage = 0;
			u32 size = 0;
			u64 sourceHash = 0;
			ShaderBindings bindings;
		};

		struct CachedShader
		{
			RendererShader::ShaderType stage;
			std::string source;
			ShaderBindings bindings;
			std::vector<u8> binary;
			size_t Size() const
			{
				return sizeof(*this) + source.capacity() + binary.capacity();
			}
		};
		std::list<CachedShader> s_cache;
		size_t s_cacheBytes = 0;
		constexpr size_t CACHE_BYTES = 8 * 1024 * 1024;
		constexpr u32 MAX_BINARY_SIZE = 16 * 1024 * 1024;
		constexpr u32 MAX_CACHE_RECORD_SIZE = sizeof(CacheHeader) + MAX_BINARY_SIZE;
		std::unique_ptr<DKShaderCodePool> s_codePool;
		fs::path s_diskDirectory;
		std::unique_ptr<FileCache> s_diskCache;

		class CompilerQueue
		{
		  public:
			void Start()
			{
				m_stop = false;
				m_thread = std::make_unique<ShaderThread>([this] {
					SetThreadName("dkShaderComp");
					SwitchThread_AllowHelperCore("Deko3D shader compiler");
					for (;;)
					{
						std::packaged_task<std::shared_ptr<DKShaderProgram>()> task;
						{
							std::unique_lock lock(m_mutex);
							m_condition.wait(lock, [this] { return m_stop || !m_tasks.empty(); });
							if (m_stop)
								return;
							task = std::move(m_tasks.front().task);
							m_tasks.pop_front();
						}
						task();
						m_pending.fetch_sub(1, std::memory_order_relaxed);
					}
				});
			}
			void Stop()
			{
				{
					std::lock_guard lock(m_mutex);
					m_stop = true;
					m_pending.fetch_sub(static_cast<u32>(m_tasks.size()), std::memory_order_relaxed);
					m_tasks.clear();
				}
				m_condition.notify_all();
				m_thread.reset();
			}
			~CompilerQueue()
			{
				Stop();
			}
			std::shared_future<std::shared_ptr<DKShaderProgram>> Add(std::function<std::shared_ptr<DKShaderProgram>()> task, const void* owner)
			{
				std::packaged_task<std::shared_ptr<DKShaderProgram>()> job(std::move(task));
				auto future = job.get_future().share();
				{
					std::lock_guard lock(m_mutex);
					if (!m_stop)
					{
						m_tasks.push_back({owner, std::move(job)});
						m_pending.fetch_add(1, std::memory_order_relaxed);
					}
				}
				m_condition.notify_one();
				return future;
			}

			u32 PendingCount() const
			{
				return m_pending.load(std::memory_order_relaxed);
			}
			void Prioritize(const void* owner)
			{
				std::lock_guard lock(m_mutex);
				const auto found = std::ranges::find(m_tasks, owner, &Job::owner);
				if (found == m_tasks.end() || found == m_tasks.begin())
					return;
				std::rotate(m_tasks.begin(), found, std::next(found));
			}

		  private:
			struct Job
			{
				const void* owner;
				std::packaged_task<std::shared_ptr<DKShaderProgram>()> task;
			};
			std::mutex m_mutex;
			std::condition_variable m_condition;
			std::deque<Job> m_tasks;
			std::atomic_uint32_t m_pending{0};
			std::unique_ptr<ShaderThread> m_thread;
			bool m_stop = true;
		} s_compiler;

		u64 SourceHash(std::string_view source, RendererShader::ShaderType stage)
		{
			u64 hash = 14695981039346656037ULL ^ static_cast<u64>(stage);
			for (unsigned char byte : source)
				hash = (hash ^ byte) * 1099511628211ULL;
			return hash;
		}

		bool ValidateBinary(std::span<const u8> binary, int stage, u32* scratchSize = nullptr)
		{
			if (scratchSize)
				*scratchSize = 0;
			const u32 programType = stage == UamStage_Fragment ? 1 : stage == UamStage_Geometry ? 2
																								: 0;
			if (binary.size() < sizeof(DkshHeader))
				return false;
			DkshHeader header;
			std::memcpy(&header, binary.data(), sizeof(header));
			if (header.magic != DKSH_MAGIC || header.headerSize < sizeof(header) ||
				header.controlSize > binary.size() || header.codeSize > binary.size() - header.controlSize ||
				header.programsOffset > header.controlSize || !header.programs ||
				header.programs > (header.controlSize - header.programsOffset) / 64)
				return false;
			u32 requiredScratch = 0;
			for (u32 i = 0; i < header.programs; ++i)
			{
				DkshProgram program;
				std::memcpy(&program, binary.data() + header.programsOffset + i * 64, sizeof(program));
				if (program.type != programType)
					return false;
				requiredScratch = std::max(requiredScratch, program.scratchSize);
			}
			if (scratchSize)
				*scratchSize = requiredScratch;
			return requiredScratch <= PER_WARP_SCRATCH_MEMORY_SIZE;
		}

		bool ValidateBindings(const ShaderBindings& bindings)
		{
			auto valid = [](const auto& slots, int count) {
				return std::ranges::all_of(slots, [count](int slot) { return slot >= -1 && slot < count; });
			};
			return valid(bindings.uniforms, DK_NUM_UNIFORM_BUFS) && valid(bindings.textures, DK_NUM_TEXTURE_BINDINGS) &&
				   valid(bindings.storage, DK_NUM_STORAGE_BUFS) && bindings.pushConstants >= -1 && bindings.pushConstants < DK_NUM_UNIFORM_BUFS;
		}

		bool LoadCachedBinary(std::span<const u8> data, u64 hash, int stage, DKShaderProgram& result)
		{
			if (data.size() < sizeof(CacheHeader))
				return false;
			CacheHeader header;
			std::memcpy(&header, data.data(), sizeof(header));
			const auto binary = data.subspan(sizeof(header));
			if (header.magic != CacheHeader{}.magic || header.version != CACHE_VERSION ||
				header.stage != static_cast<u32>(stage) || header.sourceHash != hash ||
				!header.size || header.size > MAX_BINARY_SIZE || header.size != binary.size() ||
				!ValidateBinary(binary, stage) || !ValidateBindings(header.bindings))
				return false;
			result.binary.assign(binary.begin(), binary.end());
			result.bindings = header.bindings;
			return true;
		}

		void StoreCachedBinary(u64 hash, int stage, const DKShaderProgram& program)
		{
			if (s_diskCache)
			{
				CacheHeader header{};
				header.stage = stage;
				header.size = static_cast<u32>(program.binary.size());
				header.sourceHash = hash;
				header.bindings = program.bindings;
				std::vector<u8> data(sizeof(header) + program.binary.size());
				std::memcpy(data.data(), &header, sizeof(header));
				std::memcpy(data.data() + sizeof(header), program.binary.data(), program.binary.size());
				s_diskCache->AddFile({hash, static_cast<u64>(stage)}, data.data(), static_cast<sint32>(data.size()));
			}
		}

		bool ReadDiskCache(u64 hash, int stage, DKShaderProgram& result)
		{
			{
				std::vector<u8> data;
				if (s_diskCache && s_diskCache->GetFile({hash, static_cast<u64>(stage)}, data, MAX_CACHE_RECORD_SIZE) &&
					LoadCachedBinary(data, hash, stage, result))
					return true;
				std::ifstream file(s_diskDirectory / fmt::format("{:016x}.dksh", hash), std::ios::binary);
				CacheHeader header;
				if (!file.read(reinterpret_cast<char*>(&header), sizeof(header)) ||
					!header.size || header.size > MAX_BINARY_SIZE)
					return false;
				data.resize(sizeof(header) + header.size);
				std::memcpy(data.data(), &header, sizeof(header));
				if (!file.read(reinterpret_cast<char*>(data.data() + sizeof(header)), header.size) ||
					!LoadCachedBinary(data, hash, stage, result))
					return false;
			}
			StoreCachedBinary(hash, stage, result);
			return true;
		}

		void OpenDiskCache()
		{
			s_diskDirectory = ActiveSettings::GetCachePath(fmt::format("shaderCache/driver/deko3d/{:016x}/v{}",
				CafeSystem::GetForegroundTitleId(), CACHE_VERSION));
			const fs::path path = s_diskDirectory / "shaders.bin";
			s_diskCache.reset(FileCache::Open(path, false, CACHE_VERSION));
			if (!s_diskCache)
			{
				fs::path current;
				for (const auto& component : s_diskDirectory)
				{
					current /= component;
					if (current == current.root_path() || current.string().ends_with(':'))
						continue;
					if (mkdir(current.c_str(), 0777) != 0 && errno != EEXIST)
						break;
				}
				s_diskCache.reset(FileCache::Open(path, true, CACHE_VERSION));
			}
			if (s_diskCache)
				s_diskCache->UseCompression(false);
			else
				cemuLog_log(LogType::Force, "Unable to open Deko3D indexed shader cache {}", _pathToUtf8(path));
		}

		std::string Translate(RendererShader::ShaderType type, const std::string& source, ShaderBindings& bindings)
		{
			const EShLanguage language = type == RendererShader::ShaderType::kVertex ? EShLangVertex : type == RendererShader::ShaderType::kFragment ? EShLangFragment
																																					 : EShLangGeometry;
			glslang::TShader shader(language);
			const char* code = source.c_str();
			shader.setStrings(&code, 1);
			shader.setEnvInput(glslang::EShSourceGlsl, language, glslang::EShClientVulkan, 100);
			shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
			shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
			const auto messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
			if (!shader.parse(GetDefaultResources(), 450, false, messages))
				throw std::runtime_error(shader.getInfoLog());
			glslang::TProgram program;
			program.addShader(&shader);
			if (!program.link(messages) || !program.mapIO())
				throw std::runtime_error(program.getInfoLog());
			std::vector<u32> spirv;
			glslang::SpvOptions options;
			options.disableOptimizer = true;
			glslang::GlslangToSpv(*program.getIntermediate(language), spirv, &options);
			spirv_cross::CompilerGLSL cross(std::move(spirv));
			auto resources = cross.get_shader_resources();
			auto remap = [&](const auto& list, auto& table, u32 maximum) {
				u32 slot = 0;
				for (const auto& resource : list)
				{
					const u32 original = cross.get_decoration(resource.id, spv::DecorationBinding);
					if (original >= table.size() || slot >= maximum)
						throw std::runtime_error("Shader exceeds Deko3D resource limits");
					table[original] = slot;
					cross.unset_decoration(resource.id, spv::DecorationDescriptorSet);
					cross.set_decoration(resource.id, spv::DecorationBinding, slot++);
				}
				return slot;
			};
			const u32 uniforms = remap(resources.uniform_buffers, bindings.uniforms, DK_NUM_UNIFORM_BUFS);
			remap(resources.sampled_images, bindings.textures, DK_NUM_TEXTURE_BINDINGS);
			remap(resources.storage_buffers, bindings.storage, DK_NUM_STORAGE_BUFS);
			if (!resources.push_constant_buffers.empty())
			{
				if (resources.push_constant_buffers.size() != 1 || uniforms >= DK_NUM_UNIFORM_BUFS)
					throw std::runtime_error("Shader has too many constant buffers");
				cross.set_decoration(resources.push_constant_buffers.front().id, spv::DecorationBinding, uniforms);
				bindings.pushConstants = uniforms;
			}
			spirv_cross::CompilerGLSL::Options glsl;
			glsl.version = 450;
			glsl.es = false;
			glsl.enable_420pack_extension = true;
			glsl.emit_push_constant_as_uniform_buffer = true;
			cross.set_common_options(glsl);
			return cross.compile();
		}

		std::shared_ptr<DKShaderProgram> Compile(RendererShader::ShaderType type, std::string source, u64 base, u64 auxiliary)
		{
			try
			{
				const int stage = type == RendererShader::ShaderType::kVertex ? UamStage_Vertex : type == RendererShader::ShaderType::kFragment ? UamStage_Fragment
																																				: UamStage_Geometry;
				const u64 hash = SourceHash(source, type);
				auto result = std::make_shared<DKShaderProgram>();
				const auto cached = std::ranges::find_if(s_cache, [&](const CachedShader& entry) {
					return entry.stage == type && entry.source == source;
				});
				if (cached != s_cache.end())
				{
					result->binary = cached->binary;
					result->bindings = cached->bindings;
					s_cache.splice(s_cache.begin(), s_cache, cached);
				}
				else
				{
					ReadDiskCache(hash, stage, *result);
					if (result->binary.empty())
					{
						const std::string glsl = Translate(type, source, result->bindings);
						size_t size = 0;
						char* log = nullptr;
						std::unique_ptr<void, decltype(&std::free)> binary(UamCompileGlsl(glsl.c_str(), stage, &size, &log), &std::free);
						std::unique_ptr<char, decltype(&std::free)> diagnostics(log, &std::free);
						if (!binary)
							throw std::runtime_error(log ? log : "UAM compilation failed");
						result->binary.assign(static_cast<u8*>(binary.get()), static_cast<u8*>(binary.get()) + size);
						u32 scratchSize = 0;
						if (!ValidateBinary(result->binary, stage, &scratchSize))
						{
							if (scratchSize > PER_WARP_SCRATCH_MEMORY_SIZE)
								throw std::runtime_error(fmt::format("Shader needs {} bytes of scratch per warp; the queue has {}",
									scratchSize, PER_WARP_SCRATCH_MEMORY_SIZE));
							throw std::runtime_error("Invalid DKSH shader binary");
						}
						StoreCachedBinary(hash, stage, *result);
					}
					CachedShader entry{type, std::move(source), result->bindings, result->binary};
					const size_t bytes = entry.Size();
					if (bytes <= CACHE_BYTES)
					{
						while (!s_cache.empty() && (s_cacheBytes + bytes > CACHE_BYTES || s_cache.size() >= 128))
						{
							s_cacheBytes -= s_cache.back().Size();
							s_cache.pop_back();
						}
						s_cacheBytes += bytes;
						s_cache.push_front(std::move(entry));
					}
				}
				DkshHeader header;
				std::memcpy(&header, result->binary.data(), sizeof(header));
				result->code = s_codePool->Upload(std::span(result->binary).subspan(header.controlSize, header.codeSize));
				DkShaderMaker maker;
				dkShaderMakerDefaults(&maker, result->code->GetMemory(), result->code->GetOffset());
				maker.control = result->binary.data();
				dkShaderInitialize(&result->shader, &maker);
				if (!dkShaderIsValid(&result->shader))
					throw std::runtime_error("Deko3D rejected the shader");
				++g_compiled_shaders_total;
				++g_compiled_shaders_async;
				return result;
			} catch (const std::exception& exception)
			{
				cemuLog_log(LogType::Force, "Deko3D shader {:016x}_{:016x}: {}", base, auxiliary, exception.what());
				return nullptr;
			}
		}
	} // namespace

	RendererShaderDK::RendererShaderDK(ShaderType type, u64 base, u64 auxiliary, std::string source,
									   bool compileAsync, bool gfxPack)
		: RendererShader(type, base, auxiliary, base != 0, gfxPack)
	{
		m_job = s_compiler.Add([=, source = std::move(source)]() mutable { return Compile(type, std::move(source), base, auxiliary); }, this);
		if (!compileAsync)
			WaitForCompiled();
	}

	RendererShaderDK::~RendererShaderDK()
	{
		if (m_job.valid())
			m_job.wait();
	}

	void RendererShaderDK::PreponeCompilation(bool)
	{
		WaitForCompiled();
	}
	bool RendererShaderDK::IsCompiled()
	{
		return m_job.valid() && m_job.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
	}
	bool RendererShaderDK::WaitForCompiled()
	{
		if (m_job.valid() && !IsCompiled())
			s_compiler.Prioritize(this);
		try
		{
			m_program = m_job.get();
		} catch (const std::future_error&)
		{
			m_program.reset();
		}
		return m_program != nullptr;
	}
	const DKShaderProgram* RendererShaderDK::GetProgram()
	{
		return (m_program || WaitForCompiled()) ? m_program.get() : nullptr;
	}
	u32 RendererShaderDK::GetPendingCompilationCount()
	{
		return s_compiler.PendingCount();
	}

	void RendererShaderDK::InitializeCompiler()
	{
		glslang::InitializeProcess();
		s_codePool = std::make_unique<DKShaderCodePool>();
		OpenDiskCache();
		s_compiler.Start();
	}
	void RendererShaderDK::ShutdownCompiler()
	{
		s_compiler.Stop();
		s_diskCache.reset();
		s_diskDirectory.clear();
		s_cache.clear();
		s_cacheBytes = 0;
		s_codePool.reset();
	}
} // namespace Deko3D
