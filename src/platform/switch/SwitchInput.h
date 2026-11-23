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
// The Joy-Con mouse's pointer while it is in use (Switch 2), from the overlay's ImGui frame.
void SwitchInput_DrawJoyConMouse();
bool SwitchInput_SetControllerType(SwitchControllerType type);
SwitchControllerType SwitchInput_GetControllerType();
bool SwitchInput_SetSecondaryControllerType(SwitchControllerType type);
SwitchControllerType SwitchInput_GetSecondaryControllerType();
// GameShare's roles changed: the guest and the console's controllers move to their new players
void SwitchInput_ApplyGameShareRoles();
