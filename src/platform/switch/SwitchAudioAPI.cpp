extern "C" {
#include <switch/arm/cache.h>
#include <switch/kernel/event.h>
#include <switch/kernel/uevent.h>
#include <switch/runtime/hosversion.h>
#include <switch/services/audout.h>
}

#include "platform/switch/SwitchThread.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "audio/SwitchAudioAPI.h"
#include "util/helpers/helpers.h"

namespace
{
constexpr uint32 kOutputRate = 48000;
constexpr uint32 kOutputChannels = 2;
constexpr uint32 kOutputBufferBytes = 0x10000;
constexpr uint32 kOutputFramesPerBuffer = kOutputBufferBytes / (kOutputChannels * sizeof(sint16));
constexpr uint32 kSwitchBlockCount = IAudioAPI::kBlockCount;
constexpr uint32 kMaxQueuedFrames = kOutputRate;
constexpr uint64 kPlayingBit = uint64{1} << 63;

struct SwitchAudioEndpoint
{
	IAudioAPI::AudioType type{IAudioAPI::AudioType::TV};
	uint32 samplerate{};
	uint32 channels{};
	uint32 samplesPerBlock{};
	sint32 volume{};
	bool playing{};
	uint32 queuedFrames{};
	uint32 framesPerBlock{};
	uint32 frontBlockFrames{};
	size_t readFrame{};
	size_t writeFrame{};
	std::vector<sint16> samples;
	std::atomic<uint64> queueState{0}; // Playing flag, in-flight buffer count, queued frame count.
};

class SwitchAudioMixer
{
  public:
	bool Initialize()
	{
		ueventCreate(&m_wake, true);
		Handle handle = INVALID_HANDLE;
		const Result result = serviceDispatch(audoutGetServiceSession_AudioOut(), 4,
			.out_handle_attrs = { SfOutHandleAttr_HipcCopy }, .out_handles = &handle);
		if (R_FAILED(result))
			return false;
		eventLoadRemote(&m_bufferEvent, handle, true);
		for (uint32 i = 0; i < kSwitchBlockCount; ++i)
		{
			void* memory = aligned_alloc(0x1000, kOutputBufferBytes);
			if (!memory)
			{
				ReleaseMemory();
				return false;
			}
			m_memory[i] = memory;
			auto& buffer = m_buffers[i];
			std::memset(&buffer, 0, sizeof(buffer));
			buffer.buffer = memory;
			buffer.buffer_size = kOutputBufferBytes;
			m_freeBuffers.push_back(&buffer);
		}
		try
		{
			m_worker = std::thread(&SwitchAudioMixer::WorkerMain, this);
		}
		catch (...)
		{
			ReleaseMemory();
			return false;
		}
		return true;
	}

	~SwitchAudioMixer()
	{
		StopWorker();
		ReleaseMemory();
		eventClose(&m_bufferEvent);
	}

	void Add(SwitchAudioEndpoint* endpoint)
	{
		std::scoped_lock lock(m_mutex);
		endpoint->samples.resize(static_cast<size_t>(kMaxQueuedFrames) * kOutputChannels);
		m_endpoints.push_back(endpoint);
		NotifyWorker();
	}

	void Remove(SwitchAudioEndpoint* endpoint)
	{
		std::scoped_lock lock(m_mutex);
		endpoint->playing = false;
		ClearQueue(*endpoint);
		std::erase(m_endpoints, endpoint);
		if (!AnyEndpointPlaying())
			StopHardware();
		NotifyWorker();
	}

	bool Play(SwitchAudioEndpoint& endpoint)
	{
		std::scoped_lock lock(m_mutex);
		endpoint.playing = true;
		const bool success = !m_applicationActive || StartHardware();
		NotifyWorker();
		return success;
	}

