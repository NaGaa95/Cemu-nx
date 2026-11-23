#pragma once

#include <cstdint>

void SwitchOverlay_Initialize();
void SwitchOverlay_UpdateInput(std::uint64_t down, std::uint64_t held);
void SwitchOverlay_Render();
bool SwitchOverlay_IsInputCaptured(std::uint64_t held);
bool SwitchOverlay_TakeAmiiboScanRequest();
bool SwitchOverlay_TakeExitRequest();
// Times the GameShare screen was toggled since the last call
int SwitchOverlay_TakeGameShareScreenRequests();
