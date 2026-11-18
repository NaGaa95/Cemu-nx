#pragma once

#include <string_view>

// The native renderer's name: drako3d on the Switch 2 (its settings value stays "deko3d").
#if defined(__SWITCH2__)
#define DEKO3D_NAME "Drako3D"
#define DEKO_NAME "Drako"
#else
#define DEKO3D_NAME "Deko3D"
#define DEKO_NAME "Deko"
#endif

namespace LauncherLocalization
{
void Initialize(std::string_view preference);
std::string_view Translate(std::string_view source);
std::string_view Preference();
std::string_view ActiveLanguage();
}