	bool Stop(SwitchAudioEndpoint& endpoint)
	{
		std::scoped_lock lock(m_mutex);
		endpoint.playing = false;
		ClearQueue(endpoint);
		const bool success = AnyEndpointPlaying() || StopHardware();
		NotifyWorker();
		return success;
	}

	void SetVolume(SwitchAudioEndpoint& endpoint, sint32 volume)
	{
		std::scoped_lock lock(m_mutex);
		endpoint.volume = std::clamp(volume, 0, 100);
		NotifyWorker();
	}

	void SetApplicationActive(bool active)
	{
		std::scoped_lock lock(m_mutex);
		if (m_applicationActive == active)
			return;
		m_applicationActive = active;
		if (!active)
		{
			for (auto* endpoint : m_endpoints)
				ClearQueue(*endpoint);
			StopHardware();
		}
		else if (AnyEndpointPlaying())
		{
			StartHardware();
		}
		NotifyWorker();
	}

	bool NeedsBlocks(const SwitchAudioEndpoint& endpoint, uint32 delay)
	{
		const uint64 state = endpoint.queueState.load(std::memory_order_acquire);
		if ((state & kPlayingBit) == 0)
			return false;
		const uint64 wantedFrames = uint64{std::max(endpoint.framesPerBlock, 1U)} * std::max(delay, 1U);
		const uint32 inFlight = static_cast<uint32>((state & ~kPlayingBit) >> 32);
		return inFlight < delay && static_cast<uint32>(state) < wantedFrames;
	}

	bool Feed(SwitchAudioEndpoint& endpoint, const sint16* data)
	{
		if (!data)
			return false;
		std::scoped_lock lock(m_mutex);
		if (!m_applicationActive || !endpoint.playing)
			return true;

		const uint32 frames = ConvertToStereo(endpoint, data);
		if (frames == 0)
			return false;
		NotifyWorker();
		return true;
	}

	void Shutdown()
	{
		{
			std::scoped_lock lock(m_mutex);
			m_applicationActive = false;
			for (auto* endpoint : m_endpoints)
			{
				endpoint->playing = false;
				ClearQueue(*endpoint);
			}
			StopHardware();
			m_workerShutdown = true;
			NotifyWorker();
		}
		if (m_worker.joinable())
			m_worker.join();
	}

  private:
	void PublishQueueState()
	{
		const uint64 inFlight = uint64{static_cast<uint32>(InFlight())} << 32;
		for (auto* endpoint : m_endpoints)
		{
			const uint64 playing = m_applicationActive && endpoint->playing ? kPlayingBit : 0;
			endpoint->queueState.store(playing | inFlight | endpoint->queuedFrames, std::memory_order_release);
		}
	}

	void NotifyWorker()
	{
		PublishQueueState();
		ueventSignal(&m_wake);
	}

	void StopWorker()
	{
		{
			std::scoped_lock lock(m_mutex);
			m_workerShutdown = true;
			NotifyWorker();
		}
		if (m_worker.joinable())
			m_worker.join();
	}

	void WorkerMain()
	{
		SetThreadName("SwitchAudio");
		SwitchThread_PinToHelperCore("audio mixer", 43);
		const Waiter waiters[] = {waiterForUEvent(&m_wake), waiterForEvent(&m_bufferEvent)};
		for (;;)
		{
			s32 signaled;
			if (R_FAILED(waitObjects(&signaled, waiters, 2, UINT64_MAX)))
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			std::scoped_lock lock(m_mutex);
			if (m_workerShutdown)
				break;
			if (InFlight() > 0)
				ReclaimBuffers();
			Pump();
			PublishQueueState();
		}
	}

	void ReleaseMemory()
	{
		for (void*& memory : m_memory)
		{
			free(memory);
			memory = nullptr;
		}
	}

