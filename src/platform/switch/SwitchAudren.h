// SPDX-FileCopyrightText: 2026 PalindromicBreadLoaf
// SPDX-License-Identifier: GPL-3.0-or-later
// From Kankohi (src/Common/switch/HorizonAudio.h), adapted for Cemu-nx.

#pragma once

#include <cstdint>

namespace SwitchAudren
{
	struct Stream;

	bool Init();
	void Shutdown();

	void Update();

	// While inactive (the application is in the background) every voice is paused
	void SetActive(bool active);
	bool IsActive();

	// Takes 1, 2, 4, or 6 channels and downmixes them to stereo
	Stream* OpenStream(uint32_t sampleRate, uint32_t channels, uint32_t framesPerBlock, uint32_t blockCount);
	void CloseStream(Stream* stream);

	bool QueueBlock(Stream* stream, const int16_t* samples);
	uint32_t GetQueuedBlocks(const Stream* stream);
	void SetPlaying(Stream* stream, bool playing);
	void SetVolume(Stream* stream, float volume);
}
