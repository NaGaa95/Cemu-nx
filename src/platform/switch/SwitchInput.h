#pragma once

enum class SwitchControllerType
{
	GamePad,
	Pro,
	Classic,
	Wiimote,
	WiimoteNunchuk,
	Count,
};

void SwitchInput_Setup();
void SwitchInput_Shutdown();
bool SwitchInput_SetControllerType(SwitchControllerType type);
SwitchControllerType SwitchInput_GetControllerType();