	bool ReclaimBuffers()
	{
		bool reclaimed = false;
		AudioOutBuffer* released = nullptr;
		u32 count = 0;
		while (R_SUCCEEDED(audoutGetReleasedAudioOutBuffer(&released, &count)) && count > 0 && released)
		{
			m_freeBuffers.push_back(released);
			reclaimed = true;
			released = nullptr;
			count = 0;
		}
		return reclaimed;
	}

	void ResetFreeBuffers()
	{
		m_freeBuffers.clear();
		for (auto& buffer : m_buffers)
			m_freeBuffers.push_back(&buffer);
	}

	bool StartHardware()
	{
		if (m_started)
			return true;
		m_started = R_SUCCEEDED(audoutStartAudioOut());
		return m_started;
	}

	bool StopHardware()
	{
		bool success = true;
		if (m_started && R_FAILED(audoutStopAudioOut()))
			success = false;
		m_started = false;

		if (hosversionAtLeast(4, 0, 0))
		{
			bool flushed = false;
			if (R_FAILED(audoutFlushAudioOutBuffers(&flushed)))
				success = false;
		}
		ReclaimBuffers();

		for (auto& buffer : m_buffers)
		{
			bool contained = false;
			if (R_FAILED(audoutContainsAudioOutBuffer(&buffer, &contained)) || contained)
				success = false;
		}
		if (success)
			ResetFreeBuffers();
		return success;
	}

	bool AnyEndpointPlaying() const
	{
		return std::ranges::any_of(m_endpoints, [](const auto* endpoint) { return endpoint->playing; });
	}

	int InFlight() const
	{
		return static_cast<int>(kSwitchBlockCount) - static_cast<int>(m_freeBuffers.size());
	}

	static void ClearQueue(SwitchAudioEndpoint& endpoint)
	{
		endpoint.queuedFrames = 0;
		endpoint.frontBlockFrames = 0;
		endpoint.readFrame = 0;
		endpoint.writeFrame = 0;
	}

	static uint32 FrontFrames(const SwitchAudioEndpoint& endpoint)
	{
		return endpoint.frontBlockFrames;
	}

	static bool ConsumeFrame(SwitchAudioEndpoint& endpoint, sint16& left, sint16& right)
	{
		if (endpoint.queuedFrames == 0)
			return false;
		const size_t sample = endpoint.readFrame * kOutputChannels;
		left = endpoint.samples[sample];
		right = endpoint.samples[sample + 1];
		if (++endpoint.readFrame == kMaxQueuedFrames)
			endpoint.readFrame = 0;
		--endpoint.queuedFrames;
		if (endpoint.frontBlockFrames > 0)
			--endpoint.frontBlockFrames;
		if (endpoint.frontBlockFrames == 0 && endpoint.queuedFrames > 0)
			endpoint.frontBlockFrames = std::min(endpoint.framesPerBlock, endpoint.queuedFrames);
		return true;
	}

	static void CopyScaledSamples(sint16* output, const sint16* input, size_t sampleCount, sint32 volume)
	{
		if (volume == 100)
		{
			std::memcpy(output, input, sampleCount * sizeof(sint16));
			return;
		}
		if (volume == 0)
		{
			std::memset(output, 0, sampleCount * sizeof(sint16));
			return;
		}
		for (size_t i = 0; i < sampleCount; ++i)
			output[i] = static_cast<sint16>((static_cast<sint32>(input[i]) * volume) / 100);
	}

	static void ConsumeFramesSingle(SwitchAudioEndpoint& endpoint, sint16* output, uint32 frames)
	{
		const uint32 firstFrames = std::min<uint32>(frames,
			kMaxQueuedFrames - static_cast<uint32>(endpoint.readFrame));
		CopyScaledSamples(output, endpoint.samples.data() + endpoint.readFrame * kOutputChannels,
			static_cast<size_t>(firstFrames) * kOutputChannels, endpoint.volume);
		const uint32 remainingFrames = frames - firstFrames;
		if (remainingFrames > 0)
		{
			CopyScaledSamples(output + firstFrames * kOutputChannels, endpoint.samples.data(),
				static_cast<size_t>(remainingFrames) * kOutputChannels, endpoint.volume);
		}
		endpoint.readFrame = (endpoint.readFrame + frames) % kMaxQueuedFrames;
		endpoint.queuedFrames -= frames;
		endpoint.frontBlockFrames -= frames;
		if (endpoint.frontBlockFrames == 0 && endpoint.queuedFrames > 0)
			endpoint.frontBlockFrames = std::min(endpoint.framesPerBlock, endpoint.queuedFrames);
	}

