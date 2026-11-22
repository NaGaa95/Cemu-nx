// SPDX-FileCopyrightText: 2026 PalindromicBreadLoaf
// SPDX-License-Identifier: GPL-3.0-or-later
// From Kankohi (src/Common/switch/HorizonAudio.cpp), adapted for Cemu-nx.

#include "platform/switch/SwitchAudren.h"
#include "Cemu/Logging/CemuLogging.h"

extern "C" {
#include <switch/arm/cache.h>
#include <switch/audio/driver.h>
#include <switch/kernel/svc.h>
#include <switch/services/audren.h>
}

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

namespace
{
	constexpr AudioRendererConfig RENDERER_CONFIG = {
		.output_rate = AudioRendererOutputRate_48kHz,
		.num_voices = 24,
		.num_effects = 0,
		.num_sinks = 1,
		.num_mix_objs = 1,
		.num_mix_buffers = 2,
	};
	constexpr int OUTPUT_CHANNELS = 2;
	constexpr int DETACH_ATTEMPTS = 20;
	constexpr uint64_t FRAME_NS = 5'000'000;

	struct StereoFactors
	{
		float left;
		float right;
	};

	constexpr float DOWNMIX = 0.7071f;
	constexpr std::array<StereoFactors, 1> MONO_FACTORS{{{1.0f, 1.0f}}};
	constexpr std::array<StereoFactors, 2> STEREO_FACTORS{{{1.0f, 0.0f}, {0.0f, 1.0f}}};
	constexpr std::array<StereoFactors, 4> QUAD_FACTORS{{{1.0f, 0.0f}, {0.0f, 1.0f}, {DOWNMIX, 0.0f}, {0.0f, DOWNMIX}}};
	// FL FR FC LFE BL BR
	constexpr std::array<StereoFactors, 6> SURROUND_FACTORS{
		{{1.0f, 0.0f}, {0.0f, 1.0f}, {DOWNMIX, DOWNMIX}, {0.0f, 0.0f}, {DOWNMIX, 0.0f}, {0.0f, DOWNMIX}}};

	const StereoFactors* GetDownmix(uint32_t channels)
	{
		switch (channels)
		{
		case 1:
			return MONO_FACTORS.data();
		case 2:
			return STEREO_FACTORS.data();
		case 4:
			return QUAD_FACTORS.data();
		case 6:
			return SURROUND_FACTORS.data();
		default:
			return nullptr;
		}
	}

	size_t AlignUp(size_t value, size_t alignment)
	{
		return (value + alignment - 1) & ~(alignment - 1);
	}

	AudioDriver s_driver;
	std::mutex s_mutex;
	std::vector<SwitchAudren::Stream*> s_streams;
	bool s_running = false;
	std::atomic<bool> s_active = true;
}

struct SwitchAudren::Stream
{
	int voice = -1;
	int memPool = -1;
	uint32_t blockBytes = 0;
	uint32_t blockCount = 0;
	uint8_t* memory = nullptr;
	std::unique_ptr<AudioDriverWaveBuf[]> waveBufs;

	std::mutex producerMutex;
	alignas(64) std::atomic<uint32_t> written = 0;
	alignas(64) std::atomic<uint32_t> released = 0;

	alignas(64) uint32_t submitted = 0;
	std::atomic<bool> playing = false;
	std::atomic<float> volume = 1.0f;
	bool appliedPlaying = false;
	float appliedVolume = 1.0f;
};

namespace
{
	void SubmitBlocks(SwitchAudren::Stream& stream)
	{
		const uint32_t written = stream.written.load(std::memory_order_acquire);
		for (; stream.submitted != written; stream.submitted++)
			audrvVoiceAddWaveBuf(&s_driver, stream.voice, &stream.waveBufs[stream.submitted % stream.blockCount]);

		const bool playing = s_active.load(std::memory_order_relaxed) && stream.playing.load(std::memory_order_relaxed);
		if (playing != stream.appliedPlaying)
		{
			audrvVoiceSetPaused(&s_driver, stream.voice, !playing);
			stream.appliedPlaying = playing;
		}
		const float volume = stream.volume.load(std::memory_order_relaxed);
		if (volume != stream.appliedVolume)
		{
			audrvVoiceSetVolume(&s_driver, stream.voice, volume);
			stream.appliedVolume = volume;
		}
	}

