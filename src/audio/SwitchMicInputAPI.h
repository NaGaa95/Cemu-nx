#pragma once

#include "IAudioInputAPI.h"

// The console's microphone (audin) as the GamePad microphone.
class SwitchMicInputAPI : public IAudioInputAPI
{
public:
	class SwitchMicDeviceDescription : public DeviceDescription
	{
	public:
		SwitchMicDeviceDescription() : DeviceDescription(L"Console microphone") {}
		std::wstring GetIdentifier() const override { return L"default"; }
	};

	SwitchMicInputAPI(uint32 samplerate, uint32 channels, uint32 samples_per_block, uint32 bits_per_sample);
	~SwitchMicInputAPI() override;

	AudioInputAPI GetType() const override { return Switch; }

	bool ConsumeBlock(sint16* data) override;
	bool Play() override;
	bool Stop() override;
	bool IsPlaying() const override;

	static bool InitializeStatic();
	static std::vector<DeviceDescriptionPtr> GetDevices();
};