	static uint32 ConvertToStereo(SwitchAudioEndpoint& endpoint, const sint16* input)
	{
		if (endpoint.samplerate == 0 || endpoint.channels == 0 || endpoint.samplesPerBlock == 0)
			return 0;
		const uint64 convertedFrames64 =
			(static_cast<uint64>(endpoint.samplesPerBlock) * kOutputRate + endpoint.samplerate - 1) /
			endpoint.samplerate;
		if (convertedFrames64 == 0 || convertedFrames64 > kMaxQueuedFrames)
			return 0;
		const uint32 convertedFrames = static_cast<uint32>(convertedFrames64);
		if (endpoint.queuedFrames > kMaxQueuedFrames - convertedFrames)
			return 0;
		endpoint.framesPerBlock = convertedFrames;
		const bool wasEmpty = endpoint.queuedFrames == 0;

		if (endpoint.samplerate == kOutputRate && endpoint.channels == kOutputChannels)
		{
			const uint32 firstFrames = std::min<uint32>(convertedFrames,
				kMaxQueuedFrames - static_cast<uint32>(endpoint.writeFrame));
			std::memcpy(endpoint.samples.data() + endpoint.writeFrame * kOutputChannels, input,
				static_cast<size_t>(firstFrames) * kOutputChannels * sizeof(sint16));
			const uint32 remainingFrames = convertedFrames - firstFrames;
			if (remainingFrames > 0)
				std::memcpy(endpoint.samples.data(), input + firstFrames * kOutputChannels,
					static_cast<size_t>(remainingFrames) * kOutputChannels * sizeof(sint16));
			endpoint.writeFrame = (endpoint.writeFrame + convertedFrames) % kMaxQueuedFrames;
			endpoint.queuedFrames += convertedFrames;
			if (wasEmpty)
				endpoint.frontBlockFrames = convertedFrames;
			return convertedFrames;
		}

		uint32 sourceFrame = 0;
		uint32 sourcePhase = 0;
		for (uint32 frame = 0; frame < convertedFrames; ++frame)
		{
			const sint16* source = input + static_cast<size_t>(sourceFrame) * endpoint.channels;
			sint64 left = source[0];
			sint64 right = endpoint.channels > 1 ? source[1] : source[0];
			if (endpoint.channels >= 6)
			{
				left += (static_cast<sint64>(source[2]) * 707) / 1000;
				right += (static_cast<sint64>(source[2]) * 707) / 1000;
				left += source[3] / 2;
				right += source[3] / 2;
				left += (static_cast<sint64>(source[4]) * 707) / 1000;
				right += (static_cast<sint64>(source[5]) * 707) / 1000;
			}
			else if (endpoint.channels >= 4)
			{
				left += (static_cast<sint64>(source[2]) * 707) / 1000;
				right += (static_cast<sint64>(source[3]) * 707) / 1000;
			}
			const size_t sample = endpoint.writeFrame * kOutputChannels;
			endpoint.samples[sample] = static_cast<sint16>(std::clamp<sint64>(left, -32768, 32767));
			endpoint.samples[sample + 1] = static_cast<sint16>(std::clamp<sint64>(right, -32768, 32767));
			endpoint.writeFrame = (endpoint.writeFrame + 1) % kMaxQueuedFrames;

			sourcePhase += endpoint.samplerate;
			while (sourcePhase >= kOutputRate && sourceFrame + 1 < endpoint.samplesPerBlock)
			{
				sourcePhase -= kOutputRate;
				++sourceFrame;
			}
		}
		endpoint.queuedFrames += convertedFrames;
		if (wasEmpty)
			endpoint.frontBlockFrames = convertedFrames;
		return convertedFrames;
	}