	void ReleaseBlocks(SwitchAudren::Stream& stream)
	{
		uint32_t released = stream.released.load(std::memory_order_relaxed);
		while (released != stream.submitted &&
			   stream.waveBufs[released % stream.blockCount].state == AudioDriverWaveBufState_Done)
			released++;
		stream.released.store(released, std::memory_order_release);
	}

	int FindFreeVoice()
	{
		for (int i = 0; i < RENDERER_CONFIG.num_voices; i++)
		{
			if (!s_driver.in_voices[i].is_used)
				return i;
		}
		return -1;
	}

	bool ReleaseMemPool(int memPool)
	{
		if (s_driver.in_mempools[memPool].state == AudioRendererMemPoolState_New)
			return audrvMemPoolRemove(&s_driver, memPool);
		audrvMemPoolDetach(&s_driver, memPool);
		for (int i = 0; i < DETACH_ATTEMPTS; i++)
		{
			audrvUpdate(&s_driver);
			if (s_driver.in_mempools[memPool].state == AudioRendererMemPoolState_Detached)
				return audrvMemPoolRemove(&s_driver, memPool);
			svcSleepThread(FRAME_NS);
		}
		return false;
	}

	void DestroyStream(SwitchAudren::Stream* stream)
	{
		if (stream->voice >= 0)
			audrvVoiceDrop(&s_driver, stream->voice);
		if (stream->memPool >= 0 && !ReleaseMemPool(stream->memPool))
		{
			// the renderer may still read it: leak it rather than free it
			cemuLog_log(LogType::Force, "Audio renderer: memory pool {} did not detach", stream->memPool);
			stream->memory = nullptr;
		}
		std::free(stream->memory);
		delete stream;
	}
}

namespace SwitchAudren
{
	bool Init()
	{
		std::scoped_lock lock(s_mutex);
		if (s_running)
			return true;

		Result rc = audrenInitialize(&RENDERER_CONFIG);
		if (R_FAILED(rc))
		{
			cemuLog_log(LogType::Force, "Audio renderer: audrenInitialize failed (0x{:x})", rc);
			return false;
		}

		rc = audrvCreate(&s_driver, &RENDERER_CONFIG, OUTPUT_CHANNELS);
		if (R_FAILED(rc))
		{
			cemuLog_log(LogType::Force, "Audio renderer: audrvCreate failed (0x{:x})", rc);
			audrenExit();
			return false;
		}

		static constexpr std::array<u8, OUTPUT_CHANNELS> SINK_CHANNELS{0, 1};
		if (audrvDeviceSinkAdd(&s_driver, AUDREN_DEFAULT_DEVICE_NAME, OUTPUT_CHANNELS, SINK_CHANNELS.data()) < 0)
			cemuLog_log(LogType::Force, "Audio renderer: could not add the output sink");
		rc = audrvUpdate(&s_driver);
		if (R_SUCCEEDED(rc))
			rc = audrenStartAudioRenderer();
		if (R_FAILED(rc))
		{
			cemuLog_log(LogType::Force, "Audio renderer: could not start (0x{:x})", rc);
			audrvClose(&s_driver);
			audrenExit();
			return false;
		}
		s_running = true;
		return true;
	}

	void Shutdown()
	{
		std::scoped_lock lock(s_mutex);
		if (!s_running)
			return;
		for (Stream* stream : s_streams)
			DestroyStream(stream);
		s_streams.clear();
		audrenStopAudioRenderer();
		audrvClose(&s_driver);
		audrenExit();
		s_running = false;
	}

	void Update()
	{
		{
			std::scoped_lock lock(s_mutex);
			if (!s_running)
			{
				svcSleepThread(FRAME_NS);
				return;
			}
			for (Stream* stream : s_streams)
				SubmitBlocks(*stream);
			audrvUpdate(&s_driver);
			for (Stream* stream : s_streams)
				ReleaseBlocks(*stream);
		}
		audrenWaitFrame();
	}

	void SetActive(bool active)
	{
		s_active.store(active, std::memory_order_relaxed);
	}

	bool IsActive()
	{
		return s_active.load(std::memory_order_relaxed);
	}

