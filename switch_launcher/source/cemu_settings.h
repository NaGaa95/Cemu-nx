#pragma once

// The renderer the launcher boots when none is chosen: the native deko3d core (Drako3D on the Switch 2).
#define LAUNCHER_DEFAULT_RENDERER "deko3d"
#include <string>
#include <vector>
#include <cstdint>

struct CemuKV { std::string k, v; };
const char *cemuKVGet(const std::vector<CemuKV> &kv, const char *key, const char *def);

// An empty preset list enables the pack defaults.
struct CemuPackPreset { std::string category; std::string preset; };
struct CemuGraphicPack { std::string rulesRel; std::vector<CemuPackPreset> presets; };

// Settings not owned by the launcher are preserved.
bool cemu_writeSettingsXml(const char *path, const std::vector<CemuKV> &s,
                           const std::vector<std::string> &gamePaths,
                           const std::vector<CemuGraphicPack> &enabledPacks);

bool cemu_readAccountSelection(const char *path, uint32_t &persistentId,
                               int &service);
bool cemu_readAccountService(const char *path, uint32_t persistentId,
                             int &service);
bool cemu_writeAccountSelection(const char *path, uint32_t persistentId,
                                int service);

// A zero title ID does not produce a profile.
bool cemu_writeGameProfile(const char *dir, uint64_t titleId, const char *gameName,
                           const std::vector<CemuKV> &s);
