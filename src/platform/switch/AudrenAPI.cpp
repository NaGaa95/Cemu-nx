// SPDX-FileCopyrightText: 2026 PalindromicBreadLoaf
// SPDX-License-Identifier: GPL-3.0-or-later
// From Kankohi (src/audio/AudrenAPI.cpp), adapted for Cemu-nx.

#include "audio/AudrenAPI.h"
#include "platform/switch/SwitchAudren.h"
#include "platform/switch/SwitchThread.h"
#include "util/helpers/helpers.h"

#include <atomic>
#include <mutex>
#include <stdexcept>
#include <thread>

static std::mutex s_rendererMutex;
static std::thread s_outputThread;
static std::atomic_bool s_stopOutput = false;

static void AudioOutputThread()
{
	SetThreadName("audren");
	SwitchThread_PinToHelperCore("audio renderer", 43);
	while (!s_stopOutput.load(std::memory_order_relaxed))
		SwitchAudren::Update();
}

static bool EnsureRendererStarted()
{
	std::scoped_lock lock(s_rendererMutex);
	if (s_outputThread.joinable())
		return true;
	if (!SwitchAudren::Init())
		return false;
	s_stopOutput = false;
	s_outputThread = std::thread(AudioOutputThread);
	return true;
}

AudrenAPI::AudrenAPI(uint32 samplerate, uint32 channels, uint32 samples_per_block, uint32 bits_per_sample)
	: IAudioAPI(samplerate, channels, samples_per_block, bits_per_sample)
{
	if (bits_per_sample != 16)
		throw std::runtime_error(fmt::format("audren can't play {}-bit samples", bits_per_sample));
	if (!EnsureRendererStarted())
		throw std::runtime_error("can't start the audio renderer");
	m_stream = SwitchAudren::OpenStream(samplerate, channels, samples_per_block, kBlockCount);
	if (!m_stream)
		throw std::runtime_error("can't open an audren voice");
}

AudrenAPI::~AudrenAPI()
{
	SwitchAudren::CloseStream(m_stream);
}

bool AudrenAPI::NeedAdditionalBlocks() const
{
	return SwitchAudren::IsActive() && SwitchAudren::GetQueuedBlocks(m_stream) < GetAudioDelay();
}

bool AudrenAPI::FeedBlock(sint16* data)
{
	if (!SwitchAudren::IsActive())
		return true; // dropped while the application is in the background
	if (SwitchAudren::QueueBlock(m_stream, data))
		return true;
	cemuLog_logDebug(LogType::Force, "dropped audren block since too many buffers are queued");
	return false;
}

bool AudrenAPI::Play()
{
	SwitchAudren::SetPlaying(m_stream, true);
	m_playing = true;
	return true;
}

bool AudrenAPI::Stop()
{
	SwitchAudren::SetPlaying(m_stream, false);
	m_playing = false;
	return true;
}

void AudrenAPI::SetVolume(sint32 volume)
{
	IAudioAPI::SetVolume(volume);
	SwitchAudren::SetVolume(m_stream, (float)volume / 100.0f);
}

std::vector<IAudioAPI::DeviceDescriptionPtr> AudrenAPI::GetDevices()
{
	return {std::make_shared<AudrenDeviceDescription>()};
}

bool AudrenAPI::InitializeStatic()
{
	return EnsureRendererStarted();
}

void AudrenAPI::SetApplicationActive(bool active)
{
	SwitchAudren::SetActive(active);
}

void AudrenAPI::Destroy()
{
	std::scoped_lock lock(s_rendererMutex);
	if (!s_outputThread.joinable())
		return;
	s_stopOutput = true;
	s_outputThread.join();
	SwitchAudren::Shutdown();
}