	Stream* OpenStream(uint32_t sampleRate, uint32_t channels, uint32_t framesPerBlock, uint32_t blockCount)
	{
		const StereoFactors* downmix = GetDownmix(channels);
		if (!downmix || framesPerBlock == 0 || blockCount == 0)
		{
			cemuLog_log(LogType::Force, "Audio renderer: unsupported stream of {} channels, {} frames", channels, framesPerBlock);
			return nullptr;
		}

		std::scoped_lock lock(s_mutex);
		if (!s_running)
			return nullptr;

		auto* stream = new Stream();
		stream->blockBytes = framesPerBlock * channels * sizeof(int16_t);
		stream->blockCount = blockCount;
		const size_t slotBytes = AlignUp(stream->blockBytes, AUDREN_BUFFER_ALIGNMENT);
		const size_t memorySize = AlignUp(slotBytes * blockCount, AUDREN_MEMPOOL_ALIGNMENT);
		stream->memory = static_cast<uint8_t*>(std::aligned_alloc(AUDREN_MEMPOOL_ALIGNMENT, memorySize));
		stream->waveBufs = std::make_unique<AudioDriverWaveBuf[]>(blockCount);
		if (!stream->memory)
		{
			cemuLog_log(LogType::Force, "Audio renderer: could not allocate {} bytes", memorySize);
			DestroyStream(stream);
			return nullptr;
		}
		std::memset(stream->memory, 0, memorySize);
		armDCacheFlush(stream->memory, memorySize);
		for (uint32_t i = 0; i < blockCount; i++)
		{
			AudioDriverWaveBuf& waveBuf = stream->waveBufs[i];
			waveBuf = {};
			waveBuf.data_raw = stream->memory + i * slotBytes;
			waveBuf.size = stream->blockBytes;
			waveBuf.end_sample_offset = static_cast<s32>(framesPerBlock);
		}

		stream->memPool = audrvMemPoolAdd(&s_driver, stream->memory, memorySize);
		if (stream->memPool < 0 || !audrvMemPoolAttach(&s_driver, stream->memPool))
		{
			cemuLog_log(LogType::Force, "Audio renderer: could not add a memory pool");
			DestroyStream(stream);
			return nullptr;
		}

		const int voice = FindFreeVoice();
		if (voice < 0 || !audrvVoiceInit(&s_driver, voice, static_cast<int>(channels), PcmFormat_Int16, static_cast<int>(sampleRate)))
		{
			cemuLog_log(LogType::Force, "Audio renderer: no voice left for {} channels", channels);
			DestroyStream(stream);
			return nullptr;
		}
		stream->voice = voice;
		audrvVoiceSetDestinationMix(&s_driver, voice, AUDREN_FINAL_MIX_ID);
		for (uint32_t channel = 0; channel < channels; channel++)
		{
			audrvVoiceSetMixFactor(&s_driver, voice, downmix[channel].left, static_cast<int>(channel), 0);
			audrvVoiceSetMixFactor(&s_driver, voice, downmix[channel].right, static_cast<int>(channel), 1);
		}
		audrvVoiceSetPaused(&s_driver, voice, true);
		audrvUpdate(&s_driver);

		s_streams.push_back(stream);
		return stream;
	}

	void CloseStream(Stream* stream)
	{
		if (!stream)
			return;
		std::scoped_lock lock(s_mutex);
		std::erase(s_streams, stream);
		DestroyStream(stream);
	}

	bool QueueBlock(Stream* stream, const int16_t* samples)
	{
		std::scoped_lock lock(stream->producerMutex);
		const uint32_t written = stream->written.load(std::memory_order_relaxed);
		if (written - stream->released.load(std::memory_order_acquire) >= stream->blockCount)
			return false;
		void* const destination = const_cast<void*>(stream->waveBufs[written % stream->blockCount].data_raw);
		std::memcpy(destination, samples, stream->blockBytes);
		armDCacheFlush(destination, stream->blockBytes);
		stream->written.store(written + 1, std::memory_order_release);
		return true;
	}

	uint32_t GetQueuedBlocks(const Stream* stream)
	{
		return stream->written.load(std::memory_order_acquire) - stream->released.load(std::memory_order_acquire);
	}

	void SetPlaying(Stream* stream, bool playing)
	{
		stream->playing.store(playing, std::memory_order_relaxed);
	}

	void SetVolume(Stream* stream, float volume)
	{
		stream->volume.store(std::clamp(volume, 0.0f, 1.0f), std::memory_order_relaxed);
	}
}