	SwitchAudioEndpoint* FindLeader() const
	{
		for (const auto type : {IAudioAPI::AudioType::TV, IAudioAPI::AudioType::Gamepad,
		                        IAudioAPI::AudioType::Portal})
		{
			for (auto* endpoint : m_endpoints)
				if (endpoint->playing && endpoint->type == type && endpoint->queuedFrames > 0)
					return endpoint;
		}
		return nullptr;
	}

	SwitchAudioEndpoint* FindSingleEndpoint() const
	{
		SwitchAudioEndpoint* single = nullptr;
		for (auto* endpoint : m_endpoints)
		{
			if (!endpoint->playing)
				continue;
			if (single)
				return nullptr;
			single = endpoint;
		}
		return single;
	}

	bool MainStreamsReady(uint32 frames) const
	{
		uint32 largestQueued = 0;
		bool missing = false;
		for (auto* endpoint : m_endpoints)
		{
			if (!endpoint->playing || (endpoint->type != IAudioAPI::AudioType::TV &&
			                          endpoint->type != IAudioAPI::AudioType::Gamepad))
				continue;
			largestQueued = std::max(largestQueued, endpoint->queuedFrames);
			missing |= endpoint->queuedFrames < frames;
		}
		return !missing || largestQueued >= frames * 2;
	}

	void Pump()
	{
		if (!m_started || !m_applicationActive)
			return;
		while (!m_freeBuffers.empty())
		{
			SwitchAudioEndpoint* leader = FindLeader();
			if (!leader)
				return;
			const uint32 frames = std::min(FrontFrames(*leader), kOutputFramesPerBuffer);
			if (frames == 0 || !MainStreamsReady(frames))
				return;

			AudioOutBuffer* buffer = m_freeBuffers.front();
			m_freeBuffers.pop_front();
			auto* output = static_cast<sint16*>(buffer->buffer);
			SwitchAudioEndpoint* single = FindSingleEndpoint();
			if (single == leader)
			{
				ConsumeFramesSingle(*single, output, frames);
			}
			else
			{
				for (uint32 frame = 0; frame < frames; ++frame)
				{
					sint32 mixedLeft = 0;
					sint32 mixedRight = 0;
					for (auto* endpoint : m_endpoints)
					{
						if (!endpoint->playing)
							continue;
						sint16 left = 0;
						sint16 right = 0;
						if (!ConsumeFrame(*endpoint, left, right))
							continue;
						mixedLeft += static_cast<sint32>(left) * endpoint->volume;
						mixedRight += static_cast<sint32>(right) * endpoint->volume;
					}
					mixedLeft /= 100;
					mixedRight /= 100;
					output[frame * 2] = static_cast<sint16>(std::clamp<sint32>(mixedLeft, -32768, 32767));
					output[frame * 2 + 1] = static_cast<sint16>(std::clamp<sint32>(mixedRight, -32768, 32767));
				}
			}

			buffer->data_size = static_cast<size_t>(frames) * kOutputChannels * sizeof(sint16);
			buffer->data_offset = 0;
			armDCacheFlush(buffer->buffer, buffer->data_size);
			if (R_FAILED(audoutAppendAudioOutBuffer(buffer)))
			{
				m_freeBuffers.push_front(buffer);
				return;
			}
		}
	}

