#pragma once

struct SwitchLSFGDekoConfig
{
	unsigned flowDenominator = 4;
	bool performance = true;
};

SwitchLSFGDekoConfig SwitchLSFGDeko_GetConfig();
void SwitchLSFGDeko_SetAvailable(bool available);
bool SwitchLSFGDeko_IsPreparing();
void SwitchLSFGDeko_SetPreparing(bool preparing);
void SwitchLSFGDeko_SetHighRate(bool highRate);
