#pragma once

#include <string_view>

namespace LauncherLocalization
{
void Initialize(std::string_view preference);
std::string_view Translate(std::string_view source);
std::string_view Preference();
std::string_view ActiveLanguage();
}
