#pragma once

enum class SwitchGamePadLayout
{
	Disabled,
	PadOnly,
	Right,
	Left,
	Below,
	Above,
};

bool SwitchPlatform_UseTripleBuffer();
void SwitchPlatform_BeginGameBootBoost();
void SwitchPlatform_ArmGameBootBoostHandoff();
void SwitchPlatform_EndGameBootBoost();
void SwitchPlatform_NotifyGameFrameSubmitted();
void SwitchPlatform_PollGameBootState();
#ifdef __SWITCH2__
bool SwitchPlatform_IsCpuBoostEnabled();
void SwitchPlatform_ToggleCpuBoost();
#endif
void SwitchPlatform_ApplyWindowSurfaceSize(int width, int height);
// The display's refresh rate: 60 on the Switch; on the Switch 2 120 handheld, and docked with frame generation.
int SwitchPlatform_DisplayRefreshRate();
// Switch 2 present pacing: per frame, with reapply after Mesa or drako3d set their own swap interval. No-op on Switch.
void SwitchPlatform_ApplyDisplayPacing(int swapInterval, bool reapply);
bool SwitchPlatform_ShouldReturnToLauncher();
void SwitchPlatform_SetGamePadLayout(SwitchGamePadLayout layout);
SwitchGamePadLayout SwitchPlatform_GetGamePadLayout();
void SwitchPlatform_CycleGamePadMode();
bool SwitchPlatform_TakeGamePadLayoutChange();
// Sets the layout while GameShare holds it (SwitchPlatform_SetGamePadLayout leaves it alone then)
void SwitchPlatform_ForceGamePadLayout(SwitchGamePadLayout layout);
// GameShare's guest screen: the renderer's second swapchain on the guest's window (SwitchRenderer.cpp). The OpenGL
// renderer cannot present there, and the guest then mirrors the host's screen.
bool SwitchRenderer_SupportsGuestScreen();
bool SwitchRenderer_OpenGuestScreen(void* window, int width, int height);
void SwitchRenderer_CloseGuestScreen();
bool SwitchPlatform_IsGamePadOutputActive();
bool SwitchPlatform_IsGamePadOnly();
bool SwitchPlatform_IsGamePadCompositeActive();
void SwitchPlatform_GetGamePadViewRegion(bool padView, int fullWidth, int fullHeight,
	int& x, int& y, int& width, int& height);
void SwitchPlatform_GetGamePadImageRect(bool padView, int fullWidth, int fullHeight,
	int sourceWidth, int sourceHeight, int& x, int& y, int& width, int& height);
