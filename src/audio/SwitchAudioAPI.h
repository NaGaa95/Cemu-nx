#pragma once

#include "IAudioAPI.h"

class SwitchAudioAPI : public IAudioAPI
{
  public:
	class SwitchDeviceDescription : public DeviceDescription
	{
	  public:
		explicit SwitchDeviceDescription(std::wstring name) : DeviceDescription(std::move(name)) {}
		std::wstring GetIdentifier() const override { return L"default"; }
	};

	SwitchAudioAPI(uint32 samplerate, uint32 channels, uint32 samples_per_block, uint32 bits_per_sample,
	               AudioType type = AudioType::TV);
	~SwitchAudioAPI() override;

	AudioAPI GetType() const override { return SwitchAudio; }

	bool NeedAdditionalBlocks() const override;
	bool FeedBlock(sint16* data) override;
	bool Play() override;
	bool Stop() override;
	void SetVolume(sint32 volume) override;

	static bool InitializeStatic();
	static void SetApplicationActive(bool active);
	static void Destroy();
	static std::vector<DeviceDescriptionPtr> GetDevices();

  private:
	bool IsGameShareGuestStream() const;
	bool UpdateGuestRouting();
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