	std::array<AudioOutBuffer, kSwitchBlockCount> m_buffers{};
	std::array<void*, kSwitchBlockCount> m_memory{};
	std::deque<AudioOutBuffer*> m_freeBuffers;
	std::vector<SwitchAudioEndpoint*> m_endpoints;
	std::mutex m_mutex;
	UEvent m_wake{};
	Event m_bufferEvent{};
	std::thread m_worker;
	bool m_started{};
	bool m_applicationActive{true};
	bool m_workerShutdown{};
};

std::unique_ptr<SwitchAudioMixer> s_mixer;
bool s_audioInitialized{};
} // namespace

struct SwitchAudioAPI::Impl
{
	SwitchAudioEndpoint endpoint;
};

SwitchAudioAPI::SwitchAudioAPI(uint32 samplerate, uint32 channels, uint32 samples_per_block,
                               uint32 bits_per_sample, AudioType type)
	: IAudioAPI(samplerate, channels, samples_per_block, bits_per_sample), m_impl(std::make_unique<Impl>())
{
	if (!s_mixer || samplerate == 0 || channels == 0 || samples_per_block == 0 || bits_per_sample != 16)
		throw std::invalid_argument("unsupported Switch audio format");
	m_impl->endpoint.type = type;
	m_impl->endpoint.samplerate = samplerate;
	m_impl->endpoint.channels = channels;
	m_impl->endpoint.samplesPerBlock = samples_per_block;
	m_impl->endpoint.framesPerBlock = static_cast<uint32>(
		(static_cast<uint64>(samples_per_block) * kOutputRate + samplerate - 1) / samplerate);
	s_mixer->Add(&m_impl->endpoint);
}

SwitchAudioAPI::~SwitchAudioAPI()
{
	if (s_mixer)
		s_mixer->Remove(&m_impl->endpoint);
}

bool SwitchAudioAPI::NeedAdditionalBlocks() const
{
	return s_mixer && s_mixer->NeedsBlocks(m_impl->endpoint, GetAudioDelay());
}

bool SwitchAudioAPI::FeedBlock(sint16* data)
{
	return s_mixer && s_mixer->Feed(m_impl->endpoint, data);
}

bool SwitchAudioAPI::Play()
{
	if (!s_mixer)
		return false;
	m_playing = s_mixer->Play(m_impl->endpoint);
	return m_playing;
}

bool SwitchAudioAPI::Stop()
{
	if (!s_mixer)
		return true;
	const bool success = s_mixer->Stop(m_impl->endpoint);
	m_playing = false;
	return success;
}

void SwitchAudioAPI::SetVolume(sint32 volume)
{
	IAudioAPI::SetVolume(std::clamp(volume, 0, 100));
	if (s_mixer)
		s_mixer->SetVolume(m_impl->endpoint, m_volume);
}

bool SwitchAudioAPI::InitializeStatic()
{
	if (s_audioInitialized)
		return true;
	if (R_FAILED(audoutInitialize()))
		return false;
	if (audoutGetSampleRate() != kOutputRate || audoutGetChannelCount() != kOutputChannels ||
	    audoutGetPcmFormat() != PcmFormat_Int16)
	{
		audoutExit();
		return false;
	}

	auto mixer = std::make_unique<SwitchAudioMixer>();
	if (!mixer->Initialize())
	{
		audoutExit();
		return false;
	}
	s_mixer = std::move(mixer);
	s_audioInitialized = true;
	return true;
}

void SwitchAudioAPI::SetApplicationActive(bool active)
{
	if (s_mixer)
		s_mixer->SetApplicationActive(active);
}

void SwitchAudioAPI::Destroy()
{
	if (!s_audioInitialized)
		return;
	if (s_mixer)
		s_mixer->Shutdown();
	audoutExit();
	s_mixer.reset();
	s_audioInitialized = false;
}

std::vector<IAudioAPI::DeviceDescriptionPtr> SwitchAudioAPI::GetDevices()
{
	std::vector<DeviceDescriptionPtr> devices;
	devices.emplace_back(std::make_shared<SwitchDeviceDescription>(L"Switch Audio Output"));
	return devices;
}
