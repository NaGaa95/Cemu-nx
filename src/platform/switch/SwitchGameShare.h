#pragma once

// GamePad GameShare (Switch 2): another Nintendo Switch or Switch 2 console joins a local GameShare session as the
// Wii U GamePad, or as the TV player when the roles are swapped. It gets that screen and its audio, its controller,
// touch screen and motion drive that player, and it receives that player's rumble. On Switch these are stubs.

#include <cstdint>
#include <string>

struct SwitchGameShareInput
{
	uint64_t buttons = 0;  // HidNpadButton_* bits
	int32_t stick[4] = {}; // left x/y, right x/y (HidAnalogStickState range)
	// Touch in kSwitchGameShareGuestWidth x kSwitchGameShareGuestHeight
	bool touchDown = false;
	int32_t touchX = 0;
	int32_t touchY = 0;
	// Gyro and accelerometer in HidSixAxisSensorState units
	bool hasMotion = false;
	uint64_t motionSampling = 0;
	float gyro[3] = {};
	float accel[3] = {};
};

enum class SwitchGameShareResult
{
	Started,
	Cancelled,
	Failed,
};

constexpr int kSwitchGameShareGuestWidth = 1280;
constexpr int kSwitchGameShareGuestHeight = 720;

void SwitchGameShare_Configure(bool requested, bool swapRoles);
// The launcher asked for this game to run with GameShare
bool SwitchGameShare_Requested();
// Off: the guest is the GamePad. On: the guest is the TV player and the host shows the GamePad.
bool SwitchGameShare_SwapRoles();
// Changes the roles during a session (the quick menu's GameShare screen); the caller reassigns the players
void SwitchGameShare_SetSwapRoles(bool swapRoles);
// Gives the guest a screen of its own when the renderer can present to a second window (otherwise the guest mirrors
// the host's screen), shows the system matchmaking applet and starts streaming. Call after the renderer exists and
// before the game starts.
SwitchGameShareResult SwitchGameShare_Begin(std::string& error);
// Once per frame on the main thread: the guest connection state
void SwitchGameShare_Update();
// Ends the session and closes the guest's screen. Call once the game has stopped.
void SwitchGameShare_End();
// From a successful Begin until End
bool SwitchGameShare_Active();
// The guest has a screen of its own
bool SwitchGameShare_SeparateActive();
// The guest hears this stream (the GamePad's, or the TV's when the roles are swapped) instead of the host
bool SwitchGameShare_IsGuestAudio(bool tvStream);
// The guest's audio: 48 kHz stereo s16 frames at volume (0 to 100)
void SwitchGameShare_WriteGuestAudio(const int16_t* frames, uint32_t frameCount, int32_t volume);
// The guest's latest controller, touch and motion sample; false while no guest sample is available
bool SwitchGameShare_GetGuestInput(SwitchGameShareInput& out);
// The guest's buttons (HidNpadButton_* bits), read for the quick menu
uint64_t SwitchGameShare_GetGuestButtons();
// Rumbles the guest's controller (0 to 1)
void SwitchGameShare_SetGuestRumble(float amplitude);
