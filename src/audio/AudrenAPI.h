// SPDX-FileCopyrightText: 2026 PalindromicBreadLoaf
// SPDX-License-Identifier: GPL-3.0-or-later
// From Kankohi (src/audio/AudrenAPI.h), adapted for Cemu-nx.

#pragma once

#include "IAudioAPI.h"

namespace SwitchAudren
{
	struct Stream;
}

class AudrenAPI : public IAudioAPI
{
public:
	class AudrenDeviceDescription : public DeviceDescription
	{
	public:
		AudrenDeviceDescription()
			: DeviceDescription(L"Default Device") { }

		std::wstring GetIdentifier() const override { return L"default"; }
	};

	AudrenAPI(uint32 samplerate, uint32 channels, uint32 samples_per_block, uint32 bits_per_sample,
			  AudioType type = AudioType::TV);
	~AudrenAPI() override;

	AudioAPI GetType() const override { return Audren; }
	bool NeedAdditionalBlocks() const override;
	bool FeedBlock(sint16* data) override;
	bool Play() override;
	bool Stop() override;
	void SetVolume(sint32 volume) override;

	static std::vector<DeviceDescriptionPtr> GetDevices();

	// Starts the audio renderer; false if it is not available
	static bool InitializeStatic();
	static void SetApplicationActive(bool active);
	static void Destroy();

private:
	bool IsGameShareGuestStream() const;
	bool UpdateGuestRouting();
	SwitchAudren::Stream* m_stream = nullptr;
	AudioType m_type;
	bool m_routedToGuest = false;
};
