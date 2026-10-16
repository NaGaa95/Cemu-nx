#include "ui_audio.h"

#include <switch.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace
{
constexpr int SampleRate = 48000;
constexpr size_t BufferCount = 4;
constexpr size_t BufferSize = 0x3000;
struct alignas(0x1000) SampleBuffer
{
	std::array<uint8_t, BufferSize> samples;
};
std::array<SampleBuffer, BufferCount> s_samples;
std::array<AudioOutBuffer, BufferCount> s_buffers{};
std::array<bool, BufferCount> s_queued{};
bool s_initialized = false;
bool s_enabled = true;
std::vector<int16_t> s_navigate;
std::vector<int16_t> s_confirm;
std::vector<int16_t> s_back;

std::vector<int16_t> makeTone(float startHz, float endHz, int milliseconds, float volume)
{
	const int frames = SampleRate * milliseconds / 1000;
	std::vector<int16_t> samples(static_cast<size_t>(frames) * 2);
	double phase = 0.0;
	for (int frame = 0; frame < frames; ++frame)
	{
		const float progress = frames > 1 ? static_cast<float>(frame) / (frames - 1) : 0.0f;
		const float frequency = startHz + (endHz - startHz) * progress;
		phase += 6.283185307179586 * frequency / SampleRate;
		const float attack = std::min(1.0f, progress * 10.0f);
		const float release = std::max(0.0f, 1.0f - progress);
		const float envelope = attack * release * release;
		const int16_t value = static_cast<int16_t>(std::sin(phase) * envelope * volume * 32767.0f);
		samples[static_cast<size_t>(frame) * 2] = value;
		samples[static_cast<size_t>(frame) * 2 + 1] = value;
	}
	return samples;
}

const std::vector<int16_t>& samplesFor(UiSound sound)
{
	switch (sound)
	{
	case UiSound::Navigate: return s_navigate;
	case UiSound::Confirm: return s_confirm;
	case UiSound::Back: return s_back;
	}
	return s_navigate;
}

void closeOutput()
{
	if (!s_initialized)
		return;
	audoutStopAudioOut();
	audoutExit();
	s_initialized = false;
	s_queued.fill(false);
}

bool reclaimBuffers()
{
	for (size_t index = 0; index < BufferCount; ++index)
	{
		AudioOutBuffer* released = nullptr;
		u32 count = 0;
		const Result result = audoutGetReleasedAudioOutBuffer(&released, &count);
		if (R_FAILED(result))
			return false;
		if (!count || !released)
			break;
		for (size_t slot = 0; slot < BufferCount; ++slot)
			if (released == &s_buffers[slot]) s_queued[slot] = false;
	}
	return true;
}
} // namespace

bool uiAudioInit()
{
	if (s_initialized)
		return true;
	if (R_FAILED(audoutInitialize()))
		return false;
	if (audoutGetSampleRate() != SampleRate || audoutGetChannelCount() != 2 ||
	    audoutGetPcmFormat() != PcmFormat_Int16)
	{
		audoutExit();
		return false;
	}
	for (size_t index = 0; index < BufferCount; ++index)
	{
		s_buffers[index] = {};
		s_buffers[index].buffer = s_samples[index].samples.data();
		s_buffers[index].buffer_size = BufferSize;
	}
	s_queued.fill(false);
	if (R_FAILED(audoutStartAudioOut()))
	{
		audoutExit();
		return false;
	}
	s_initialized = true;
	s_navigate = makeTone(920.0f, 1040.0f, 18, 0.10f);
	s_confirm = makeTone(620.0f, 980.0f, 42, 0.16f);
	s_back = makeTone(760.0f, 420.0f, 48, 0.14f);
	return true;
}

void uiAudioPlay(UiSound sound)
{
	if (!s_initialized || !s_enabled)
		return;
	if (!reclaimBuffers())
		return;
	const auto& samples = samplesFor(sound);
	const size_t bytes = samples.size() * sizeof(samples[0]);
	if (!bytes || bytes > BufferSize)
		return;
	for (size_t index = 0; index < BufferCount; ++index)
	{
		if (s_queued[index])
			continue;
		auto& buffer = s_buffers[index];
		std::memcpy(buffer.buffer, samples.data(), bytes);
		armDCacheFlush(buffer.buffer, bytes);
		buffer.data_size = bytes;
		const Result result = audoutAppendAudioOutBuffer(&buffer);
		s_queued[index] = R_SUCCEEDED(result);
		return;
	}
}

void uiAudioSetEnabled(bool enabled)
{
	if (s_enabled == enabled)
		return;
	s_enabled = enabled;
	if (enabled)
		uiAudioInit();
	else
		closeOutput();
}

void uiAudioShutdown()
{
	closeOutput();
	s_navigate.clear();
	s_confirm.clear();
	s_back.clear();
}
