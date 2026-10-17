#include "platform/switch/SwitchLSFG.h"
#include "platform/switch/SwitchLSFGDeko.h"

#include <atomic>
#include <cstdio>

namespace
{
	std::atomic_bool s_prepared{false};
	std::atomic_bool s_available{false};
	std::atomic_bool s_requested{false};
	std::atomic_bool s_highRate{false};
	std::atomic_bool s_preparing{false};
	SwitchLSFGDekoConfig s_config;
} // namespace

void SwitchLSFG_Configure(bool prepared, float flowScale, bool performanceMode)
{
	FILE* file = prepared ? std::fopen("sdmc:/switch/cemu/lsfg/Lossless.dll", "rb") : nullptr;
	s_prepared.store(file != nullptr, std::memory_order_release);
	if (file)
		std::fclose(file);
	s_config = {flowScale == 0.5f ? 2U : 4U, performanceMode};
	s_requested.store(false, std::memory_order_release);
	s_available.store(file != nullptr, std::memory_order_release);
	s_highRate.store(false, std::memory_order_release);
	s_preparing.store(false, std::memory_order_release);
}

bool SwitchLSFG_IsPrepared()
{
	return s_prepared.load(std::memory_order_acquire);
}
bool SwitchLSFG_IsAvailable()
{
	return s_available.load(std::memory_order_acquire);
}
bool SwitchLSFG_IsEnabled()
{
	return s_requested.load(std::memory_order_acquire);
}
bool SwitchLSFG_IsHighRatePassthrough()
{
	return s_highRate.load(std::memory_order_acquire);
}

void SwitchLSFG_RequestEnabled(bool enabled)
{
	s_requested.store(enabled && SwitchLSFG_IsAvailable(), std::memory_order_release);
}

SwitchLSFGDekoConfig SwitchLSFGDeko_GetConfig()
{
	return s_config;
}
bool SwitchLSFGDeko_IsPreparing()
{
	return s_preparing.load(std::memory_order_acquire);
}
void SwitchLSFGDeko_SetPreparing(bool preparing)
{
	s_preparing.store(preparing, std::memory_order_release);
}
void SwitchLSFGDeko_SetAvailable(bool available)
{
	s_available.store(available, std::memory_order_release);
	if (!available)
		s_requested.store(false, std::memory_order_release);
}
void SwitchLSFGDeko_SetHighRate(bool highRate)
{
	s_highRate.store(highRate, std::memory_order_release);
}
