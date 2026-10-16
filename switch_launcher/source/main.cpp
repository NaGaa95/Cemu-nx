#include "SwitchAddressSpace.h"
#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_image.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <map>
#include <unordered_map>
#include <iterator>
#include <initializer_list>
#include <array>
#include <climits>
#include <string>
#include <vector>
#include <deque>
#include <algorithm>
#include <functional>
#include <atomic>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <memory>
#include <unordered_set>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <dirent.h>

#include "griddb.h"
#include "cemu_container_title.h"
#include "cemu_titles.h"
#include "cemu_settings.h"
#include "gfxpacks.h"
#include "install.h"
#include "forwarder.h"
#include "launcher_update.h"
#include "localization.h"
#include "SwitchStorage.h"
#include "ui_audio.h"

static const char *tr(const char *text) {
  return LauncherLocalization::Translate(text ? text : "").data();
}
static std::string uiText(const char *text) {
  return std::string(LauncherLocalization::Translate(text ? text : ""));
}

static void drawToastOverlay();
static void pumpCoverDecodeResults();
static void cancelQueuedCoverDecodes();
static void stopCoverDecodeWorker();
static void presentUi(SDL_Renderer *renderer) { drawToastOverlay(); SDL_RenderPresent(renderer); }
#define SDL_RenderPresent presentUi

// SDL uses Xbox button names.
#define BTN_CONFIRM  SDL_CONTROLLER_BUTTON_B
#define BTN_CANCEL   SDL_CONTROLLER_BUTTON_A
#define BTN_X        SDL_CONTROLLER_BUTTON_Y
#define BTN_Y        SDL_CONTROLLER_BUTTON_X

static const char *DATA_DIR    = "sdmc:/switch/cemu";
static const char *EMU_HOST_DIR= "sdmc:/switch/cemu/.emu";
static const char *LAUNCHER_INI= "sdmc:/switch/cemu/launcher.ini";
static const char *COVERS_DIR  = "sdmc:/switch/cemu/covers";
static const char *GAMECFG_DIR = "sdmc:/switch/cemu/gamecfg";
static const char *DEF_GAMEDIR = "sdmc:/switch/cemu/games";
static const char *SETTINGS_XML= "sdmc:/switch/cemu/settings.xml";
static const char *GAMEPROFILES_DIR = "sdmc:/switch/cemu/gameProfiles";
static const char *GRAPHICPACKS_DIR = "sdmc:/switch/cemu/graphicPacks";
static const char *LSFG_DIR = "sdmc:/switch/cemu/lsfg";
static const char *LSFG_DLL_FILE = "sdmc:/switch/cemu/lsfg/Lossless.dll";
static const char *LAUNCHER_NRO = "sdmc:/switch/cemu/cemu.nro";
static std::string g_launcherNroPath = LAUNCHER_NRO;
static const char *EMU_NRO_SRC = "romfs:/emu/cemu_core.nro";
static const char *EMU_HASH_SRC = "romfs:/emu/cemu_core.sha256";
static const char *EMU_NRO_DST = "sdmc:/switch/cemu/.emu/cemu_core.nro";
static const char *LAUNCH_HANDOFF = "sdmc:/switch/cemu/switch.ini";

struct KV { std::string k, v; };
struct Store {
  std::vector<KV> kv;
  mutable std::unordered_map<std::string,size_t> index;
  mutable size_t indexedSize=SIZE_MAX;
};

static Store g_global;
static Store g_game;
static Store g_titles;
static Store g_containerTitles;
static Store g_gameIdentities;
static Store *g_active = &g_global;
static int g_systemCemuLanguage = 1;
static const char *g_systemLanguageName = "English";
static const char *TITLES_INI = "sdmc:/switch/cemu/titles.ini";
static const char *CONTAINER_TITLES_INI = "sdmc:/switch/cemu/container_titles.ini";
static const char *GAME_IDENTITIES_INI = "sdmc:/switch/cemu/game_identities.ini";

static std::string trim(const std::string &s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

static void setLauncherPathFromArg(const char *path) {
  if(!path||!path[0]) return;
  std::string candidate=path;
  std::replace(candidate.begin(),candidate.end(),'\\','/');
  if(candidate.rfind("sdmc:/",0)!=0||candidate.find("/../")!=std::string::npos||
     candidate.find("/./")!=std::string::npos) return;
  std::string lowercase=candidate;
  std::transform(lowercase.begin(),lowercase.end(),lowercase.begin(),
    [](unsigned char value){ return (char)std::tolower(value); });
  if(lowercase.size()<4||lowercase.compare(lowercase.size()-4,4,".nro")!=0) return;
  g_launcherNroPath=std::move(candidate);
}
static void ensureStoreIndex(const Store &s) {
  if(s.indexedSize==s.kv.size())return;
  s.index.clear();s.index.reserve(s.kv.size());
  for(size_t item=0;item<s.kv.size();item++)s.index[s.kv[item].k]=item;
  s.indexedSize=s.kv.size();
}
static void invalidateStoreIndex(Store &s) {
  s.index.clear();s.indexedSize=SIZE_MAX;
}
static const char *storeGet(Store &s, const char *key, const char *def) {
  ensureStoreIndex(s);
  const auto found=s.index.find(key);
  return found==s.index.end()?def:s.kv[found->second].v.c_str();
}
static void storeSet(Store &s, const char *key, const char *val) {
  ensureStoreIndex(s);
  const auto found=s.index.find(key);
  if(found!=s.index.end()){s.kv[found->second].v=val;return;}
  s.kv.push_back({key,val});s.index[s.kv.back().k]=s.kv.size()-1;s.indexedSize=s.kv.size();
}
static void storeRemove(Store &s, const char *key) {
  ensureStoreIndex(s);const auto found=s.index.find(key);if(found==s.index.end())return;
  s.kv.erase(s.kv.begin()+found->second);invalidateStoreIndex(s);
}
static void storeRemovePrefix(Store &s, const char *prefix) {
  const size_t length = strlen(prefix);
  s.kv.erase(std::remove_if(s.kv.begin(), s.kv.end(), [&](const KV &entry) {
    return entry.k.compare(0, length, prefix) == 0;
  }), s.kv.end());
  invalidateStoreIndex(s);
}
static bool recoverAtomicFile(const std::string &path);
static void storeLoad(Store &s, const char *path) {
  s.kv.clear();
  invalidateStoreIndex(s);
  if (!recoverAtomicFile(path)) return;
  FILE *f = fopen(path, "r");
  if (!f) return;
  // Persistent library identities include canonical/current paths and a short
  // rename history.  Keep a comfortably bounded line buffer so a valid Switch
  // path is never truncated while loading that registry.
  char line[32768];
  while (fgets(line, sizeof(line), f)) {
    std::string t = trim(line);
    if (t.empty() || t[0] == '#' || t[0] == ';' || t[0] == '[') continue;
    size_t eq = t.find('=');
    if (eq == std::string::npos) continue;
    std::string k = trim(t.substr(0, eq)), v = trim(t.substr(eq + 1));
    if (!k.empty()) s.kv.push_back({ k, v });
  }
  fclose(f);
}

static bool queryRegularFile(const std::string &path, bool &exists) {
  struct stat st{};
  if (stat(path.c_str(), &st) == 0) {
    exists = true;
    return S_ISREG(st.st_mode);
  }
  exists = false;
  return errno == ENOENT;
}

static int allowedCpuCores() {
  u64 mask = 0;
  if (R_FAILED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)))
    return 3;
  int cores = 0;
  for (int core = 0; core < 4; core++)
    if (mask & (UINT64_C(1) << core)) cores++;
  return cores ? cores : 3;
}

static bool regularFileExists(const std::string &path) {
  bool exists = false;
  return queryRegularFile(path, exists) && exists;
}

static bool recoverAtomicFile(const std::string &path) {
  const std::string tmp = path + ".tmp";
  const std::string old = path + ".old";
  bool currentExists = false, oldExists = false, tmpExists = false;
  if (!queryRegularFile(path, currentExists) || !queryRegularFile(old, oldExists) ||
      !queryRegularFile(tmp, tmpExists)) return false;
  if (!currentExists && oldExists) {
    if (rename(old.c_str(), path.c_str()) != 0) return false;
    fsdevCommitDevice("sdmc");
    currentExists = true;
    oldExists = false;
  }
  if (tmpExists && remove(tmp.c_str()) != 0) return false;
  if (currentExists && oldExists && remove(old.c_str()) != 0) return false;
  if (tmpExists || oldExists) fsdevCommitDevice("sdmc");
  return true;
}

static bool replaceAtomic(const std::string &path, const std::string &tmp) {
  const std::string old = path + ".old";
  bool hadCurrent = false, oldExists = false, tmpExists = false;
  if (!queryRegularFile(path, hadCurrent) || !queryRegularFile(old, oldExists) ||
      !queryRegularFile(tmp, tmpExists) || !tmpExists) return false;
  if (oldExists && remove(old.c_str()) != 0) return false;
  if (hadCurrent && rename(path.c_str(), old.c_str()) != 0) return false;
  if (rename(tmp.c_str(), path.c_str()) != 0) {
    if (hadCurrent) {
      rename(old.c_str(), path.c_str());
      fsdevCommitDevice("sdmc");
    }
    return false;
  }
  fsdevCommitDevice("sdmc");
  if (hadCurrent && remove(old.c_str()) == 0) fsdevCommitDevice("sdmc");
  return true;
}

static bool writeAtomicText(const std::string &path, const std::string &text) {
  const std::string tmp = path + ".tmp";
  if (!recoverAtomicFile(path)) return false;

  FILE *file = fopen(tmp.c_str(), "wb");
  if (!file) return false;
  bool ok = fwrite(text.data(), 1, text.size(), file) == text.size();
  if (fflush(file) != 0 || fsync(fileno(file)) != 0) ok = false;
  if (fclose(file) != 0) ok = false;
  if (!ok) { remove(tmp.c_str()); return false; }
  if (!replaceAtomic(path, tmp)) { remove(tmp.c_str()); return false; }
  return true;
}

static bool storeSave(Store &s, const char *path) {
  mkdir(DATA_DIR, 0777);
  std::string text = "# Cemu launcher\n";
  for (auto &e : s.kv) text += e.k + " = " + e.v + "\n";
  return writeAtomicText(path, text);
}

static const char *iniGet(const char *key, const char *def) {
  if (g_active == &g_game) {
    for (auto &e : g_game.kv)   if (e.k == key) return e.v.c_str();
    for (auto &e : g_global.kv) if (e.k == key) return e.v.c_str();
    return def;
  }
  return storeGet(*g_active, key, def);
}
static void iniSet(const char *key, const char *val) { storeSet(*g_active, key, val); }

enum OType { OT_CHOICE, OT_RANGE, OT_ACTION, OT_STATUS };
struct Choice { const char *label, *val; };
struct Opt {
  const char *label;
  const char *key;
  OType type;
  const Choice *ch; int nch;
  int lo, hi, step;
  const char *def;
};
#define O_CHOICE(l,k,c,d)      { l, k, OT_CHOICE, c, (int)(sizeof(c)/sizeof(*c)), 0,0,0, d }
#define O_RANGE(l,k,lo,hi,s,d) { l, k, OT_RANGE,  nullptr,0, lo,hi,s, d }
#define O_ACTION(l)            { l, nullptr, OT_ACTION, nullptr,0, 0,0,0, nullptr }
#define O_STATUS(l)            { l, nullptr, OT_STATUS, nullptr,0, 0,0,0, nullptr }
#define O_STATUS_KEY(l,k)      { l, k, OT_STATUS, nullptr,0, 0,0,0, nullptr }

static const Choice C_cpumode[]  = { {"Multi-core recompiler","3"}, {"Single-core recompiler","1"},
                                     {"Interpreter (slow)","0"} };
// ActiveSettings stores the CPU timer as a shift value.
static const Choice C_timer[]    = { {"1x (normal)","3"}, {"2x faster","2"}, {"4x faster","1"}, {"8x faster","0"},
                                     {"0.5x slower","4"}, {"0.25x slower","5"} };
static const Choice C_bool[]     = { {"Off","false"}, {"On","true"} };
static const Choice C_bool01[]   = { {"Off","0"}, {"On","1"} };
static const Choice C_backend[]  = { {"Deko3D","deko3d"}, {"Vulkan (NVK)","vk"}, {"OpenGL (NVC0)","gl"},
                                     {"OpenGL (Zink/NVK)","zink"} };
static const Choice C_barriers[] = { {"Disabled","false"}, {"Enabled","true"} };
static const Choice C_filter[]   = { {"Bilinear","0"}, {"Bicubic","1"}, {"Hermite","2"}, {"Nearest","3"} };
static const Choice C_scaling[]  = { {"Keep aspect ratio","0"}, {"Stretch","1"} };
static const Choice C_ovpos[]    = { {"Disabled","0"}, {"Top-left","1"}, {"Top-center","2"}, {"Top-right","3"},
                                     {"Bottom-left","4"}, {"Bottom-center","5"}, {"Bottom-right","6"} };
static const Choice C_lang[]     = { {"Auto","-1"}, {"Japanese","0"}, {"English","1"}, {"French","2"}, {"German","3"},
                                     {"Italian","4"}, {"Spanish","5"}, {"Chinese","6"}, {"Korean","7"},
                                     {"Dutch","8"}, {"Portuguese","9"}, {"Russian","10"}, {"Taiwanese","11"} };
static const Choice C_ctype[]    = { {"Wii U GamePad","GamePad"}, {"Pro Controller","Pro"}, {"Classic Controller","Classic"} };
static const Choice C_gamepadLayout[] = { {"TV only","off"}, {"PAD only","pad"}, {"PAD on right","right"}, {"PAD on left","left"},
                                           {"PAD below TV","below"}, {"PAD above TV","above"} };
static const Choice C_lsfgFlowScale[] = { {"Quarter resolution","0.25"}, {"Half resolution","0.5"} };
static const Choice C_players[]  = { {"1","1"}, {"2","2"}, {"3","3"}, {"4","4"},
                                     {"5","5"}, {"6","6"}, {"7","7"}, {"8","8"} };
struct InputMapping { const char *label, *emu, *key, *def; };
static const InputMapping C_inputMappings[] = {
  {"A", "A", "in_A", "A"}, {"B", "B", "in_B", "B"},
  {"X", "X", "in_X", "X"}, {"Y", "Y", "in_Y", "Y"},
  {"L", "L", "in_L", "L"}, {"R", "R", "in_R", "R"},
  {"ZL", "ZL", "in_ZL", "ZL"}, {"ZR", "ZR", "in_ZR", "ZR"},
  {"Plus", "Plus", "in_Plus", "PLUS"}, {"Minus", "Minus", "in_Minus", "MINUS"},
  {"D-Pad Up", "Up", "in_Up", "DUP"}, {"D-Pad Down", "Down", "in_Down", "DDOWN"},
  {"D-Pad Left", "Left", "in_Left", "DLEFT"}, {"D-Pad Right", "Right", "in_Right", "DRIGHT"},
  {"L-Stick click", "StickL", "in_StickL", "LCLICK"},
  {"R-Stick click", "StickR", "in_StickR", "RCLICK"},
};
static const Choice C_launcherTheme[] = { {"Wii U","wiiu"}, {"Classic","classic"} };
static const Choice C_gridColumns[] = { {"3","3"}, {"4","4"}, {"5","5"}, {"6","6"}, {"7","7"}, {"8","8"} };
static const Choice C_gridRows[] = { {"1","1"}, {"2","2"}, {"3","3"} };
static const Choice C_uiLanguage[] = { {"System","system"}, {"English","en"}, {"Français","fr"},
                                       {"Deutsch","de"}, {"Español","es"}, {"Italiano","it"},
                                       {"Português","pt"}, {"简体中文","zh-CN"},
                                       {"繁體中文","zh-TW"} };

enum { SCR_CPU, SCR_GRAPHICS, SCR_FRAMEGEN, SCR_AUDIO, SCR_OVERLAY, SCR_INPUT, SCR_COUNT };

static const Opt S_cpu[] = {
  O_CHOICE("CPU mode",         "cpuMode",       C_cpumode, "3"),
  O_CHOICE("CPU timer speed",  "TimerShiftFactor", C_timer, "3"),
  O_CHOICE("Hardware video decoding",  "H264HardwareDecode", C_bool,    "true"),
  O_STATUS_KEY("CPU cores",    "cpu-cores"),
};
static const Opt S_graphics[] = {
  O_CHOICE("Renderer",              "Wrapper/Renderer", C_backend, "vk"),
  O_CHOICE("VSync",                 "VSync",             C_bool01,  "0"),
  O_CHOICE("Triple buffering",      "TripleBuffer",      C_bool01,  "1"),
  O_CHOICE("Async shader compile",  "AsyncCompile",      C_bool,    "true"),
  O_CHOICE("Accurate barriers (Vulkan)", "vkAccurateBarriers", C_barriers, "true"),
  O_CHOICE("Upscale filter",        "UpscaleFilter",     C_filter,  "1"),
  O_CHOICE("Downscale filter",      "DownscaleFilter",   C_filter,  "0"),
  O_CHOICE("Fullscreen scaling",    "FullscreenScaling", C_scaling, "0"),
  O_CHOICE("GamePad screen",        "GamePadLayout",     C_gamepadLayout, "off"),
};
static const Opt S_audio[] = {
  O_RANGE ("TV volume",        "TVVolume",    0, 100, 5, "50"),
  O_CHOICE("GamePad audio",    "PadAudio",    C_bool,    "true"),
  O_RANGE ("GamePad volume",   "PadVolume",   0, 100, 5, "50"),
  O_RANGE ("Audio latency",    "AudioDelay",  0, 23,  1, "2"),
};
static const Opt S_framegen[] = {
  O_CHOICE("LSFG 2x (Vulkan)", "Wrapper/LSFGEnabled", C_bool, "false"),
  O_CHOICE("Flow resolution", "Wrapper/LSFGFlowScale", C_lsfgFlowScale, "0.25"),
  O_CHOICE("Performance mode", "Wrapper/LSFGPerformance", C_bool, "true"),
  O_STATUS("Lossless.dll"),
};
static const Opt S_overlay[] = {
  O_CHOICE("FPS counter",      "OverlayFPS",         C_bool,  "false"),
  O_CHOICE("Overlay position", "OverlayPosition",    C_ovpos, "1"),
  O_CHOICE("Shader-compile notice","NotifShaderCompile",C_bool,"true"),
};
static const Opt O_console_language = O_CHOICE("Console language", "console_language", C_lang, "-1");
static const Opt S_input[] = {
  O_CHOICE("Controller type",  "in_type",   C_ctype, "GamePad"),
  O_CHOICE("Active players",   "in_players", C_players, "1"),
  O_CHOICE("Rumble",           "in_rumble", C_bool,  "true"),
  O_RANGE ("Stick dead zone",  "in_deadzone", 0, 50, 1, "15"),
  O_ACTION("Control mapping"),
  O_CHOICE("Skylanders Portal",      "UsbSkylanders", C_bool, "false"),
  O_CHOICE("Disney Infinity Base",   "UsbInfinity",   C_bool, "false"),
  O_CHOICE("LEGO Dimensions Toypad", "UsbDimensions", C_bool, "false"),
};
static const Opt S_launcher[] = {
  O_CHOICE("Language",          "Wrapper/Language",       C_uiLanguage,    "system"),
  O_CHOICE("Theme",             "Wrapper/Theme",          C_launcherTheme, "wiiu"),
  O_CHOICE("Games per row",     "Wrapper/GridColumns",    C_gridColumns,   "5"),
  O_CHOICE("Rows per page",     "Wrapper/GridRows",       C_gridRows,      "2"),
  O_CHOICE("Show game titles",  "Wrapper/ShowGameTitles", C_bool,          "true"),
  O_CHOICE("Show region flags", "Wrapper/ShowRegionFlags", C_bool,         "true"),
  O_CHOICE("Show custom settings badges", "Wrapper/ShowCustomSettingsBadges", C_bool, "true"),
  O_CHOICE("UI animations",     "Wrapper/UiAnimations",   C_bool,          "true"),
  O_CHOICE("Sound effects",     "Wrapper/UiSounds",       C_bool,          "true"),
  O_CHOICE("Check updates at boot", "Wrapper/CheckUpdatesAtBoot", C_bool,   "true"),
};
struct Screen { const char *title; const Opt *opts; int n; };
static const Screen g_screens[SCR_COUNT] = {
  { "CPU / Emulation",   S_cpu,      (int)(sizeof(S_cpu)/sizeof(Opt)) },
  { "Graphics",          S_graphics, (int)(sizeof(S_graphics)/sizeof(Opt)) },
  { "Frame Generation",  S_framegen, (int)(sizeof(S_framegen)/sizeof(Opt)) },
  { "Audio",             S_audio,    (int)(sizeof(S_audio)/sizeof(Opt)) },
  { "Overlay",           S_overlay,  (int)(sizeof(S_overlay)/sizeof(Opt)) },
  { "Controller / Input / Accessories", S_input, (int)(sizeof(S_input)/sizeof(Opt)) },
};

struct SettingHelpEntry {
  const char *key;
  const char *kind;
  const char *text;
};

/* Keep the SDL launcher descriptions beside the options they document.  This
 * gives global and per-game settings the same contextual help without adding
 * settings belonging to another emulator. */
static const SettingHelpEntry SETTING_HELP[] = {
  {"cpuMode", "CPU emulation",
   "Selects how Cemu executes the Wii U CPU. The multi-core recompiler is fastest on Switch. Use single-core only for compatibility testing; the interpreter is extremely slow and intended for debugging."},
  {"TimerShiftFactor", "Game timing / compatibility",
   "Changes the speed of the emulated CPU timer without increasing the Switch CPU clock. Some game-specific fixes need a different timer rate, but the normal 1x value is the safest default."},
  {"H264HardwareDecode", "Video playback",
   "Uses the Switch hardware video decoder for Wii U H.264 movies. Disable it only when troubleshooting broken or missing in-game video playback."},

  {"Wrapper/Renderer", "Display backend",
   "Chooses the graphics backend in the unified Cemu core. Vulkan (NVK) is recommended and is required by LSFG. OpenGL uses native NVC0, while Zink runs Cemu's OpenGL renderer on NVK as an additional compatibility path."},

  {"VSync", "Presentation",
   "Synchronizes completed frames to the display refresh to reduce tearing. It can add latency or expose performance drops when a game cannot maintain its target frame rate."},
  {"TripleBuffer", "Presentation / performance",
   "Keeps a third Vulkan swapchain image available while another frame is being displayed. This can make presentation steadier under load, at the cost of some memory and potentially more latency."},
  {"AsyncCompile", "Shader compilation",
   "Compiles Vulkan shaders asynchronously to reduce long gameplay stalls. Newly encountered effects may be missing briefly while their shaders finish compiling."},
  {"vkAccurateBarriers", "Graphics compatibility",
   "Uses more accurate Vulkan synchronization between rendering operations. Keep it enabled for correct effects; disabling it may improve performance slightly but can cause game-specific rendering errors."},
  {"UpscaleFilter", "Image scaling",
   "Selects the filter used when the Wii U image is enlarged to the Switch display. Nearest is sharp and pixelated, while the other filters trade sharpness for smoother scaling."},
  {"DownscaleFilter", "Image scaling",
   "Selects the filter used when an image must be reduced. The choice changes sharpness and aliasing but does not change the game's internal rendering resolution."},
  {"FullscreenScaling", "Display aspect ratio",
   "Keep aspect ratio preserves the game's intended shape and may leave borders. Stretch fills the output but can distort the image."},
  {"GamePadLayout", "Wii U GamePad display",
   "Chooses whether to show the TV view, the GamePad view, or both views in a split layout. Composite layouts reduce the space available to each screen."},

  {"TVVolume", "Audio output",
   "Sets the volume of the emulated Wii U TV audio stream sent to the Switch output."},
  {"PadAudio", "Wii U GamePad audio",
   "Enables the separate audio stream that Wii U software sends to the GamePad. Disable it when duplicated TV and GamePad sound is unwanted."},
  {"PadVolume", "Wii U GamePad audio",
   "Sets the volume of the emulated GamePad audio stream when GamePad audio is enabled."},
  {"AudioDelay", "Audio latency / stability",
   "Controls Cemu's audio buffer target. Lower values reduce latency but can crackle when emulation is uneven; higher values are more stable but respond later."},

  {"Wrapper/LSFGEnabled", "Frame generation",
   "Generates one intermediate display frame for each real frame to target a smoother 2x presentation. It does not increase emulation speed and may add artifacts or latency. Vulkan only."},
  {"Wrapper/LSFGFlowScale", "Frame generation quality",
   "Sets the resolution used for optical-flow analysis. Half resolution can retain more motion detail but costs more GPU time and memory; Quarter is recommended on Switch."},
  {"Wrapper/LSFGPerformance", "Frame generation performance",
   "Uses LSFG's lighter performance-oriented processing path. Disable it only when you prefer image quality and have enough GPU headroom."},

  {"OverlayFPS", "Performance display",
   "Shows Cemu's frame-rate counter during gameplay."},
  {"OverlayPosition", "Performance display",
   "Chooses where Cemu's on-screen performance overlay is drawn. Disabled hides the overlay and its FPS counter."},
  {"NotifShaderCompile", "Shader compilation",
   "Shows a notification while Cemu is compiling shaders so brief stutter or temporarily missing effects can be identified."},

  {"in_type", "Emulated controller",
   "Selects the Wii U controller type presented to the game. Wii U GamePad is required by games that use its screen, microphone, or touch features."},
  {"in_players", "Controller input",
   "Sets how many active emulated controller slots Cemu creates for local multiplayer."},
  {"in_rumble", "Controller feedback",
   "Forwards supported Wii U controller vibration effects to the connected Switch controller."},
  {"in_deadzone", "Analog input",
   "Sets how far an analog stick must move before Cemu accepts input. Raise it to prevent drift; lower it for quicker response."},

  {"UsbSkylanders", "Emulated USB accessory",
   "Enables Cemu's virtual Skylanders Portal for games that communicate with that USB accessory."},
  {"UsbInfinity", "Emulated USB accessory",
   "Enables Cemu's virtual Disney Infinity Base for games that communicate with that USB accessory."},
  {"UsbDimensions", "Emulated USB accessory",
   "Enables Cemu's virtual LEGO Dimensions Toy Pad for games that communicate with that USB accessory."},

  {"console_language", "Wii U system language",
   "Sets the language reported by the emulated Wii U console. Auto follows the Switch system language when Cemu supports it; games may need to be restarted after a change."},

  {"Wrapper/Language", "Launcher language",
   "Selects the language used by the SDL launcher. System follows the Switch console language; technical emulator names and identifiers remain unchanged for accuracy."},

  {"Wrapper/Theme", "Launcher appearance",
   "Changes the SDL launcher's background and visual theme. It does not affect gameplay rendering."},
  {"Wrapper/GridColumns", "Library layout",
   "Sets how many game covers are displayed across each library row. More columns make each cover smaller."},
  {"Wrapper/GridRows", "Library layout",
   "Sets how many rows of game covers are displayed on each library page. More rows make each cover smaller."},
  {"Wrapper/ShowGameTitles", "Library layout",
   "Shows or hides game names below their cover artwork in the launcher library."},
  {"Wrapper/ShowRegionFlags", "Library layout",
   "Shows or hides the region flag in the top-left corner of each game cover."},
  {"Wrapper/ShowCustomSettingsBadges", "Library layout",
   "Shows or hides the square badge on games that have per-game settings. The settings themselves are not changed."},
  {"Wrapper/UiAnimations", "Launcher appearance",
   "Enables launcher transitions, moving highlights, and animated theme effects."},
  {"Wrapper/UiSounds", "Launcher audio",
   "Enables navigation, confirmation, and back sound effects in the SDL launcher."},
  {"Wrapper/CheckUpdatesAtBoot", "Launcher updates",
   "Checks for a newer Cemu-nx release when the SDL launcher starts. The result appears in Launcher settings."},
};

struct SettingHelpInfo {
  const char *kind;
  std::string text;
};

static SettingHelpInfo settingHelpFor(const Opt &option) {
  if(option.key){
    for(const SettingHelpEntry &entry:SETTING_HELP)
      if(!strcmp(entry.key,option.key)) return {entry.kind,entry.text};
  }
  if(option.type==OT_ACTION && option.label && !strcmp(option.label,"Control mapping"))
    return {"Controller mapping",
            "Opens the press-to-bind screen for every Wii U controller input. Select a control, then press the Switch button or trigger that should activate it."};
  if(option.type==OT_STATUS && option.key && !strcmp(option.key,"cpu-cores"))
    return {"Measured value",
            "How many of the console's four CPU cores this process is allowed to use. A HOME Menu shortcut grants all four; launching through the album applet grants three and leaves core 3 idle. Install the shortcut from the applet-mode installer to get the fourth core."};
  if(option.type==OT_STATUS)
    return {"Required component",
            "Shows whether the Lossless Scaling frame-generation library is installed. LSFG cannot be enabled until Lossless.dll is copied to sdmc:/switch/cemu/lsfg/."};
  return {"Missing exact help",
          "This launcher build is missing reviewed help for this setting."};
}

static void detectSystemLanguage() {
  if(R_FAILED(setInitialize())) return;
  u64 code=0;
  SetLanguage language=SetLanguage_ENUS;
  Result result=setGetSystemLanguage(&code);
  if(R_SUCCEEDED(result)) result=setMakeLanguage(code,&language);
  setExit();
  if(R_FAILED(result)) return;

  switch(language){
    case SetLanguage_JA: g_systemCemuLanguage=0; g_systemLanguageName="Japanese"; break;
    case SetLanguage_FR: case SetLanguage_FRCA: g_systemCemuLanguage=2; g_systemLanguageName="French"; break;
    case SetLanguage_DE: g_systemCemuLanguage=3; g_systemLanguageName="German"; break;
    case SetLanguage_IT: g_systemCemuLanguage=4; g_systemLanguageName="Italian"; break;
    case SetLanguage_ES: case SetLanguage_ES419: g_systemCemuLanguage=5; g_systemLanguageName="Spanish"; break;
    case SetLanguage_ZHCN: case SetLanguage_ZHHANS: g_systemCemuLanguage=6; g_systemLanguageName="Chinese"; break;
    case SetLanguage_KO: g_systemCemuLanguage=7; g_systemLanguageName="Korean"; break;
    case SetLanguage_NL: g_systemCemuLanguage=8; g_systemLanguageName="Dutch"; break;
    case SetLanguage_PT: case SetLanguage_PTBR: g_systemCemuLanguage=9; g_systemLanguageName="Portuguese"; break;
    case SetLanguage_RU: g_systemCemuLanguage=10; g_systemLanguageName="Russian"; break;
    case SetLanguage_ZHTW: case SetLanguage_ZHHANT: g_systemCemuLanguage=11; g_systemLanguageName="Taiwanese"; break;
    default: g_systemCemuLanguage=1; g_systemLanguageName="English"; break;
  }
}

static void commitAll() {
  for (int s = 0; s < SCR_COUNT; s++)
    for (int i = 0; i < g_screens[s].n; i++) {
      const Opt &o = g_screens[s].opts[i];
      if (o.type == OT_STATUS || !o.key || !o.def) continue;
      std::string v = iniGet(o.key, o.def);
      iniSet(o.key, v.c_str());
    }
}

static std::vector<CemuKV> buildEffectiveSettings(const std::string &gameKey) {
  Store perGame;
  if (!gameKey.empty()) {
    const std::string path = std::string(GAMECFG_DIR) + "/" + gameKey + ".ini";
    if (regularFileExists(path))
      storeLoad(perGame, path.c_str());
  }
  std::vector<CemuKV> out;
  for (int s = 0; s < SCR_COUNT; s++)
    for (int i = 0; i < g_screens[s].n; i++) {
      const Opt &o = g_screens[s].opts[i];
      if (!o.key) continue;
      const char *v = nullptr;
      for (auto &e : perGame.kv)  if (e.k == o.key) { v = e.v.c_str(); break; }
      if (!v) for (auto &e : g_global.kv) if (e.k == o.key) { v = e.v.c_str(); break; }
      out.push_back({ o.key, v ? v : (o.def ? o.def : "") });
    }
  for (const auto &mapping : C_inputMappings) {
    const char *value = nullptr;
    for (auto &entry : perGame.kv) if (entry.k == mapping.key) { value = entry.v.c_str(); break; }
    if (!value) for (auto &entry : g_global.kv) if (entry.k == mapping.key) { value = entry.v.c_str(); break; }
    out.push_back({mapping.key, value ? value : mapping.def});
  }
  { const char *v = nullptr;
    for (auto &e : g_global.kv) if (e.k == "console_language") { v = e.v.c_str(); break; }
    std::string language=v?v:"-1";
    if(language=="-1") language=std::to_string(g_systemCemuLanguage);
    out.push_back({ "console_language", std::move(language) }); }
  return out;
}

static bool writeInputIni(const std::vector<CemuKV> &eff) {
  std::string text = "[controller]\n";
  text += "type = " + std::string(cemuKVGet(eff, "in_type", "GamePad")) + "\n";
  text += "players = " + std::string(cemuKVGet(eff, "in_players", "1")) + "\n";
  text += "rumble = ";
  text += (strcmp(cemuKVGet(eff, "in_rumble", "true"), "true") == 0 ? "on\n" : "off\n");
  text += "deadzone = " + std::string(cemuKVGet(eff, "in_deadzone", "15")) + "\n";
  text += "[map]\n";
  for (const auto &mapping : C_inputMappings)
    text += std::string(mapping.emu) + " = " + cemuKVGet(eff, mapping.key, mapping.def) + "\n";
  return writeAtomicText("sdmc:/switch/cemu/input.ini", text);
}

static bool appendHandoffValue(std::string &text, const char *key, const std::string &value) {
  if (value.find_first_of("\r\n") != std::string::npos)
    return false;
  text += key;
  text += '=';
  text += value;
  text += '\n';
  return true;
}

static const char *ENABLED_PACKS_FILE = "sdmc:/switch/cemu/enabled_packs.txt";
static bool readEnabledPacks(std::vector<CemuGraphicPack> &out) {
  out.clear();
  if (!recoverAtomicFile(ENABLED_PACKS_FILE)) return false;
  FILE *f = fopen(ENABLED_PACKS_FILE, "r");
  if (!f) return errno == ENOENT;
  char line[4096];
  bool ok = true;
  while (fgets(line, sizeof(line), f)) {
    if (!strchr(line, '\n') && !feof(f)) { ok = false; break; }
    std::string s = line;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    if (s.empty()) continue;
    CemuGraphicPack pk;
    size_t start = 0; bool first = true;
    for (;;) {
      size_t tab = s.find('\t', start);
      std::string field = s.substr(start, tab == std::string::npos ? std::string::npos : tab - start);
      if (first) {
        pk.rulesRel = (field.rfind("sdmc:", 0) == 0) ? field : (std::string(DATA_DIR) + "/" + field);
        first = false; }
      else if (!field.empty()) { size_t eq = field.find('='); if (eq != std::string::npos) pk.presets.push_back({field.substr(0, eq), field.substr(eq + 1)}); }
      if (tab == std::string::npos) break;
      start = tab + 1;
    }
    if (!pk.rulesRel.empty()) out.push_back(std::move(pk));
  }
  if (ferror(f) || fclose(f) != 0) ok = false;
  if (!ok) out.clear();
  return ok;
}

static SDL_Window   *g_win = nullptr;
static SDL_Renderer *g_ren = nullptr;
static TTF_Font     *g_font = nullptr, *g_font_sm = nullptr, *g_font_big = nullptr,
                    *g_font_caption = nullptr, *g_font_hint = nullptr;
static PlSharedFontType g_requestedFontType = PlSharedFontType_Total;
static SDL_Texture  *g_logo = nullptr;
static int SW = 1280, SH = 720;
static int g_outputW = 1280, g_outputH = 720;
static float g_uiScale = 1.0f;    // output pixels per logical unit
static float g_fontScale = 1.0f;  // scale the currently loaded fonts were opened at
static bool g_presentVsync = false;
static SDL_Texture *g_roundTexture = nullptr;

static void configureLauncherScale(){
  SW=1280; SH=720;
  if(g_outputW<1) g_outputW=1280;
  if(g_outputH<1) g_outputH=720;
  g_uiScale=std::min(g_outputW/1280.0f,g_outputH/720.0f);
  if(!g_ren) return;
  SDL_RenderSetViewport(g_ren,nullptr);
  SDL_RenderSetScale(g_ren,g_uiScale,g_uiScale);
}
static bool g_romfsReady = false;
static bool g_sdlReady = false;
static bool g_ttfReady = false;
static bool g_imgReady = false;
static bool g_plReady = false;
static bool g_griddbReady = false;
static bool g_storageSocketReady = false;
static std::string g_updateNoticeTag;
static std::string g_updateNotifiedTag;
static Uint32 g_updateNoticeUntil = 0;

enum class LauncherTheme { WiiU, Classic };
static LauncherTheme g_launcherTheme = LauncherTheme::WiiU;
static bool g_uiAnimations = true;
static bool g_showGameTitles = true;
static bool g_showRegionFlags = true;
static bool g_showCustomSettingsBadges = true;
static int g_gridColumns = 5;
static int g_gridRows = 2;
static SDL_Texture *g_glowTexture = nullptr;

static SDL_Color COL_BG    = { 8, 12, 24, 255 };
static SDL_Color COL_TXT   = { 235, 239, 247, 255 };
static SDL_Color COL_DIM   = { 151, 163, 184, 255 };
static SDL_Color COL_HI    = { 100, 211, 255, 255 };
static SDL_Color COL_VAL   = { 255, 215, 120, 255 };
static SDL_Color COL_SEL   = { 116, 200, 255, 255 };
static SDL_Color COL_PANEL = { 16, 23, 39, 184 };
static SDL_Color COL_CARD  = { 22, 30, 49, 214 };
static SDL_Color COL_FOCUS = { 28, 69, 92, 210 };

static SDL_FRect pixelAlignedRect(int x,int y,int w,int h){
  float sx=1,sy=1; if(g_ren) SDL_RenderGetScale(g_ren,&sx,&sy);
  if(sx<=0) sx=1;
  if(sy<=0) sy=1;
  const float left=std::round(x*sx),top=std::round(y*sy);
  return {left/sx,top/sy,(std::round((x+w)*sx)-left)/sx,(std::round((y+h)*sy)-top)/sy};
}
static void fillRect(int x,int y,int w,int h, SDL_Color c){
  if(w<=0||h<=0) return;
  const SDL_FRect r=pixelAlignedRect(x,y,w,h);
  SDL_SetRenderDrawColor(g_ren,c.r,c.g,c.b,c.a); SDL_RenderFillRectF(g_ren,&r);
}
static void border(int x,int y,int w,int h,int t, SDL_Color c){ SDL_SetRenderDrawColor(g_ren,c.r,c.g,c.b,c.a); for(int i=0;i<t;i++){ SDL_Rect r={x-i,y-i,w+2*i,h+2*i}; SDL_RenderDrawRect(g_ren,&r); } }

// Layout uses 720p coordinates; text textures retain the output resolution.
static int fontMetric(int pixels){ return (int)std::ceil(pixels/(g_fontScale>0.f?g_fontScale:1.f)); }
static int fontHeight(TTF_Font *font){ return font?fontMetric(TTF_FontHeight(font)):0; }

static void roundedRect(int x,int y,int width,int height,int radius,SDL_Color color){
  if(width<=0||height<=0)return;
  const int r=std::min({radius,width/2,height/2});
  if(!g_roundTexture&&g_ren){
    SDL_Surface *surface=SDL_CreateRGBSurfaceWithFormat(0,32,32,32,SDL_PIXELFORMAT_RGBA32);
    if(surface){
      for(int py=0;py<32;py++){
        auto *row=(Uint32*)((Uint8*)surface->pixels+py*surface->pitch);
        for(int px=0;px<32;px++){
          const float dx=px-15.5f,dy=py-15.5f;
          const Uint8 alpha=(Uint8)(255*std::clamp(16.f-std::sqrt(dx*dx+dy*dy),0.f,1.f));
          row[px]=SDL_MapRGBA(surface->format,255,255,255,alpha);
        }
      }
      g_roundTexture=SDL_CreateTextureFromSurface(g_ren,surface);SDL_FreeSurface(surface);
      if(g_roundTexture)SDL_SetTextureBlendMode(g_roundTexture,SDL_BLENDMODE_BLEND);
    }
  }
  if(!g_roundTexture||r<1){fillRect(x,y,width,height,color);return;}
  fillRect(x+r,y,width-r*2,height,color);
  fillRect(x,y+r,r,height-r*2,color);fillRect(x+width-r,y+r,r,height-r*2,color);
  SDL_SetTextureColorMod(g_roundTexture,color.r,color.g,color.b);SDL_SetTextureAlphaMod(g_roundTexture,color.a);
  for(int corner=0;corner<4;corner++){
    const SDL_Rect source={(corner&1)*16,(corner>>1)*16,16,16};
    const SDL_FRect destination=pixelAlignedRect(x+((corner&1)?width-r:0),y+((corner>>1)?height-r:0),r,r);
    SDL_RenderCopyF(g_ren,g_roundTexture,&source,&destination);
  }
}
static void roundedPanel(int x,int y,int width,int height,SDL_Color face,SDL_Color edge,int radius=8,int thickness=1){
  roundedRect(x,y,width,height,radius,edge);
  roundedRect(x+thickness,y+thickness,width-2*thickness,height-2*thickness,std::max(0,radius-thickness),face);
}
static void drawButtonPanel(int x,int y,int width,int height,bool selected){
  roundedPanel(x,y,width,height,selected?COL_FOCUS:COL_CARD,selected?COL_SEL:SDL_Color{255,255,255,28},6);
}
// Fully rounded pill; clamps its own fraction so callers pass a raw ratio.
static void drawProgressBar(int x,int y,int width,int height,double fraction){
  roundedRect(x,y,width,height,height/2,COL_CARD);
  const int filled=(int)(width*std::clamp(fraction,0.0,1.0));
  if(filled>0)roundedRect(x,y,filled,height,height/2,COL_SEL);
}
static void drawRowHighlight(int x,int y,int width,int height){
  roundedRect(x,y,width,height,4,COL_FOCUS);
  fillRect(x,y+height/5,3,height*3/5,COL_SEL);
}

static int g_footN=0;

// Per-position marquee: restarts when the text changes and rests 0.9s at each end.
struct TextScroll { std::string text; Uint32 since=0; };
static std::map<std::pair<int,int>,TextScroll> g_textScroll;
static bool g_scrollTextActive=false;
static int textScrollOffset(int x,int y,int span,const char *text){
  g_scrollTextActive=true;
  if(g_textScroll.size()>64)g_textScroll.clear();
  auto &scroll=g_textScroll[{x,y}];
  const Uint32 now=SDL_GetTicks();
  if(scroll.text!=text){scroll.text=text;scroll.since=now;}
  const float travel=span/45.0f,pause=0.9f;
  const float phase=std::fmod((now-scroll.since)/1000.0f,2*(travel+pause));
  if(phase<pause)return 0;
  if(phase<pause+travel)return (int)((phase-pause)*45);
  if(phase<2*pause+travel)return span;
  return std::max(0,span-(int)((phase-2*pause-travel)*45));
}

struct TextKey {
  TTF_Font *font;
  Uint32 color;
  std::string text;
  bool operator==(const TextKey &other) const {
    return font == other.font && color == other.color && text == other.text;
  }
};

struct TextKeyHash {
  size_t operator()(const TextKey &key) const {
    size_t hash = std::hash<std::string>{}(key.text);
    hash ^= std::hash<TTF_Font *>{}(key.font) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    hash ^= std::hash<Uint32>{}(key.color) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    return hash;
  }
};

struct TextEntry {
  SDL_Texture *texture;
  float width;
  float height;
  size_t bytes;
  Uint64 use;
};

struct MetricKey {
  TTF_Font *font;
  std::string text;
  bool operator==(const MetricKey &other) const { return font == other.font && text == other.text; }
};

struct MetricKeyHash {
  size_t operator()(const MetricKey &key) const {
    size_t hash = std::hash<std::string>{}(key.text);
    return hash ^ (std::hash<TTF_Font *>{}(key.font) + 0x9e3779b9 + (hash << 6) + (hash >> 2));
  }
};

struct MetricEntry { int width; Uint64 use; };

struct EllipsisKey {
  TTF_Font *font;
  int maxWidth;
  std::string text;
  bool operator==(const EllipsisKey &other) const {
    return font == other.font && maxWidth == other.maxWidth && text == other.text;
  }
};

struct EllipsisKeyHash {
  size_t operator()(const EllipsisKey &key) const {
    size_t hash = std::hash<std::string>{}(key.text);
    hash ^= std::hash<TTF_Font *>{}(key.font) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    hash ^= std::hash<int>{}(key.maxWidth) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    return hash;
  }
};

struct EllipsisEntry { std::string text; Uint64 use; };

static std::unordered_map<TextKey, TextEntry, TextKeyHash> g_textCache;
static std::unordered_map<MetricKey, MetricEntry, MetricKeyHash> g_metricCache;
static std::unordered_map<EllipsisKey, EllipsisEntry, EllipsisKeyHash> g_ellipsisCache;
static size_t g_textCacheBytes = 0;
static Uint64 g_textUseSerial = 0;
static constexpr size_t TEXT_CACHE_LIMIT = 512;
static constexpr size_t TEXT_CACHE_BYTES = 12 * 1024 * 1024;
static constexpr size_t METRIC_CACHE_LIMIT = 2048;
static constexpr size_t ELLIPSIS_CACHE_LIMIT = 512;

static Uint32 packColor(SDL_Color color) {
  return (Uint32)color.r | ((Uint32)color.g << 8) | ((Uint32)color.b << 16) | ((Uint32)color.a << 24);
}

static void rememberTextMetric(TTF_Font *font, const std::string &text, int width) {
  MetricKey key{font, text};
  auto found = g_metricCache.find(key);
  if (found != g_metricCache.end()) {
    found->second.width = width;
    found->second.use = ++g_textUseSerial;
    return;
  }
  if (g_metricCache.size() >= METRIC_CACHE_LIMIT) {
    auto victim = g_metricCache.begin();
    for (auto it = std::next(g_metricCache.begin()); it != g_metricCache.end(); ++it)
      if (it->second.use < victim->second.use) victim = it;
    g_metricCache.erase(victim);
  }
  g_metricCache.emplace(std::move(key), MetricEntry{width, ++g_textUseSerial});
}

static void evictTextEntries(size_t incomingBytes) {
  while (!g_textCache.empty() &&
         (g_textCache.size() >= TEXT_CACHE_LIMIT || g_textCacheBytes > TEXT_CACHE_BYTES - incomingBytes)) {
    auto victim = g_textCache.begin();
    for (auto it = std::next(g_textCache.begin()); it != g_textCache.end(); ++it)
      if (it->second.use < victim->second.use) victim = it;
    SDL_DestroyTexture(victim->second.texture);
    g_textCacheBytes -= victim->second.bytes;
    g_textCache.erase(victim);
  }
}

static void clearTextCaches() {
  for (auto &entry : g_textCache) SDL_DestroyTexture(entry.second.texture);
  g_textCache.clear();
  g_metricCache.clear();
  g_ellipsisCache.clear();
  g_textCacheBytes = 0;
  g_textUseSerial = 0;
}

static bool normalizeLauncherTheme(Store &values) {
  const std::string theme=storeGet(values,"Wrapper/Theme","wiiu");
  if(theme=="wiiu"||theme=="classic") return false;
  storeSet(values,"Wrapper/Theme","wiiu");
  return true;
}

static void applyLauncherAppearance() {
  LauncherTheme previous = g_launcherTheme;
  const char *theme = storeGet(g_global, "Wrapper/Theme", "wiiu");
  g_launcherTheme = !strcmp(theme,"classic") ? LauncherTheme::Classic : LauncherTheme::WiiU;
  normalizeLauncherTheme(g_global);
  g_uiAnimations = strcmp(storeGet(g_global, "Wrapper/UiAnimations", "true"), "false") != 0;
  g_showGameTitles = strcmp(storeGet(g_global, "Wrapper/ShowGameTitles", "true"), "false") != 0;
  g_showRegionFlags = strcmp(storeGet(g_global, "Wrapper/ShowRegionFlags", "true"), "false") != 0;
  g_showCustomSettingsBadges =
      strcmp(storeGet(g_global, "Wrapper/ShowCustomSettingsBadges", "true"), "false") != 0;
  g_gridColumns = std::max(3, std::min(8, atoi(storeGet(g_global, "Wrapper/GridColumns", "5"))));
  g_gridRows = std::max(1, std::min(3, atoi(storeGet(g_global, "Wrapper/GridRows", "2"))));

  COL_BG={0,8,16,255}; COL_TXT={235,248,255,255}; COL_DIM={143,192,216,255};
  COL_HI={118,222,255,255}; COL_VAL={194,239,255,255}; COL_SEL={61,183,235,255};
  COL_PANEL={4,31,50,255}; COL_CARD={5,35,56,218}; COL_FOCUS={12,76,108,255};
  if (previous != g_launcherTheme && g_ren)
    clearTextCaches();
}

static void ensureGlowTexture() {
  if (g_glowTexture || !g_ren) return;
  constexpr int size=256;
  SDL_Surface *surface=SDL_CreateRGBSurfaceWithFormat(0,size,size,32,SDL_PIXELFORMAT_RGBA32);
  if(!surface) return;
  if(SDL_LockSurface(surface)==0){
    for(int y=0;y<size;y++){
      auto *row=(Uint32*)((Uint8*)surface->pixels+y*surface->pitch);
      for(int x=0;x<size;x++){
        float dx=(x-(size-1)*0.5f)/(size*0.5f),dy=(y-(size-1)*0.5f)/(size*0.5f);
        float distance=sqrtf(dx*dx+dy*dy);
        float strength=distance>=1.f?0.f:1.f-distance;
        Uint8 alpha=(Uint8)(255.f*strength*strength);
        row[x]=SDL_MapRGBA(surface->format,255,255,255,alpha);
      }
    }
    SDL_UnlockSurface(surface);
    g_glowTexture=SDL_CreateTextureFromSurface(g_ren,surface);
    if(g_glowTexture) SDL_SetTextureBlendMode(g_glowTexture,SDL_BLENDMODE_BLEND);
  }
  SDL_FreeSurface(surface);
}

static bool hasAnimatedBackground() { return g_launcherTheme==LauncherTheme::WiiU; }

static void drawBackgroundParticles(float time,SDL_Color color,int count,float speed) {
  for(int i=0;i<count;i++){
    float travel=fmodf(i*0.371f+time*speed*(0.65f+(i%5)*0.11f),1.12f)-0.06f;
    float y=fmodf(i*0.217f+0.11f*sinf(time*0.29f+i*1.73f),1.f);
    float pulse=0.45f+0.55f*sinf(time*(0.9f+(i%4)*0.17f)+i);
    Uint8 alpha=(Uint8)(color.a*(0.55f+0.45f*pulse));
    int size=(i%9==0)?3:2;
    fillRect((int)(travel*SW),(int)(y*SH),size,size,(SDL_Color){color.r,color.g,color.b,alpha});
  }
}

static Uint8 blendChannel(Uint8 first,Uint8 second,float amount) {
  return (Uint8)(first+(second-first)*std::clamp(amount,0.f,1.f));
}

static void drawBubble(int centerX,int centerY,int radius,Uint8 alpha) {
  if(radius<3||alpha==0) return;
  if(g_glowTexture){
    SDL_SetTextureColorMod(g_glowTexture,90,205,255);
    SDL_SetTextureAlphaMod(g_glowTexture,(Uint8)(alpha/5));
    SDL_Rect glow={centerX-radius*2,centerY-radius*2,radius*4,radius*4};
    SDL_RenderCopy(g_ren,g_glowTexture,nullptr,&glow);
  }
  const int segments=24;
  SDL_SetRenderDrawColor(g_ren,124,220,255,alpha);
  std::array<SDL_Point,segments+1> outer{},inner{};
  for(int segment=0;segment<=segments;segment++){
    float angle=segment*6.2831853f/segments;
    float x=cosf(angle),y=sinf(angle);
    outer[segment]={centerX+(int)(x*radius),centerY+(int)(y*radius)};
    inner[segment]={centerX+(int)(x*(radius-1)),centerY+(int)(y*(radius-1))};
  }
  SDL_RenderDrawLines(g_ren,outer.data(),(int)outer.size());
  SDL_RenderDrawLines(g_ren,inner.data(),(int)inner.size());
  SDL_SetRenderDrawColor(g_ren,235,252,255,(Uint8)std::min(255,(int)alpha+55));
  std::array<SDL_Point,6> highlight{};
  for(int segment=0;segment<(int)highlight.size();segment++){
    float angle=3.55f+segment*0.13f;
    highlight[segment]={centerX+(int)(cosf(angle)*radius),centerY+(int)(sinf(angle)*radius)};
  }
  SDL_RenderDrawLines(g_ren,highlight.data(),(int)highlight.size());
}

static void drawBubblesBackground(float time) {
  const SDL_Color top={20,126,169,255},middle={4,54,82,255},bottom={0,5,11,255};
  constexpr int bands=56;
  for(int band=0;band<bands;band++){
    float y=(band+0.5f)/bands;
    SDL_Color color{};
    if(y<0.58f){
      float amount=y/0.58f;
      color={blendChannel(top.r,middle.r,amount),blendChannel(top.g,middle.g,amount),blendChannel(top.b,middle.b,amount),255};
    } else {
      float amount=(y-0.58f)/0.42f;
      color={blendChannel(middle.r,bottom.r,amount),blendChannel(middle.g,bottom.g,amount),blendChannel(middle.b,bottom.b,amount),255};
    }
    int y0=band*SH/bands,y1=(band+1)*SH/bands;
    fillRect(0,y0,SW,y1-y0,color);
  }

  if(g_glowTexture){
    SDL_SetTextureColorMod(g_glowTexture,118,225,255);
    SDL_SetTextureAlphaMod(g_glowTexture,105);
    SDL_Rect surface={-SW/6,-SH/3,SW*4/3,SH*2/3};
    SDL_RenderCopy(g_ren,g_glowTexture,nullptr,&surface);
    for(int ray=0;ray<7;ray++){
      float sway=sinf(time*(0.10f+ray*0.013f)+ray*1.31f);
      int width=SW*(11+(ray%3)*3)/100;
      int x=SW*(8+ray*14)/100+(int)(sway*SW*0.025f)-width/2;
      SDL_Rect shaft={x,-SH/3,width,SH*4/3};
      SDL_SetTextureAlphaMod(g_glowTexture,(Uint8)(23+(ray%3)*7));
      SDL_RenderCopyEx(g_ren,g_glowTexture,nullptr,&shaft,-9.0+ray*2.7+sway*2.0,nullptr,SDL_FLIP_NONE);
    }
  }

  for(int index=0;index<18;index++){
    float progress=fmodf(index*0.173f+time*(0.038f+(index%5)*0.007f),1.18f);
    float y=1.08f-progress;
    float x=0.05f+fmodf(index*0.283f,0.90f)+0.032f*sinf(time*(0.31f+(index%4)*0.04f)+index);
    float fade=std::min(std::clamp((1.10f-y)*5.f,0.f,1.f),std::clamp((y+0.12f)*6.f,0.f,1.f));
    int radius=(int)(SH*(0.009f+(index%6)*0.0042f));
    if(index%11==0) radius=radius*3/2;
    drawBubble((int)(x*SW),(int)(y*SH),radius,(Uint8)(fade*(85+(index%4)*24)));
  }
  drawBackgroundParticles(time,(SDL_Color){164,228,255,62},24,0.008f);
}

static void drawClassicBackground();

static void drawGradient(const SDL_Rect &rect,SDL_Color first,SDL_Color last,bool horizontal=false){
  const float x=(float)rect.x,y=(float)rect.y,w=(float)rect.w,h=(float)rect.h;
  const SDL_Vertex vertices[]={{{x,y},first,{0,0}},{{x+w,y},horizontal?last:first,{0,0}},
    {{x,y+h},horizontal?first:last,{0,0}},{{x+w,y+h},last,{0,0}}};
  const int indices[]={0,1,2,2,1,3};
  SDL_RenderGeometry(g_ren,nullptr,vertices,4,indices,6);
}

static void clearUiBackground() {
  g_footN=0;
  g_scrollTextActive=false;
  SDL_RenderSetClipRect(g_ren,nullptr);
  SDL_SetRenderDrawColor(g_ren,COL_BG.r,COL_BG.g,COL_BG.b,255);
  SDL_RenderClear(g_ren);
  if(g_launcherTheme==LauncherTheme::Classic){ drawClassicBackground(); return; }
  ensureGlowTexture();
  drawBubblesBackground(g_uiAnimations?SDL_GetTicks()/1000.f:0.f);
  if(g_glowTexture){ SDL_SetTextureColorMod(g_glowTexture,255,255,255); SDL_SetTextureAlphaMod(g_glowTexture,255); }
}

static void glassPanel(int x,int y,int width,int height) {
  roundedPanel(x,y,width,height,COL_PANEL,(SDL_Color){255,255,255,24});
}

static void drawText(TTF_Font*f,int x,int y,const char*s,SDL_Color c){
  if(!f||!s||!*s) return;
  TextKey key{f,packColor(c),s};
  auto found=g_textCache.find(key);
  if(found!=g_textCache.end()){
    found->second.use=++g_textUseSerial;
    SDL_FRect d={(float)x,(float)y,found->second.width,found->second.height};
    SDL_RenderCopyF(g_ren,found->second.texture,nullptr,&d);
    return;
  }
  SDL_Surface*sf=TTF_RenderUTF8_Blended(f,s,c); if(!sf) return;
  SDL_Texture*t=SDL_CreateTextureFromSurface(g_ren,sf);
  const size_t bytes=(size_t)sf->w*(size_t)sf->h*4;
  const float w=sf->w/(g_fontScale>0.f?g_fontScale:1.f),h=sf->h/(g_fontScale>0.f?g_fontScale:1.f);
  SDL_FreeSurface(sf);
  if(!t) return;
  rememberTextMetric(f,s,(int)std::ceil(w));
  if(bytes<=TEXT_CACHE_BYTES){
    evictTextEntries(bytes);
    TextEntry entry{t,w,h,bytes,++g_textUseSerial};
    auto inserted=g_textCache.emplace(std::move(key),entry);
    g_textCacheBytes+=bytes;
    SDL_FRect d={(float)x,(float)y,w,h}; SDL_RenderCopyF(g_ren,inserted.first->second.texture,nullptr,&d);
  } else {
    SDL_FRect d={(float)x,(float)y,w,h}; SDL_RenderCopyF(g_ren,t,nullptr,&d); SDL_DestroyTexture(t);
  }
}
static int textW(TTF_Font*f,const char*s){
  if(!f||!s||!*s) return 0;
  MetricKey key{f,s}; auto found=g_metricCache.find(key);
  if(found!=g_metricCache.end()){ found->second.use=++g_textUseSerial; return found->second.width; }
  int w=0,h=0; if(TTF_SizeUTF8(f,s,&w,&h)!=0) return 0;
  w=fontMetric(w); rememberTextMetric(f,s,w); return w;
}

static const std::string &ellipsizedText(TTF_Font *font, const std::string &text, int maxWidth) {
  EllipsisKey key{font,maxWidth,text};
  auto found=g_ellipsisCache.find(key);
  if(found!=g_ellipsisCache.end()){ found->second.use=++g_textUseSerial; return found->second.text; }

  std::vector<size_t> boundaries{0};
  for(size_t i=0;i<text.size();){
    const unsigned char lead=(unsigned char)text[i];
    size_t length=lead<0x80?1:(lead&0xe0)==0xc0?2:(lead&0xf0)==0xe0?3:(lead&0xf8)==0xf0?4:1;
    if(i+length>text.size()) length=1;
    for(size_t j=1;j<length;j++) if(((unsigned char)text[i+j]&0xc0)!=0x80){ length=1; break; }
    i+=length; boundaries.push_back(i);
  }
  size_t low=0,high=boundaries.size()-1;
  while(low<high){
    size_t middle=(low+high+1)/2;
    std::string candidate=text.substr(0,boundaries[middle])+"...";
    if(textW(font,candidate.c_str())<=maxWidth) low=middle; else high=middle-1;
  }
  std::string shortened=text.substr(0,boundaries[low])+"...";
  if(g_ellipsisCache.size()>=ELLIPSIS_CACHE_LIMIT){
    auto victim=g_ellipsisCache.begin();
    for(auto it=std::next(g_ellipsisCache.begin());it!=g_ellipsisCache.end();++it)
      if(it->second.use<victim->second.use) victim=it;
    g_ellipsisCache.erase(victim);
  }
  auto inserted=g_ellipsisCache.emplace(std::move(key),EllipsisEntry{std::move(shortened),++g_textUseSerial});
  return inserted.first->second.text;
}
// Like ellipsizedText, but returns the text unchanged when it already fits.
static std::string fittedText(TTF_Font *font,const std::string &text,int maxWidth){
  if(maxWidth<=0) return {};
  if(textW(font,text.c_str())<=maxWidth) return text;
  return ellipsizedText(font,text,maxWidth);
}
static void drawTextR(TTF_Font*f,int xr,int y,const char*s,SDL_Color c){ drawText(f,xr-textW(f,s),y,s,c); }
static void drawTextC(TTF_Font*f,int cx,int y,const char*s,SDL_Color c){ drawText(f,cx-textW(f,s)/2,y,s,c); }

// Translation is deliberately opt-in at semantic UI call sites.  Game names,
// paths, network errors and text entered by the user continue to use drawText
// directly, so a dynamic value can never be mistaken for a catalog key.
template<size_t N>
static void drawStaticText(TTF_Font*f,int x,int y,const char (&s)[N],SDL_Color c){
  drawText(f,x,y,LauncherLocalization::Translate(s).data(),c);
}
template<size_t N>
static void drawStaticTextC(TTF_Font*f,int cx,int y,const char (&s)[N],SDL_Color c){
  drawTextC(f,cx,y,LauncherLocalization::Translate(s).data(),c);
}

static void drawTitleCell(int cx,int cellW,int y,const std::string&title,bool sel,SDL_Color col);
static void downloadAllCovers();
static void toast(const char *msg);
static void modalMessage(const char *title, const std::vector<std::string> &lines);
static void gfxPackScreen(uint64_t filterTitleId);
static bool confirmBox(const char *title, const std::vector<std::string> &lines);
static void runUpdateScreen();
static std::string installedReleaseTag();
static void pollUpdateNotification();
static void drawUpdateNotification();
static int dropdown(const char *title, const char *const *labels, int n, int cur);
static void ensureDefaultGameSource();
static void beginScreenFx();
static void drawFadeIn();
static int topBarH();
static int settingsRowH();
static int settingsListY();
static int settingsFooterReserve();
static void drawHeader(const char *title,const char *ctx);
static void drawScrollTextR(TTF_Font *font,int xRight,int y,int maxWidth,const char *text,SDL_Color color);
static void drawScrollTextL(TTF_Font *font,int x,int y,int maxWidth,const char *text,SDL_Color color);
static void drawWrapped(TTF_Font *font,int x,int y,int maxWidth,int lineHeight,int maxLines,const char *text,SDL_Color color);
static SDL_Texture *loadScaledTexture(const std::string &path,int width,int height,bool preserveAspect=false);
static bool g_rescanAfterSettings = false;

struct StaticDialogText {
  std::string value;
  template<size_t N> StaticDialogText(const char (&literal)[N])
      : value(LauncherLocalization::Translate(literal)) {}
  StaticDialogText(const std::string &dynamic) : value(dynamic) {}
  StaticDialogText(std::string &&dynamic) : value(std::move(dynamic)) {}
};

static std::vector<std::string> materializeStaticDialog(
    std::initializer_list<StaticDialogText> lines) {
  std::vector<std::string> result;
  result.reserve(lines.size());
  for(const StaticDialogText &line:lines) result.push_back(line.value);
  return result;
}

template<size_t N>
static void toastStatic(const char (&message)[N]) {
  const std::string localized(LauncherLocalization::Translate(message));
  toast(localized.c_str());
}

template<size_t N>
static void modalMessageStatic(const char (&title)[N],
                               std::initializer_list<StaticDialogText> lines) {
  const std::string localizedTitle(LauncherLocalization::Translate(title));
  modalMessage(localizedTitle.c_str(),materializeStaticDialog(lines));
}

template<size_t N>
static bool confirmBoxStatic(const char (&title)[N],
                             std::initializer_list<StaticDialogText> lines) {
  const std::string localizedTitle(LauncherLocalization::Translate(title));
  return confirmBox(localizedTitle.c_str(),materializeStaticDialog(lines));
}

template<size_t N>
static void drawHeaderStatic(const char (&title)[N],const char *context) {
  drawHeader(LauncherLocalization::Translate(title).data(),context);
}

template<size_t N>
static int dropdownStaticTitle(const char (&title)[N],const char *const *labels,int count,int current) {
  return dropdown(LauncherLocalization::Translate(title).data(),labels,count,current);
}

static SDL_Texture *g_flag[4] = { nullptr, nullptr, nullptr, nullptr };
static void fillCircle(int cx,int cy,int r,SDL_Color c){
  SDL_SetRenderDrawColor(g_ren,c.r,c.g,c.b,c.a);
  for(int dy=-r;dy<=r;dy++){ int dx=(int)(sqrt((double)(r*r-dy*dy))+0.5); SDL_RenderDrawLine(g_ren,cx-dx,cy+dy,cx+dx,cy+dy); }
}
static SDL_Texture *makeFlagTex(int region,int W,int H){
  SDL_Texture *t=SDL_CreateTexture(g_ren,SDL_PIXELFORMAT_RGBA8888,SDL_TEXTUREACCESS_TARGET,W,H);
  if(!t) return nullptr;
  SDL_SetTextureBlendMode(t,SDL_BLENDMODE_BLEND);
  SDL_Texture *previous=SDL_GetRenderTarget(g_ren);
  float scaleX=1.0f,scaleY=1.0f;SDL_RenderGetScale(g_ren,&scaleX,&scaleY);
  SDL_SetRenderTarget(g_ren,t);
  SDL_RenderSetScale(g_ren,1.0f,1.0f);
  SDL_SetRenderDrawColor(g_ren,0,0,0,0); SDL_RenderClear(g_ren);
  if(region==3){
    fillRect(0,0,W,H,(SDL_Color){245,245,245,255});
    fillCircle(W/2,H/2,H*30/100,(SDL_Color){188,0,45,255});
  } else if(region==1){
    for(int i=0;i<7;i++) fillRect(0,i*H/7,W,H/7+1,(i%2)?(SDL_Color){235,235,235,255}:(SDL_Color){178,34,52,255});
    fillRect(0,0,W*2/5,(H*4)/7,(SDL_Color){45,50,110,255});
    for(int ry=0;ry<2;ry++)for(int cc=0;cc<3;cc++) fillRect(5+cc*(W*2/5-8)/3,4+ry*8,2,2,(SDL_Color){255,255,255,255});
  } else if(region==2){
    fillRect(0,0,W,H,(SDL_Color){0,51,153,255});
    for(int i=0;i<12;i++){ double a=i*6.28318/12.0; int sx=W/2+(int)(cos(a)*W*0.30), sy=H/2+(int)(sin(a)*H*0.32);
      fillRect(sx-1,sy-1,2,2,(SDL_Color){255,204,0,255}); }
  }
  SDL_SetRenderTarget(g_ren,previous);
  SDL_RenderSetScale(g_ren,scaleX,scaleY);
  return t;
}
static void makeFlags(){ g_flag[1]=makeFlagTex(1,36,24); g_flag[2]=makeFlagTex(2,36,24); g_flag[3]=makeFlagTex(3,36,24); }

static SDL_Texture *g_gA=nullptr,*g_gB=nullptr,*g_gX=nullptr,*g_gY=nullptr,
                   *g_gPlus=nullptr,*g_gMinus=nullptr,
                   *g_gLeftRight=nullptr,*g_gUpDown=nullptr,*g_gTouch=nullptr;
// Supersampling keeps the downscaled glyphs crisp.
static const int GLYPH_SS = 3;
static SDL_Texture *makeGlyph(const char *label, bool pill){
  if(!g_font_sm || !g_font_big) return nullptr;
  const int S=GLYPH_SS, base=26;
  int H=base*S, W=(pill? base*8/5 : base)*S;
  SDL_Texture *t=SDL_CreateTexture(g_ren,SDL_PIXELFORMAT_RGBA8888,SDL_TEXTUREACCESS_TARGET,W,H);
  if(!t) return nullptr;
  SDL_SetTextureBlendMode(t,SDL_BLENDMODE_BLEND);
  SDL_Texture *previous=SDL_GetRenderTarget(g_ren);
  float scaleX=1.0f,scaleY=1.0f; SDL_RenderGetScale(g_ren,&scaleX,&scaleY);
  SDL_SetRenderTarget(g_ren,t);
  SDL_RenderSetScale(g_ren,1.0f,1.0f);
  SDL_SetRenderDrawColor(g_ren,0,0,0,0); SDL_RenderClear(g_ren);
  const SDL_Color face={242,244,246,255},ink={18,20,24,255};
  roundedRect(0,0,W,H,H/2,face);
  SDL_Surface *sf=TTF_RenderUTF8_Blended(g_font_big,label,ink);
  if(sf){ SDL_Texture *lt=SDL_CreateTextureFromSurface(g_ren,sf);
    if(lt) SDL_SetTextureBlendMode(lt,SDL_BLENDMODE_BLEND);
    int inner=H*56/100, lw=sf->w, lh=sf->h;
    if(lh>0){ lw=lw*inner/lh; lh=inner; }
    SDL_Rect d={(W-lw)/2,(H-lh)/2,lw,lh}; SDL_FreeSurface(sf);
    if(lt){ SDL_RenderCopy(g_ren,lt,nullptr,&d); SDL_DestroyTexture(lt); } }
  SDL_SetRenderTarget(g_ren,previous);
  SDL_RenderSetScale(g_ren,scaleX,scaleY);
  return t;
}
static void makeGlyphs(){
  g_gA=makeGlyph("A",false); g_gB=makeGlyph("B",false);
  g_gX=makeGlyph("X",false); g_gY=makeGlyph("Y",false);
  g_gPlus=makeGlyph("+",false); g_gMinus=makeGlyph("-",false);
  g_gLeftRight=makeGlyph("< >",true);
  g_gUpDown=makeGlyph("^ v",true);
  g_gTouch=makeGlyph("Touch",true);
}

static void destroyGlyphs(){
  SDL_Texture **glyphs[] = { &g_gA, &g_gB, &g_gX, &g_gY, &g_gPlus, &g_gMinus,
                            &g_gLeftRight, &g_gUpDown, &g_gTouch };
  for(SDL_Texture **glyph:glyphs){
    if(*glyph) SDL_DestroyTexture(*glyph);
    *glyph=nullptr;
  }
}

static PlSharedFontType requestedLauncherFontType(){
  const std::string_view language=LauncherLocalization::ActiveLanguage();
  if(language=="zh-CN") return PlSharedFontType_ChineseSimplified;
  if(language=="zh-TW") return PlSharedFontType_ChineseTraditional;
  return PlSharedFontType_Standard;
}

static bool openLauncherFontSet(PlSharedFontType type,TTF_Font *&small,TTF_Font *&normal,
                                TTF_Font *&large,TTF_Font *&caption,TTF_Font *&hint){
  PlFontData data{};
  if(R_FAILED(plGetSharedFontByType(&data,type))||!data.address||!data.size||data.size>INT_MAX) return false;
  auto openFont=[&](int size)->TTF_Font*{
    SDL_RWops *rw=SDL_RWFromConstMem(data.address,(int)data.size);
    return rw?TTF_OpenFontRW(rw,1,(int)std::lround(size*g_uiScale)):nullptr;
  };
  small=openFont(20);
  normal=openFont(26);
  large=openFont(32);
  caption=openFont(14);
  hint=openFont(18);
  if(small&&normal&&large&&caption&&hint) return true;
  if(small) TTF_CloseFont(small);
  if(normal) TTF_CloseFont(normal);
  if(large) TTF_CloseFont(large);
  if(caption) TTF_CloseFont(caption);
  if(hint) TTF_CloseFont(hint);
  small=normal=large=caption=hint=nullptr;
  return false;
}

static bool loadLauncherFonts(){
  const PlSharedFontType requested=requestedLauncherFontType();
  if(g_requestedFontType==requested&&g_font_sm&&g_font&&g_font_big&&g_font_caption&&g_font_hint&&
     g_fontScale==g_uiScale) return true;
  TTF_Font *small=nullptr,*normal=nullptr,*large=nullptr,*caption=nullptr,*hint=nullptr;
  if(!openLauncherFontSet(requested,small,normal,large,caption,hint)) return false;
  clearTextCaches();
  destroyGlyphs();
  if(g_font_sm) TTF_CloseFont(g_font_sm);
  if(g_font) TTF_CloseFont(g_font);
  if(g_font_big) TTF_CloseFont(g_font_big);
  if(g_font_caption) TTF_CloseFont(g_font_caption);
  if(g_font_hint) TTF_CloseFont(g_font_hint);
  g_fontScale=g_uiScale;
  g_font_sm=small; g_font=normal; g_font_big=large; g_font_caption=caption; g_font_hint=hint;
  g_requestedFontType=requested;
  makeGlyphs();
  return true;
}

struct FootItem { const char *button; const char *label; };
static SDL_Rect g_footHit[10];
static int g_footButton[10];

static SDL_Texture *buttonGlyph(const char *button){
  if(!button) return nullptr;
  if(!strcmp(button,"A")) return g_gA;
  if(!strcmp(button,"B")) return g_gB;
  if(!strcmp(button,"X")) return g_gX;
  if(!strcmp(button,"Y")) return g_gY;
  if(!strcmp(button,"+")) return g_gPlus;
  if(!strcmp(button,"-")) return g_gMinus;
  if(!strcmp(button,"Left / Right")) return g_gLeftRight;
  if(!strcmp(button,"Up / Down")) return g_gUpDown;
  if(!strcmp(button,"Touch")) return g_gTouch;
  return nullptr;
}

static void buttonHintSize(const char *button,int &width,int &height){
  SDL_Texture *glyph=buttonGlyph(button);
  width=height=0;
  if(glyph){
    SDL_QueryTexture(glyph,nullptr,nullptr,&width,&height);
    width/=GLYPH_SS; height/=GLYPH_SS;
  } else if(button&&button[0]) {
    width=textW(g_font_hint,button)+14;
    height=fontHeight(g_font_hint)+6;
  }
}

static void drawButtonGlyph(int x,int cy,const char *button){
  SDL_Texture *glyph=buttonGlyph(button);
  int width=0,height=0; buttonHintSize(button,width,height);
  if(glyph){
    SDL_Rect destination={x,cy-height/2,width,height};
    SDL_RenderCopy(g_ren,glyph,nullptr,&destination);
  } else if(button&&button[0]) {
    roundedPanel(x,cy-height/2,width,height,COL_CARD,COL_DIM,height/2);
    drawTextC(g_font_hint,x+width/2,cy-fontHeight(g_font_hint)/2,button,COL_TXT);
  }
}

struct FooterLayout {
  int itemWidth[10]{},gapAfter[10]{},rowStart[10]{},rowEnd[10]{},rowWidth[10]{};
  int rowCount=0,rowSpacing=0,height=0;
};
static FooterLayout measureFooter(const FootItem *it,int n){
  FooterLayout result;
  const int gap=8,pairGap=26,glyphGap=16;
  const int fh=fontHeight(g_font_hint),maxWidth=std::max(80,SW-160);
  for(int i=0;i<n&&i<10;i++){
    int gw=0,gh=0; buttonHintSize(it[i].button,gw,gh);
    const bool label=it[i].label&&it[i].label[0];
    result.itemWidth[i]=gw+(label?gap+textW(g_font_hint,tr(it[i].label)):0);
    result.gapAfter[i]=label?pairGap:glyphGap;
  }
  for(int first=0;first<n&&first<10;){
    int last=first,width=0;
    while(last<n&&last<10){
      const int added=result.itemWidth[last]+(last>first?result.gapAfter[last-1]:0);
      if(last>first&&width+added>maxWidth) break;
      width+=added; last++;
    }
    result.rowStart[result.rowCount]=first;
    result.rowEnd[result.rowCount]=last;
    result.rowWidth[result.rowCount]=width;
    result.rowCount++; first=last;
  }
  result.rowSpacing=fh+22;
  result.height=(std::max(1,result.rowCount)-1)*result.rowSpacing+fh+32;
  return result;
}

static void drawLauncherMark(int cy){
  if(!g_logo) return;
  int w=0,h=0; SDL_QueryTexture(g_logo,nullptr,nullptr,&w,&h);
  if(h<1) return;
  const SDL_Rect rect{40,cy-48,w*64/h,64};
  SDL_RenderCopy(g_ren,g_logo,nullptr,&rect);
}

static void drawFooterHints(const FootItem *it,int n,int cy){
  const FooterLayout layout=measureFooter(it,n);
  const int rowCount=layout.rowCount,rowSpacing=layout.rowSpacing;
  const int fh=fontHeight(g_font_hint),gap=8;
  const bool screenBottom=cy>=SH-64;
  if(screenBottom) drawLauncherMark(cy);
  g_footN=0;
  for(int row=0;row<rowCount;row++){
    const int rowY=cy-(rowCount-1-row)*rowSpacing;
    int x=screenBottom?SW-40-layout.rowWidth[row]:(SW-layout.rowWidth[row])/2;
    for(int i=layout.rowStart[row];i<layout.rowEnd[row];i++){
      int gw=0,gh=0; buttonHintSize(it[i].button,gw,gh);
      const int x0=x;
      drawButtonGlyph(x,rowY,it[i].button);
      x+=gw;
      const bool label=it[i].label&&it[i].label[0];
      if(label){
        const char *localized=tr(it[i].label);
        x+=gap;
        drawText(g_font_hint,x,rowY-fh/2,localized,COL_TXT);
        x+=textW(g_font_hint,localized);
      }
      if(g_footN<10){
        g_footHit[g_footN]={x0-4,rowY-std::max(gh,fh)/2-8,(x-x0)+8,std::max(gh,fh)+16};
        const char *button=it[i].button;
        g_footButton[g_footN]=
          !button?-1:
          !strcmp(button,"A")?BTN_CONFIRM:
          !strcmp(button,"B")?BTN_CANCEL:
          !strcmp(button,"X")?BTN_X:
          !strcmp(button,"Y")?BTN_Y:
          !strcmp(button,"+")?SDL_CONTROLLER_BUTTON_START:-1;
        g_footN++;
      }
      if(i+1<layout.rowEnd[row]) x+=layout.gapAfter[i];
    }
  }
}

static std::array<FootItem,3> libraryFooter(){
  return {{{"A","Launch"},{"Y","Game Menu"},{"B","Quit"}}};
}

static void drawSettingsFooter(const char *text,int cy=-1){
  if(!text||!text[0]) return;
  std::vector<std::string> tokens;
  const std::string source=text;
  size_t cursor=0;
  while(cursor<source.size()){
    while(cursor<source.size()&&source[cursor]==' ') cursor++;
    if(cursor>=source.size()) break;
    size_t end=cursor;
    while(end<source.size()&&!(source[end]==' '&&end+1<source.size()&&source[end+1]==' ')) end++;
    std::string token=trim(source.substr(cursor,end-cursor));
    if(!token.empty()) tokens.push_back(std::move(token));
    cursor=end;
    while(cursor<source.size()&&source[cursor]==' ') cursor++;
  }
  std::vector<FootItem> hints;
  hints.reserve(tokens.size()/2);
  for(size_t index=0;index+1<tokens.size();index+=2)
    hints.push_back({tokens[index].c_str(),tokens[index+1].c_str()});
  if(hints.empty()) drawTextC(g_font_sm,SW/2,cy>=0?cy-fontHeight(g_font_sm)/2:SH-38,text,COL_DIM);
  else drawFooterHints(hints.data(),(int)hints.size(),cy>=0?cy:SH-26);
}

static bool pressFooterButton(int x,int y){
  for(int i=0;i<g_footN;i++){
    const SDL_Rect &rect=g_footHit[i];
    if(g_footButton[i]<0||x<rect.x||x>=rect.x+rect.w||y<rect.y||y>=rect.y+rect.h) continue;
    SDL_Event press{}; press.type=SDL_CONTROLLERBUTTONDOWN;
    press.cbutton.button=(Uint8)g_footButton[i];
    return SDL_PushEvent(&press)==1;
  }
  return false;
}

static int drawButtonHint(int x,int cy,const char *button,const char *label) {
  int width=0,height=0; buttonHintSize(button,width,height);
  drawButtonGlyph(x,cy,button);
  if(label&&label[0]) drawText(g_font_hint,x+width+8,cy-fontHeight(g_font_hint)/2,label,COL_DIM);
  return width+((label&&label[0])?8+textW(g_font_hint,label):0);
}


// Semantic helper for static UI hints. Dynamic paths, game names and error
// text never pass through localization or the low-level drawing routines.
static void drawLocalizedFooter(const char *text,int centerY=-1){
  if(!text||!text[0]) return;
  std::vector<std::string> tokens;
  const size_t length=strlen(text); size_t cursor=0;
  while(cursor<length){
    while(cursor<length&&text[cursor]==' ') cursor++;
    if(cursor>=length) break;
    const size_t start=cursor;
    while(cursor<length&&!(cursor+1<length&&text[cursor]==' '&&text[cursor+1]==' ')) cursor++;
    tokens.emplace_back(trim(std::string(text+start,cursor-start)));
    while(cursor<length&&text[cursor]==' ') cursor++;
  }
  std::string localized;
  for(size_t index=0;index<tokens.size();index++){
    if(index) localized+="       ";
    localized+=index%2?std::string(LauncherLocalization::Translate(tokens[index])):tokens[index];
  }
  drawSettingsFooter(localized.c_str(),centerY);
}

enum TouchKind { TOUCH_NONE, TOUCH_TAP, TOUCH_SWIPE_L, TOUCH_SWIPE_R, TOUCH_SCROLL_UP, TOUCH_SCROLL_DOWN };
struct TouchG {
  bool active=false, vertical=false;
  SDL_FingerID fid=0;
  float x0=0,y0=0,lastY=0;
  Uint32 t0=0;
};
static TouchG g_touch;
static int g_touchScrollSteps=1;
static TouchKind touchFeed(const SDL_Event &e,int *ox,int *oy){
  const int TAP_MOVE=26, SWIPE_DX=90, SCROLL_STEP=30; const Uint32 TAP_MS=400;
  if(e.type==SDL_FINGERDOWN){
    if(g_touch.active && SDL_GetTicks()-g_touch.t0 < 2000) return TOUCH_NONE;
    g_touch.active=true; g_touch.vertical=false; g_touch.fid=e.tfinger.fingerId;
    g_touch.x0=e.tfinger.x*SW; g_touch.y0=e.tfinger.y*SH; g_touch.lastY=g_touch.y0; g_touch.t0=SDL_GetTicks();
  } else if(e.type==SDL_FINGERMOTION && g_touch.active && e.tfinger.fingerId==g_touch.fid){
    float ux=e.tfinger.x*SW, uy=e.tfinger.y*SH, dx=ux-g_touch.x0, dy=uy-g_touch.y0;
    if(!g_touch.vertical && fabsf(dy)>TAP_MOVE && fabsf(dy)>fabsf(dx)*1.15f) g_touch.vertical=true;
    if(g_touch.vertical){
      float step=uy-g_touch.lastY;
      if(fabsf(step)>=SCROLL_STEP){
        g_touchScrollSteps=std::min(6,std::max(1,(int)(fabsf(step)/SCROLL_STEP)));
        g_touch.lastY=uy;
        if(ox) *ox=(int)ux;
        if(oy) *oy=(int)uy;
        return step<0?TOUCH_SCROLL_UP:TOUCH_SCROLL_DOWN;
      }
    }
  } else if(e.type==SDL_FINGERUP && g_touch.active && e.tfinger.fingerId==g_touch.fid){
    g_touch.active=false;
    float ux=e.tfinger.x*SW, uy=e.tfinger.y*SH, dx=ux-g_touch.x0, dy=uy-g_touch.y0;
    Uint32 dt=SDL_GetTicks()-g_touch.t0;
    if(ox) *ox=(int)ux;
    if(oy) *oy=(int)uy;
    if(g_touch.vertical || (fabsf(dy)>=55 && fabsf(dy)>fabsf(dx)*1.15f)){
      float remaining=uy-g_touch.lastY;
      if(fabsf(remaining)<18 && g_touch.vertical) return TOUCH_NONE;
      g_touchScrollSteps=std::min(6,std::max(1,(int)(fabsf(g_touch.vertical?remaining:dy)/SCROLL_STEP)));
      return (g_touch.vertical?remaining:dy)<0?TOUCH_SCROLL_UP:TOUCH_SCROLL_DOWN;
    }
    if(fabsf(dx)>=SWIPE_DX && fabsf(dx)>fabsf(dy)*1.5f) return dx<0?TOUCH_SWIPE_L:TOUCH_SWIPE_R;
    if(fabsf(dx)<=TAP_MOVE && fabsf(dy)<=TAP_MOVE && dt<=TAP_MS)
      return pressFooterButton((int)ux,(int)uy)?TOUCH_NONE:TOUCH_TAP;
  }
  return TOUCH_NONE;
}

static bool touchScrollList(TouchKind kind,int &sel,int &top,int count,int visible){
  if((kind!=TOUCH_SCROLL_UP && kind!=TOUCH_SCROLL_DOWN) || count<=0) return false;
  const int previous=sel;
  int delta=(kind==TOUCH_SCROLL_UP?1:-1)*g_touchScrollSteps;
  sel=std::max(0,std::min(count-1,sel+delta));
  if(sel<top) top=sel;
  if(sel>=top+visible) top=sel-visible+1;
  if(top<0) top=0;
  if(sel!=previous) uiAudioPlay(UiSound::Navigate);
  return true;
}

static bool g_stickXLatched=false, g_stickYLatched=false;
static char stickNav(const SDL_Event &e){
  const int TH=18000, DZ=8000;
  if(e.type!=SDL_CONTROLLERAXISMOTION) return 0;
  if(e.caxis.axis==SDL_CONTROLLER_AXIS_LEFTX){
    if(!g_stickXLatched && e.caxis.value<-TH){ g_stickXLatched=true; return 'L'; }
    if(!g_stickXLatched && e.caxis.value> TH){ g_stickXLatched=true; return 'R'; }
    if(e.caxis.value>-DZ && e.caxis.value<DZ) g_stickXLatched=false;
  } else if(e.caxis.axis==SDL_CONTROLLER_AXIS_LEFTY){
    if(!g_stickYLatched && e.caxis.value<-TH){ g_stickYLatched=true; return 'U'; }
    if(!g_stickYLatched && e.caxis.value> TH){ g_stickYLatched=true; return 'D'; }
    if(e.caxis.value>-DZ && e.caxis.value<DZ) g_stickYLatched=false;
  }
  return 0;
}
static void pumpStick(const SDL_Event &e){
  char n=stickNav(e); if(!n) return;
  SDL_Event s; memset(&s,0,sizeof(s));
  s.type=SDL_CONTROLLERBUTTONDOWN;
  s.cbutton.button = n=='U'?SDL_CONTROLLER_BUTTON_DPAD_UP : n=='D'?SDL_CONTROLLER_BUTTON_DPAD_DOWN
                   : n=='L'?SDL_CONTROLLER_BUTTON_DPAD_LEFT : SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
  SDL_PushEvent(&s);
}

static bool g_libraryTriggers[2]={false,false};

static SDL_GameController *g_pad=nullptr;
static bool g_exitRequested=false;
static int g_navHeld=0;
static Uint32 g_navSince=0,g_navLast=0;
static std::deque<SDL_Event> g_waitedEvents;
static Uint32 g_fxT=0,g_redrawUntil=0;
static std::string g_toastMessage;
static Uint32 g_toastUntil=0;

static void openController(int index) {
  if (!g_pad && index >= 0 && SDL_IsGameController(index))
    g_pad = SDL_GameControllerOpen(index);
}

static void closeController() {
  if (!g_pad) return;
  SDL_GameControllerClose(g_pad);
  g_pad = nullptr;
  g_stickXLatched = g_stickYLatched = false;
  g_navHeld = 0;
  g_navSince = g_navLast = 0;
  g_libraryTriggers[0]=g_libraryTriggers[1]=false;
}

static void handleOutputSizeChange() {
  if (!g_ren) return;
  int width=0,height=0;
  if (SDL_GetRendererOutputSize(g_ren,&width,&height)!=0||width<1||height<1) return;
  if (width==g_outputW&&height==g_outputH) return;
  g_outputW=width; g_outputH=height;
  configureLauncherScale();
  if (g_fontScale!=g_uiScale) loadLauncherFonts();
  clearTextCaches();
  g_textScroll.clear();
  g_redrawUntil=SDL_GetTicks()+220;
}

static bool beginUiFrame() {
  if (g_exitRequested) return false;
  if (!appletMainLoop()) {
    g_exitRequested = true;
    return false;
  }
  handleOutputSizeChange();
  if(g_pad&&!SDL_GameControllerGetAttached(g_pad)) closeController();
  pumpCoverDecodeResults();
  return true;
}

static int keyboardNavigationButton(SDL_Keycode key) {
  switch(key){
    case SDLK_RETURN: case SDLK_KP_ENTER: return BTN_CONFIRM;
    case SDLK_ESCAPE: return BTN_CANCEL;
    case SDLK_UP: return SDL_CONTROLLER_BUTTON_DPAD_UP;
    case SDLK_DOWN: return SDL_CONTROLLER_BUTTON_DPAD_DOWN;
    case SDLK_LEFT: return SDL_CONTROLLER_BUTTON_DPAD_LEFT;
    case SDLK_RIGHT: return SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
    case SDLK_PAGEUP: return SDL_CONTROLLER_BUTTON_LEFTSHOULDER;
    case SDLK_PAGEDOWN: return SDL_CONTROLLER_BUTTON_RIGHTSHOULDER;
    case SDLK_y: case SDLK_SPACE: return BTN_Y;
    case SDLK_F1: return BTN_X;
    case SDLK_x: return BTN_X;
    default: return -1;
  }
}

static bool pollUiEvent(SDL_Event &event) {
  for (;;) {
    if(!g_waitedEvents.empty()){
      event=g_waitedEvents.front();
      g_waitedEvents.pop_front();
    } else if(!SDL_PollEvent(&event)) return false;
    if(event.type==SDL_WINDOWEVENT&&
       (event.window.event==SDL_WINDOWEVENT_SIZE_CHANGED||
        event.window.event==SDL_WINDOWEVENT_RESIZED)){
      handleOutputSizeChange();
      continue;
    }
    if(event.type==SDL_CONTROLLERBUTTONDOWN||event.type==SDL_CONTROLLERAXISMOTION||
       event.type==SDL_FINGERDOWN||event.type==SDL_FINGERMOTION||event.type==SDL_FINGERUP||
       event.type==SDL_KEYDOWN)
      g_redrawUntil=SDL_GetTicks()+220;
    if (event.type == SDL_QUIT) {
      g_exitRequested = true;
      continue;
    }
    if (event.type == SDL_CONTROLLERDEVICEADDED) {
      openController(event.cdevice.which);
      continue;
    }
    if (event.type == SDL_CONTROLLERDEVICEREMOVED) {
      if (g_pad) {
        SDL_Joystick *joystick = SDL_GameControllerGetJoystick(g_pad);
        if (joystick && SDL_JoystickInstanceID(joystick) == event.cdevice.which)
          closeController();
      }
      continue;
    }
    if(event.type==SDL_KEYDOWN){
      const int button=keyboardNavigationButton(event.key.keysym.sym);
      if(button>=0){
        SDL_Event press{};
        press.type=SDL_CONTROLLERBUTTONDOWN;
        press.cbutton.button=(Uint8)button;
        SDL_PushEvent(&press);
      }
    }
    if (event.type == SDL_CONTROLLERBUTTONDOWN) {
      switch (event.cbutton.button) {
        case BTN_CONFIRM: uiAudioPlay(UiSound::Confirm); break;
        case BTN_CANCEL: uiAudioPlay(UiSound::Back); break;
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
          uiAudioPlay(UiSound::Navigate); break;
        default: break;
      }
    }
    return true;
  }
}

// Wait until input, a worker notification, or the next animated frame.  Static
// screens no longer spin continuously, while animated themes keep a steady
// presentation cadence without the old fixed eight-millisecond busy loop.
static void waitForNextUiFrame(bool forceAnimation=false) {
  for(;;){
    SDL_Event event{};
    const Uint32 now=SDL_GetTicks();
    const bool transientAnimation=g_uiAnimations&&!SDL_TICKS_PASSED(now,g_redrawUntil);
    const bool animate=forceAnimation||g_scrollTextActive||
                       (g_uiAnimations&&hasAnimatedBackground())||transientAnimation||
                       g_navHeld!=0||g_touch.active;
    int frameDeadline=animate?16:-1;
    if(!g_toastMessage.empty()){
      if(SDL_TICKS_PASSED(now,g_toastUntil)){ g_toastMessage.clear(); return; }
      const int remaining=(int)(g_toastUntil-now);
      frameDeadline=frameDeadline<0?remaining:std::min(frameDeadline,remaining);
    }
    // libnx workers have lower priority; an immediate swap must not starve them.
    if(animate&&g_presentVsync){ SDL_Delay(1); return; }
    const int timeout=frameDeadline<0?250:std::min(frameDeadline,250);
    if(SDL_WaitEventTimeout(&event,timeout)){ g_waitedEvents.push_back(event); return; }
    if(!appletMainLoop()){ g_exitRequested=true; return; }
    if(frameDeadline>=0) return;
  }
}

static void navRepeat(){
  if(!g_pad||!SDL_GameControllerGetAttached(g_pad)) return;
  const int TH=18000;
  int dir=0;
  if(SDL_GameControllerGetButton(g_pad,SDL_CONTROLLER_BUTTON_DPAD_UP)   || SDL_GameControllerGetAxis(g_pad,SDL_CONTROLLER_AXIS_LEFTY)<-TH) dir=SDL_CONTROLLER_BUTTON_DPAD_UP;
  else if(SDL_GameControllerGetButton(g_pad,SDL_CONTROLLER_BUTTON_DPAD_DOWN) || SDL_GameControllerGetAxis(g_pad,SDL_CONTROLLER_AXIS_LEFTY)> TH) dir=SDL_CONTROLLER_BUTTON_DPAD_DOWN;
  else if(SDL_GameControllerGetButton(g_pad,SDL_CONTROLLER_BUTTON_DPAD_LEFT)  || SDL_GameControllerGetAxis(g_pad,SDL_CONTROLLER_AXIS_LEFTX)<-TH) dir=SDL_CONTROLLER_BUTTON_DPAD_LEFT;
  else if(SDL_GameControllerGetButton(g_pad,SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || SDL_GameControllerGetAxis(g_pad,SDL_CONTROLLER_AXIS_LEFTX)> TH) dir=SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
  Uint32 now=SDL_GetTicks();
  if(dir!=g_navHeld){ g_navHeld=dir; g_navSince=now; g_navLast=now; return; }
  if(!dir) return;
  const Uint32 DELAY=360, RATE=85;
  if(now-g_navSince<DELAY || now-g_navLast<RATE) return;
  g_navLast=now;
  SDL_Event s; memset(&s,0,sizeof(s)); s.type=SDL_CONTROLLERBUTTONDOWN; s.cbutton.button=(Uint8)dir;
  SDL_PushEvent(&s);
}

struct Game {
  std::string path;
  std::string storageId;
  std::string file;
  std::string title;
  std::string key;
  std::string legacyKey;
  uint64_t fingerprint = 0;
  long long fileSize = 0;
  long long modified = 0;
  uint64_t titleId = 0;
  std::string titleIdError;
  std::string iconPath;
  SDL_Texture *cover = nullptr;
  Uint32 coverAt = 0;
  Uint64 coverUse = 0;
  Uint64 coverRequest = 0;
  bool coverQueued = false;
  bool triedCover = false;
  SDL_Texture *background=nullptr;
  Uint64 backgroundUse=0,backgroundRequest=0;
  bool backgroundQueued=false,triedBackground=false;
  bool hasCfg = false;
  int region = 0;
  long long added = 0;
  long long played = 0;
};
static std::string foldedKey(std::string key);
static void releaseGameArtwork(Game &game){
  if(game.cover) SDL_DestroyTexture(game.cover);
  if(game.background) SDL_DestroyTexture(game.background);
  game.cover=game.background=nullptr;
}

static std::vector<Game> g_games;
static std::vector<Game*> g_visibleGames;
static std::unordered_set<std::string> g_favorites;
enum class LibraryView { Recent, All, Favorites };
static LibraryView g_libraryView=LibraryView::All;
static bool g_libraryHeaderFocused=false;
static int g_libraryHeaderSelection=1;
static std::array<float,3> g_libraryTabOpen{0,1,0};
static Uint32 g_libraryAnimationTick=0;
static float g_recentPosition=-1;
static std::array<std::string,3> g_librarySelections;

static Uint64 g_coverUseSerial=0;
static constexpr size_t COVER_CACHE_LIMIT=64;
static constexpr size_t BACKGROUND_CACHE_LIMIT=4;
static std::string g_backgroundTarget,g_backgroundCurrent,g_backgroundPrevious;
static Uint32 g_backgroundSince=0;
static Store g_recent;
static const char *RECENT_INI="sdmc:/switch/cemu/recent.ini";

static void loadLibraryOrganization(){
  g_favorites.clear();
  const int count=std::clamp(atoi(storeGet(g_global,"Library/FavoriteCount","0")),0,65536);
  for(int index=0;index<count;index++){
    const char *id=storeGet(g_global,("Library/Favorite"+std::to_string(index)).c_str(),"");
    if(*id) g_favorites.insert(id);
  }
  g_libraryView=LibraryView::All;
}

static void saveLibraryOrganization(){
  storeRemovePrefix(g_global,"Library/Favorite");
  storeSet(g_global,"Library/FavoriteCount",std::to_string(g_favorites.size()).c_str());
  size_t index=0;
  for(const auto &id:g_favorites)
    storeSet(g_global,("Library/Favorite"+std::to_string(index++)).c_str(),id.c_str());
  storeSave(g_global,LAUNCHER_INI);
}

static void rebuildVisibleGames() {
  g_visibleGames.clear();
  for(Game &game:g_games){
    if(g_libraryView==LibraryView::Recent&&game.played<=0) continue;
    if(g_libraryView==LibraryView::Favorites&&!g_favorites.count(game.key)) continue;
    g_visibleGames.push_back(&game);
  }
  if(g_libraryView==LibraryView::Recent)
    std::stable_sort(g_visibleGames.begin(),g_visibleGames.end(),[](const Game *left,const Game *right){
      return left->played>right->played;
    });
}

static Game *visibleGame(int index) {
  return index>=0&&index<(int)g_visibleGames.size()?g_visibleGames[index]:nullptr;
}
static int visibleIndexForKey(const std::string &key)
{
  if(key.empty()) return 0;
  for(int index=0;index<(int)g_visibleGames.size();index++)
    if(g_visibleGames[index]->key==key) return index;
  return 0;
}

static int libraryTabCount(){ return 3; }

static int libraryTabIndex(){ return (int)g_libraryView; }

static void selectLibraryTab(int tab,int &selection){
  if(Game *game=visibleGame(selection)) g_librarySelections[libraryTabIndex()]=game->key;
  tab=std::clamp(tab,0,libraryTabCount()-1);
  g_libraryView=(LibraryView)tab;
  rebuildVisibleGames();
  selection=visibleIndexForKey(g_librarySelections[tab]);
  g_libraryHeaderSelection=tab;
  g_recentPosition=-1;
  beginScreenFx();
}

static int detectRegion(const std::string &file) {
  std::string tags; int depth = 0;
  for (char c : file) {
    if (c=='('||c=='[') depth++;
    else if (c==')'||c==']') { if (depth) depth--; if (depth==0) tags += '|'; }
    else if (depth) tags += (char)tolower((unsigned char)c);
  }
  auto has = [&](const char *s){ return tags.find(s) != std::string::npos; };
  if (has("japan")||has("ntsc-j")||has("jpn")||has("(j)")) return 3;
  if (has("usa")||has("ntsc-u")||has("america")||has("(u)")) return 1;
  if (has("europe")||has("pal")||has("australia")||has("(uk")||has("france")||
      has("germany")||has("spain")||has("ital")||has("(e)")) return 2;
  std::string l; for (char c : file) l += (char)tolower((unsigned char)c);
  if (l.find("ntsc-j")!=std::string::npos) return 3;
  if (l.find("ntsc-u")!=std::string::npos) return 1;
  return 0;
}
static void applySort() {
  std::sort(g_games.begin(),g_games.end(),[](const Game &left,const Game &right){
    return strcasecmp(left.title.c_str(),right.title.c_str())<0;
  });
  rebuildVisibleGames();
}
static void recordPlayed(const Game &game){
  long long seq = atoll(storeGet(g_global,"Wrapper/PlaySeq","0")) + 1;
  char b[24]; snprintf(b,sizeof(b),"%lld",seq);
  storeSet(g_global,"Wrapper/PlaySeq",b);
  storeSet(g_recent,game.key.c_str(),b);
}

static bool hasGameExt(const char *n) {
  const char *e = strrchr(n, '.');
  if (!e) return false;
  static const char *x[] = { ".wua",".wud",".wux",".wuhb",".rpx",".elf",".iso" };
  for (auto s : x) if (!strcasecmp(e, s)) return true;
  return false;
}
static std::string join(const std::string &b, const std::string &n) { std::string r=b; if(!r.empty()&&r.back()=='/') r.pop_back(); return r+"/"+n; }
static std::string foldedKey(std::string key);
static bool pathAtOrBelow(const std::string &path,const std::string &root);
static bool isUsbStoragePath(const std::string &path);

static std::string normalizeLocationPath(const std::string &input) {
  std::string path=trim(input);
  if(path.empty()) return {};
  std::string output;
  output.reserve(path.size()+1);
  bool slash=false;
  for(char c:path){
    if(c=='\\') c='/';
    if(c=='/'){
      if(slash) continue;
      slash=true;
    } else slash=false;
    output+=c;
  }
  size_t colon=output.find(':');
  if(colon!=std::string::npos && colon+1==output.size()) output+='/';
  size_t minimum=colon==std::string::npos?1:colon+2;
  while(output.size()>minimum && output.back()=='/') output.pop_back();
  return output;
}

static std::string pathIdentity(const std::string &input) {
  return foldedKey(normalizeLocationPath(input));
}

static std::string usbStableIdForPath(const std::string &path,
                                     const std::vector<SwitchStorage::Location> &locations) {
  for(const auto &location:locations)
    if(pathAtOrBelow(path,location.path)) return location.id;
  return {};
}
static std::string usbStableIdForPath(const std::string &path) {
  return usbStableIdForPath(path,SwitchStorage::ListUsbLocations());
}

struct GameSourceRecord {
  std::string storedPath;
  std::string runtimePath;
  std::string stableId;
  std::string physicalId;
  std::string relative;
  bool relativeValid=true;
};

static bool normalizeUsbRelative(const std::string &input,std::string &output) {
  std::string value=input;
  std::replace(value.begin(),value.end(),'\\','/');
  output.clear();
  size_t offset=0;
  while(offset<=value.size()){
    size_t slash=value.find('/',offset);
    std::string component=value.substr(offset,slash==std::string::npos?std::string::npos:slash-offset);
    if(component=="..") return false;
    if(!component.empty()&&component!="."){
      if(!output.empty()) output+='/';
      output+=component;
    }
    if(slash==std::string::npos) break;
    offset=slash+1;
  }
  return true;
}

static std::string unresolvedUsbSource(const std::string &stableId,const std::string &relative) {
  return "usb-id:"+stableId+(relative.empty()?std::string{}:"/"+relative);
}

static bool isUnresolvedUsbSource(const std::string &path) {
  return path.rfind("usb-id:",0)==0;
}

static bool parseUnresolvedUsbSource(const std::string &path,std::string &stableId,
                                     std::string &relative) {
  static constexpr const char *PREFIX="usb-id:";
  if(path.rfind(PREFIX,0)!=0) return false;
  const size_t slash=path.find('/',strlen(PREFIX));
  stableId=path.substr(strlen(PREFIX),slash==std::string::npos?std::string::npos:slash-strlen(PREFIX));
  const std::string rawRelative=slash==std::string::npos?std::string{}:path.substr(slash+1);
  return !stableId.empty()&&normalizeUsbRelative(rawRelative,relative);
}

static bool directoryExists(const std::string &path) {
  struct stat info{};
  return stat(path.c_str(),&info)==0&&S_ISDIR(info.st_mode);
}

static std::string gameSourceIdentity(const GameSourceRecord &record) {
  if(!record.stableId.empty()) return "usb:"+foldedKey(record.stableId)+"/"+foldedKey(record.relative);
  return pathIdentity(record.runtimePath.empty()?record.storedPath:record.runtimePath);
}

static bool bindMountedUsbPath(const std::string &path,
                               const std::vector<SwitchStorage::Location> &locations,
                               std::string &stableId,std::string &physicalId,
                               std::string &relative) {
  const std::string normalizedPath=normalizeLocationPath(path);
  for(const auto &location:locations){
    const std::string root=normalizeLocationPath(location.path);
    if(!pathAtOrBelow(normalizedPath,root)) continue;
    std::string rawRelative=normalizedPath.substr(std::min(normalizedPath.size(),root.size()));
    while(!rawRelative.empty()&&rawRelative.front()=='/') rawRelative.erase(rawRelative.begin());
    if(!normalizeUsbRelative(rawRelative,relative)) return false;
    stableId=location.id;
    physicalId=location.physicalId;
    return true;
  }
  return false;
}

static std::vector<GameSourceRecord> loadSavedGameSourceRecords() {
  std::vector<GameSourceRecord> records;
  int count=std::max(0,std::min(16,atoi(storeGet(g_global,"Wrapper/GamePathCount","1"))));
  for(int i=0;i<count;i++){
    const std::string prefix="Wrapper/GamePath"+std::to_string(i);
    GameSourceRecord record;
    record.storedPath=normalizeLocationPath(storeGet(g_global,prefix.c_str(),i==0?DEF_GAMEDIR:""));
    record.runtimePath=record.storedPath;
    record.stableId=storeGet(g_global,("Wrapper/GamePathStable"+std::to_string(i)).c_str(),"");
    record.physicalId=storeGet(g_global,("Wrapper/GamePathPhysical"+std::to_string(i)).c_str(),"");
    const std::string rawRelative=storeGet(g_global,("Wrapper/GamePathRelative"+std::to_string(i)).c_str(),"");
    record.relativeValid=normalizeUsbRelative(rawRelative,record.relative);
    if(!record.storedPath.empty()||!record.stableId.empty()) records.push_back(std::move(record));
  }
  return records;
}

static std::mutex g_gameSourceCacheMutex;
static std::vector<GameSourceRecord> g_gameSourceCache;
static uint64_t g_gameSourceCacheGeneration=0;
static bool g_gameSourceCacheValid=false;

static void invalidateGameSourceCache() {
  std::lock_guard<std::mutex> lock(g_gameSourceCacheMutex);
  g_gameSourceCacheValid=false;
}

static void persistGameSourceRecords(const std::vector<GameSourceRecord> &input) {
  std::vector<GameSourceRecord> records;
  std::unordered_set<std::string> seen;
  for(const auto &entry:input){
    GameSourceRecord record=entry;
    if(record.storedPath.empty()&&!isUnresolvedUsbSource(record.runtimePath))
      record.storedPath=normalizeLocationPath(record.runtimePath);
    const std::string identity=gameSourceIdentity(record);
    if(identity.empty()||!seen.insert(identity).second||records.size()>=16) continue;
    records.push_back(std::move(record));
  }
  storeRemovePrefix(g_global,"Wrapper/GamePath");
  storeSet(g_global,"Wrapper/GamePathCount",std::to_string(records.size()).c_str());
  for(size_t index=0;index<records.size();index++){
    const std::string pathKey="Wrapper/GamePath"+std::to_string(index);
    storeSet(g_global,pathKey.c_str(),records[index].storedPath.c_str());
    if(!records[index].stableId.empty()){
      storeSet(g_global,("Wrapper/GamePathStable"+std::to_string(index)).c_str(),records[index].stableId.c_str());
      if(!records[index].physicalId.empty())
        storeSet(g_global,("Wrapper/GamePathPhysical"+std::to_string(index)).c_str(),records[index].physicalId.c_str());
      storeSet(g_global,("Wrapper/GamePathRelative"+std::to_string(index)).c_str(),records[index].relative.c_str());
    }
  }
  invalidateGameSourceCache();
}

static bool resolveStableGameSource(GameSourceRecord &record,
                                    const std::vector<SwitchStorage::Location> &locations,
                                    bool allowRebind,bool &persistChanged) {
  if(record.stableId.empty()) return false;
  if(!record.relativeValid){
    record.runtimePath=unresolvedUsbSource(record.stableId,{});
    return false;
  }
  const auto stable=std::find_if(locations.begin(),locations.end(),[&](const auto &location){
    return location.id==record.stableId;
  });
  if(stable!=locations.end()){
    const std::string root=normalizeLocationPath(stable->path);
    const std::string candidate=normalizeLocationPath(root+record.relative);
    if(pathAtOrBelow(candidate,root)){
      record.runtimePath=candidate;
      if(pathIdentity(record.storedPath)!=pathIdentity(candidate)){
        record.storedPath=candidate;
        persistChanged=true;
      }
      if(record.physicalId!=stable->physicalId){
        record.physicalId=stable->physicalId;
        persistChanged=true;
      }
      return true;
    }
    record.runtimePath=unresolvedUsbSource(record.stableId,record.relative);
    return false;
  }
  if(allowRebind){
    const SwitchStorage::Location *physicalMatch=nullptr;
    std::string physicalPath;
    if(!record.physicalId.empty()) for(const auto &location:locations){
      if(location.physicalId!=record.physicalId) continue;
      const std::string root=normalizeLocationPath(location.path);
      const std::string candidate=normalizeLocationPath(root+record.relative);
      if(!pathAtOrBelow(candidate,root)||!directoryExists(candidate)) continue;
      if(physicalMatch){ physicalMatch=nullptr; physicalPath.clear(); break; }
      physicalMatch=&location;
      physicalPath=candidate;
    }
    if(physicalMatch){
      record.stableId=physicalMatch->id;
      record.storedPath=physicalPath;
      record.runtimePath=physicalPath;
      persistChanged=true;
      return true;
    }
    const SwitchStorage::Location *match=nullptr;
    std::string matchedPath;
    for(const auto &location:locations){
      const std::string root=normalizeLocationPath(location.path);
      const std::string candidate=normalizeLocationPath(root+record.relative);
      if(!pathAtOrBelow(candidate,root)||!directoryExists(candidate)) continue;
      if(match){ match=nullptr; matchedPath.clear(); break; }
      match=&location;
      matchedPath=candidate;
    }
    if(match){
      record.stableId=match->id;
      record.physicalId=match->physicalId;
      record.storedPath=matchedPath;
      record.runtimePath=matchedPath;
      persistChanged=true;
      return true;
    }
  }
  record.runtimePath=unresolvedUsbSource(record.stableId,record.relative);
  return false;
}

static std::vector<GameSourceRecord> resolveGameSourceRecords(std::vector<GameSourceRecord> records,
    const std::vector<SwitchStorage::Location> &locations,bool allowRebind,bool *persistChangedOutput) {
  bool persistChanged=false;
  std::vector<GameSourceRecord> resolved;
  std::unordered_set<std::string> seen;
  for(auto &record:records){
    if(!record.stableId.empty()){
      resolveStableGameSource(record,locations,allowRebind,persistChanged);
    } else if(allowRebind&&isUsbStoragePath(record.storedPath)){
      std::string stableId,physicalId,relative;
      if(directoryExists(record.storedPath)&&
         bindMountedUsbPath(record.storedPath,locations,stableId,physicalId,relative)){
        record.stableId=std::move(stableId);
        record.physicalId=std::move(physicalId);
        record.relative=std::move(relative);
        record.relativeValid=true;
        record.runtimePath=record.storedPath;
        persistChanged=true;
      } else if(!directoryExists(record.storedPath)){
        const size_t colon=record.storedPath.find(':');
        const std::string rawRelative=colon==std::string::npos?std::string{}:record.storedPath.substr(colon+1);
        record.relativeValid=normalizeUsbRelative(rawRelative,record.relative);
        const SwitchStorage::Location *match=nullptr;
        std::string matchedPath;
        if(record.relativeValid) for(const auto &location:locations){
          const std::string root=normalizeLocationPath(location.path);
          const std::string candidate=normalizeLocationPath(root+record.relative);
          if(!pathAtOrBelow(candidate,root)||!directoryExists(candidate)) continue;
          if(match){ match=nullptr; matchedPath.clear(); break; }
          match=&location;
          matchedPath=candidate;
        }
        if(match){
          record.stableId=match->id;
          record.physicalId=match->physicalId;
          record.storedPath=matchedPath;
          record.runtimePath=matchedPath;
          persistChanged=true;
        }
      }
    }
    const std::string identity=gameSourceIdentity(record);
    if(identity.empty()||!seen.insert(identity).second){
      if(allowRebind) persistChanged=true;
      continue;
    }
    resolved.push_back(std::move(record));
  }
  if(persistChangedOutput) *persistChangedOutput=persistChanged;
  return resolved;
}

static void cacheGameSourceRecords(const std::vector<GameSourceRecord> &records,uint64_t generation) {
  std::lock_guard<std::mutex> lock(g_gameSourceCacheMutex);
  g_gameSourceCache=records;
  g_gameSourceCacheGeneration=generation;
  g_gameSourceCacheValid=true;
}

static std::vector<GameSourceRecord> resolvedGameSourceRecords(bool allowRebind,
                                                               bool *persistChangedOutput=nullptr) {
  const auto snapshot=SwitchStorage::GetUsbSnapshot();
  if(!allowRebind){
    std::lock_guard<std::mutex> lock(g_gameSourceCacheMutex);
    if(g_gameSourceCacheValid&&g_gameSourceCacheGeneration==snapshot.generation){
      if(persistChangedOutput) *persistChangedOutput=false;
      return g_gameSourceCache;
    }
  }
  auto records=resolveGameSourceRecords(loadSavedGameSourceRecords(),snapshot.locations,
                                       allowRebind,persistChangedOutput);
  cacheGameSourceRecords(records,snapshot.generation);
  return records;
}

static std::vector<std::string> loadGameSources() {
  std::vector<std::string> paths;
  for(auto &record:resolvedGameSourceRecords(false))
    if(!record.runtimePath.empty()) paths.push_back(std::move(record.runtimePath));
  return paths;
}

static void saveGameSources(const std::vector<std::string> &input) {
  const auto existing=resolvedGameSourceRecords(false);
  const auto locations=SwitchStorage::ListUsbLocations();
  std::vector<GameSourceRecord> records;
  std::unordered_set<std::string> seen;
  for(const auto &entry:input){
    GameSourceRecord record;
    record.runtimePath=normalizeLocationPath(entry);
    if(record.runtimePath.empty()) continue;
    if(parseUnresolvedUsbSource(record.runtimePath,record.stableId,record.relative)){
      const auto old=std::find_if(existing.begin(),existing.end(),[&](const auto &candidate){
        return candidate.stableId==record.stableId&&foldedKey(candidate.relative)==foldedKey(record.relative);
      });
      if(old!=existing.end()){
        record.storedPath=old->storedPath;
        record.physicalId=old->physicalId;
      }
    } else {
      record.storedPath=record.runtimePath;
      if(!directoryExists(record.runtimePath)||
         !bindMountedUsbPath(record.runtimePath,locations,record.stableId,record.physicalId,record.relative)){
        const auto old=std::find_if(existing.begin(),existing.end(),[&](const auto &candidate){
          return pathIdentity(candidate.runtimePath)==pathIdentity(record.runtimePath)||
                 pathIdentity(candidate.storedPath)==pathIdentity(record.runtimePath);
        });
        if(old!=existing.end()){
          record.stableId=old->stableId;
          record.physicalId=old->physicalId;
          record.relative=old->relative;
          record.relativeValid=old->relativeValid;
        }
      }
    }
    const std::string identity=gameSourceIdentity(record);
    if(identity.empty()||!seen.insert(identity).second||records.size()>=16) continue;
    records.push_back(std::move(record));
  }
  persistGameSourceRecords(records);
}

static std::vector<std::string> loadFavoriteFolders() {
  std::vector<std::string> paths;
  int count=std::max(0,std::min(24,atoi(storeGet(g_global,"Browser/FavoriteCount","0"))));
  std::unordered_set<std::string> seen;
  for(int i=0;i<count;i++){
    std::string key="Browser/Favorite"+std::to_string(i);
    std::string path=normalizeLocationPath(storeGet(g_global,key.c_str(),""));
    if(!path.empty() && seen.insert(pathIdentity(path)).second) paths.push_back(std::move(path));
  }
  return paths;
}

static void saveFavoriteFolders(const std::vector<std::string> &input) {
  std::vector<std::string> paths;
  std::unordered_set<std::string> seen;
  for(const auto &entry:input){
    std::string path=normalizeLocationPath(entry);
    if(!path.empty()&&seen.insert(pathIdentity(path)).second&&paths.size()<24) paths.push_back(std::move(path));
  }
  storeRemovePrefix(g_global,"Browser/Favorite");
  storeSet(g_global,"Browser/FavoriteCount",std::to_string(paths.size()).c_str());
  for(size_t i=0;i<paths.size();i++){
    std::string key="Browser/Favorite"+std::to_string(i);
    storeSet(g_global,key.c_str(),paths[i].c_str());
  }
  storeSave(g_global,LAUNCHER_INI);
}

static std::vector<SwitchStorage::SmbShare> loadSmbSharesFromStore() {
  std::vector<SwitchStorage::SmbShare> shares;
  std::unordered_set<std::string> ids;
  int count=std::max(0,std::min(8,atoi(storeGet(g_global,"Storage/SmbCount","0"))));
  for(int i=0;i<count;i++){
    std::string prefix="Storage/Smb"+std::to_string(i);
    SwitchStorage::SmbShare share;
    share.id=storeGet(g_global,(prefix+"Id").c_str(),"");
    share.name=storeGet(g_global,(prefix+"Name").c_str(),"");
    share.server=storeGet(g_global,(prefix+"Server").c_str(),"");
    share.share=storeGet(g_global,(prefix+"Share").c_str(),"");
    share.path=storeGet(g_global,(prefix+"Path").c_str(),"");
    share.user=storeGet(g_global,(prefix+"User").c_str(),"");
    share.password=storeGet(g_global,(prefix+"Password").c_str(),"");
    share.domain=storeGet(g_global,(prefix+"Domain").c_str(),"");
    const char *automatic=storeGet(g_global,(prefix+"AutoMount").c_str(),"true");
    share.autoMount=!strcmp(automatic,"true")||!strcmp(automatic,"1");
    if(!SwitchStorage::SmbRootPath(share.id).empty()&&!share.server.empty()&&!share.share.empty()&&ids.insert(share.id).second)
      shares.push_back(std::move(share));
  }
  return shares;
}

static void saveSmbShares(const std::vector<SwitchStorage::SmbShare> &shares) {
  storeRemovePrefix(g_global,"Storage/Smb");
  storeSet(g_global,"Storage/SmbCount",std::to_string(shares.size()).c_str());
  for(size_t i=0;i<shares.size();i++){
    const auto &share=shares[i]; std::string prefix="Storage/Smb"+std::to_string(i);
    storeSet(g_global,(prefix+"Id").c_str(),share.id.c_str());
    storeSet(g_global,(prefix+"Name").c_str(),share.name.c_str());
    storeSet(g_global,(prefix+"Server").c_str(),share.server.c_str());
    storeSet(g_global,(prefix+"Share").c_str(),share.share.c_str());
    storeSet(g_global,(prefix+"Path").c_str(),share.path.c_str());
    storeSet(g_global,(prefix+"User").c_str(),share.user.c_str());
    storeSet(g_global,(prefix+"Password").c_str(),share.password.c_str());
    storeSet(g_global,(prefix+"Domain").c_str(),share.domain.c_str());
    storeSet(g_global,(prefix+"AutoMount").c_str(),share.autoMount?"true":"false");
  }
  storeSave(g_global,LAUNCHER_INI);
}

static bool isJunkToken(const std::string &tok) {
  std::string l;
  for (char c : tok) l += (char)tolower((unsigned char)c);
  static const char *junk[] = {
    "pal","ntsc","ntsc-u","ntsc-j","ntscu","ntscj","usa","us","europe","eu","japan","jp","jpn",
    "world","korea","asia","multi","multi3","multi5","nkit","redump","proper","unl","disc","cd","dvd",
    "iso","chd","cso","zso","enfrespt",
  };
  for (auto j : junk) if (l == j) return true;
  if (l.size() >= 2 && l[0] == 'v' && isdigit((unsigned char)l[1])) return true;
  return false;
}
static std::string cleanTitle(const std::string &file) {
  std::string s = file;
  size_t dot = s.find_last_of('.');
  if (dot != std::string::npos) s = s.substr(0, dot);
  std::string o; int depth = 0;
  for (char c : s) {
    if (c == '(' || c == '[' || c == '{') depth++;
    else if (c == ')' || c == ']' || c == '}') { if (depth) depth--; }
    else if (!depth) o += (c == '_') ? ' ' : c;
  }
  std::string w; bool sp = true;
  for (char c : o) { if (isspace((unsigned char)c)) { if (!sp) w += ' '; sp = true; } else { w += c; sp = false; } }
  o = trim(w);
  std::string filtered;
  for(size_t start=0;start<o.size();){
    size_t end=o.find(' ',start);
    std::string token=o.substr(start,end==std::string::npos?std::string::npos:end-start);
    if(foldedKey(token)!="enfrespt"){
      if(!filtered.empty()) filtered+=' ';
      filtered+=token;
    }
    if(end==std::string::npos) break;
    start=end+1;
  }
  o=std::move(filtered);
  for (;;) {
    size_t p = o.find_last_of(" -");
    std::string last = (p == std::string::npos) ? o : o.substr(p + 1);
    if (!last.empty() && isJunkToken(last) && p != std::string::npos) {
      o = trim(o.substr(0, p));
      while (!o.empty() && (o.back() == '-' || o.back() == ' ' || o.back() == '.')) o.pop_back();
    } else break;
  }
  return trim(o);
}
static std::string sanitize(const std::string &file) {
  std::string s = file;
  size_t dot = s.find_last_of('.');
  if (dot != std::string::npos) s = s.substr(0, dot);
  std::string o;
  for (char c : s) o += (isalnum((unsigned char)c) || c=='-'||c=='_') ? c : '_';
  return o;
}

static std::string foldedKey(std::string key) {
  std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return (char)std::tolower(c); });
  return key;
}

static std::string makeGameKey(const std::string &file, const std::string &path) {
  std::string base = sanitize(file);
  if (base.empty()) base = "game";
  if (base.size() > 80) base.resize(80);

  uint64_t hash = 1469598103934665603ULL;
  for (unsigned char c : cemu_normalizeTitlePath(path)) {
    hash ^= c;
    hash *= 1099511628211ULL;
  }
  char suffix[24];
  snprintf(suffix, sizeof(suffix), "-%016llx", (unsigned long long)hash);
  return base + suffix;
}

static uint64_t fingerprintGameFile(const std::string &path, const struct stat &st) {
  uint64_t hash=1469598103934665603ULL;
  auto feed=[&](const void *data,size_t size){
    const unsigned char *bytes=static_cast<const unsigned char*>(data);
    for(size_t i=0;i<size;i++){ hash^=bytes[i]; hash*=1099511628211ULL; }
  };
  const uint64_t size=static_cast<uint64_t>(st.st_size);
  feed(&size,sizeof(size));
  FILE *file=fopen(path.c_str(),"rb");
  if(!file) return hash;
  std::array<unsigned char,65536> buffer{};
  size_t read=fread(buffer.data(),1,buffer.size(),file);
  feed(buffer.data(),read);
  if(st.st_size>static_cast<off_t>(buffer.size()) &&
     fseeko(file,st.st_size-static_cast<off_t>(buffer.size()),SEEK_SET)==0){
    read=fread(buffer.data(),1,buffer.size(),file);
    feed(buffer.data(),read);
  }
  fclose(file);
  return hash;
}

static uint64_t fingerprintGameDirectory(const std::string &path,uint64_t titleId) {
  uint64_t hash=1469598103934665603ULL;
  auto feed=[&](const void *data,size_t size){
    const auto *bytes=static_cast<const unsigned char*>(data);
    for(size_t index=0;index<size;index++){hash^=bytes[index];hash*=1099511628211ULL;}
  };
  // Title metadata identifies an unpacked game independently of its folder
  // name. Sample the RPX header as a fallback for malformed/no-ID homebrew.
  feed(&titleId,sizeof(titleId));
  const std::string codePath=join(path,"code");
  DIR *directory=opendir(codePath.c_str());
  if(directory){
    std::vector<std::string> candidates;dirent *entry=nullptr;
    while((entry=readdir(directory))){
      const std::string name=entry->d_name;
      const size_t dot=name.find_last_of('.');
      if(dot!=std::string::npos&&foldedKey(name.substr(dot))==".rpx")candidates.push_back(name);
    }
    closedir(directory);std::sort(candidates.begin(),candidates.end());
    if(!candidates.empty()){
      const std::string file=join(codePath,candidates.front());struct stat info{};
      if(stat(file.c_str(),&info)==0&&S_ISREG(info.st_mode)){
        const uint64_t size=(uint64_t)info.st_size;feed(&size,sizeof(size));
        FILE *input=fopen(file.c_str(),"rb");if(input){std::array<unsigned char,65536> bytes{};const size_t count=fread(bytes.data(),1,bytes.size(),input);feed(bytes.data(),count);fclose(input);}
      }
    }
  }
  return hash;
}

static std::string stableGameKey(uint64_t titleId,const std::string &path,const struct stat &st,
                                 bool isDirectory,uint64_t knownFingerprint=0) {
  char key[64]{};
  if(titleId&&isDirectory){
    snprintf(key,sizeof(key),"wiiu-dir-%016llx",(unsigned long long)titleId);
    return key;
  }
  uint64_t fingerprint=knownFingerprint;
  if(!fingerprint&&!isDirectory) fingerprint=fingerprintGameFile(path,st);
  else if(isDirectory) fingerprint=fingerprintGameDirectory(path,titleId);
  if(titleId) snprintf(key,sizeof(key),"wiiu-%016llx-%016llx",(unsigned long long)titleId,
                       (unsigned long long)fingerprint);
  else snprintf(key,sizeof(key),"content-%016llx",(unsigned long long)fingerprint);
  return key;
}

struct GameIdentityRecord {
  std::string key,format,canonicalPath,currentPath;
  std::vector<std::string> previousPaths;
  uint64_t titleId=0,fingerprint=0;
  bool retired=false;
};

static constexpr size_t MAX_PREVIOUS_LIBRARY_PATHS=6;

static std::string identityFormat(const std::string &path,bool directory) {
  if(directory) return "dir";
  const size_t dot=path.find_last_of('.');
  return foldedKey(dot==std::string::npos?std::string{}:path.substr(dot+1));
}

static std::string identityScope(const std::string &canonical) {
  if(canonical.rfind("usb:",0)==0||canonical.rfind("smb:",0)==0){
    const size_t slash=canonical.find('/',4);
    return slash==std::string::npos?canonical:canonical.substr(0,slash);
  }
  const size_t colon=canonical.find(':');
  return colon==std::string::npos?std::string{}:canonical.substr(0,colon+1);
}

static std::string canonicalGamePath(const std::string &input) {
  const std::string path=normalizeLocationPath(input);
  for(const auto &location:SwitchStorage::ListUsbLocations()){
    const std::string root=normalizeLocationPath(location.path);
    if(!pathAtOrBelow(path,root)) continue;
    std::string relative=path.substr(std::min(path.size(),root.size()));
    while(!relative.empty()&&relative.front()=='/') relative.erase(relative.begin());
    return "usb:"+foldedKey(location.id)+"/"+foldedKey(relative);
  }
  // SMB devoptab names are deterministically derived from the configured
  // share ID, so the normalized path is already independent of server aliases.
  return pathIdentity(path);
}

static bool identityPathExists(const GameIdentityRecord &record) {
  if(record.retired||record.currentPath.empty())return false;
  struct stat info{};
  if(stat(record.currentPath.c_str(),&info)!=0||(!S_ISREG(info.st_mode)&&!S_ISDIR(info.st_mode)))return false;
  // A recycled umsN: alias must never make an old record look live for a
  // different disk.  Re-resolve the stable USB identity before reserving it.
  return canonicalGamePath(record.currentPath)==record.canonicalPath;
}

static size_t canonicalRenameScore(const std::string &left,const std::string &right) {
  const size_t leftSlash=left.find_last_of('/'),rightSlash=right.find_last_of('/');
  if(leftSlash!=std::string::npos&&rightSlash!=std::string::npos&&
     left.substr(0,leftSlash)==right.substr(0,rightSlash))
    return (size_t{1}<<30)+leftSlash;
  size_t common=0,lastBoundary=0;
  while(common<left.size()&&common<right.size()&&left[common]==right[common]){
    if(left[common]=='/')lastBoundary=common+1;
    ++common;
  }
  return lastBoundary;
}

static void rememberPreviousPath(GameIdentityRecord &record,const std::string &path) {
  const std::string normalized=normalizeLocationPath(path);
  if(normalized.empty()) return;
  record.previousPaths.erase(std::remove_if(record.previousPaths.begin(),record.previousPaths.end(),
    [&](const std::string &entry){return pathIdentity(entry)==pathIdentity(normalized);}),record.previousPaths.end());
  record.previousPaths.push_back(normalized);
  if(record.previousPaths.size()>MAX_PREVIOUS_LIBRARY_PATHS)
    record.previousPaths.erase(record.previousPaths.begin(),
                               record.previousPaths.end()-MAX_PREVIOUS_LIBRARY_PATHS);
}

static std::string encodeIdentityField(const std::string &value) {
  static const char hex[]="0123456789ABCDEF";
  std::string encoded;encoded.reserve(value.size()*2);
  for(unsigned char byte:value){encoded.push_back(hex[byte>>4]);encoded.push_back(hex[byte&15]);}
  return encoded;
}

static bool decodeIdentityField(const std::string &value,std::string &decoded) {
  if(value.size()%2) return false;
  auto digit=[](unsigned char c)->int{
    if(c>='0'&&c<='9')return c-'0';
    c=(unsigned char)std::tolower(c);return c>='a'&&c<='f'?c-'a'+10:-1;
  };
  decoded.clear();decoded.reserve(value.size()/2);
  for(size_t index=0;index<value.size();index+=2){
    const int high=digit(value[index]),low=digit(value[index+1]);
    if(high<0||low<0){decoded.clear();return false;}
    decoded.push_back((char)((high<<4)|low));
  }
  return true;
}

static bool parseIdentityRecord(const KV &entry,GameIdentityRecord &record) {
  record={};record.key=entry.k;
  if(entry.v.rfind("v2,",0)==0){
    std::vector<std::string> fields;size_t start=3;
    for(;;){const size_t comma=entry.v.find(',',start);fields.push_back(entry.v.substr(start,comma==std::string::npos?std::string::npos:comma-start));if(comma==std::string::npos)break;start=comma+1;}
    if(fields.size()<7)return false;
    char *end=nullptr;errno=0;record.titleId=strtoull(fields[0].c_str(),&end,16);if(errno||!end||*end)return false;
    record.format=fields[1];errno=0;record.fingerprint=strtoull(fields[2].c_str(),&end,16);if(errno||!end||*end)return false;
    if(!decodeIdentityField(fields[3],record.canonicalPath)||!decodeIdentityField(fields[4],record.currentPath))return false;
    record.retired=fields[5]=="1";errno=0;const unsigned long count=strtoul(fields[6].c_str(),&end,10);if(errno||!end||*end||count>MAX_PREVIOUS_LIBRARY_PATHS||fields.size()!=7+count)return false;
    for(size_t index=0;index<count;index++){std::string path;if(!decodeIdentityField(fields[7+index],path))return false;record.previousPaths.push_back(std::move(path));}
    return !record.key.empty()&&!record.format.empty()&&!record.canonicalPath.empty();
  }
  // Read the 1.0.x hash-only format so existing per-game data keeps its ID.
  // It is upgraded to v2 as soon as a compatible title is observed.
  char format[20]{};unsigned long long title=0,fingerprint=0,scope=0,locator=0;int used=0;
  if(sscanf(entry.v.c_str(),"%llx,%19[^,],%llx,%llx,%llx%n",&title,format,&fingerprint,&scope,&locator,&used)!=5||entry.v[used])return false;
  record.format=format;record.titleId=(uint64_t)title;record.fingerprint=(uint64_t)fingerprint;
  return !record.key.empty();
}

static void saveIdentityRecord(Store &store,const GameIdentityRecord &record) {
  char header[128];snprintf(header,sizeof(header),"v2,%016llx,%s,%016llx,",
    (unsigned long long)record.titleId,record.format.c_str(),(unsigned long long)record.fingerprint);
  std::string value=header+encodeIdentityField(record.canonicalPath)+","+
    encodeIdentityField(record.currentPath)+","+(record.retired?"1":"0")+","+
    std::to_string(record.previousPaths.size());
  for(const std::string &path:record.previousPaths)value+=","+encodeIdentityField(path);
  storeSet(store,record.key.c_str(),value.c_str());
}

static std::string choosePersistentGameKey(const Store &registry,Store &refreshed,
                                           std::unordered_set<std::string> &used,
                                           std::unordered_set<std::string> &reservedIds,
                                           const std::unordered_set<std::string> &reservedCanonicalPaths,
                                           uint64_t titleId,uint64_t fingerprint,
                                           const std::string &format,const std::string &canonical,
                                           const std::string &currentPath) {
  std::vector<GameIdentityRecord> records;
  for(const KV &entry:registry.kv){ GameIdentityRecord record; if(parseIdentityRecord(entry,record)) records.push_back(std::move(record)); }
  auto compatible=[&](const GameIdentityRecord &record){
    return record.format==format&&record.titleId==titleId;
  };
  GameIdentityRecord *chosen=nullptr;
  for(auto &record:records)if(!record.retired&&!used.count(record.key)&&
    record.canonicalPath==canonical&&compatible(record)){chosen=&record;break;}
  for(auto &record:records)if(!record.retired&&record.canonicalPath==canonical&&!compatible(record)){
    rememberPreviousPath(record,record.currentPath);record.currentPath.clear();record.retired=true;saveIdentityRecord(refreshed,record);
    reservedIds.erase(record.key);
  }
  const std::string scope=identityScope(canonical);
  if(!chosen){
    GameIdentityRecord *best=nullptr;size_t bestScore=0;bool tied=false;
    for(auto &record:records){
      if(record.retired||used.count(record.key)||!compatible(record)||record.fingerprint!=fingerprint||
         scope.empty()||identityScope(record.canonicalPath)!=scope)continue;
      if(reservedIds.count(record.key)&&!record.canonicalPath.empty()&&
         reservedCanonicalPaths.count(record.canonicalPath))continue;
      const size_t score=canonicalRenameScore(record.canonicalPath,canonical);
      if(!best||score>bestScore){best=&record;bestScore=score;tied=false;}
      else if(score==bestScore)tied=true;
    }
    // If two vanished identical copies are indistinguishable, keep both old
    // records reserved rather than silently attaching the wrong settings.
    if(best&&!tied){chosen=best;reservedIds.erase(best->key);}
  }
  // Legacy hash-only records have no locator. Match only an unambiguous
  // compatible record, preserving its historical key without guessing between
  // identical copies.
  if(!chosen){
    std::vector<GameIdentityRecord*> legacy;
    for(auto &record:records)if(!record.retired&&!used.count(record.key)&&record.canonicalPath.empty()&&
      compatible(record)&&(record.fingerprint==fingerprint||(format=="dir"&&titleId!=0)))legacy.push_back(&record);
    if(legacy.size()==1)chosen=legacy.front();
  }
  GameIdentityRecord current;
  if(chosen) current=*chosen;
  else {
    struct stat dummy{}; dummy.st_size=0;
    current.key=stableGameKey(titleId,"",dummy,false,fingerprint);
    const std::string base=current.key;
    auto registryHas=[&](const std::string &key){
      return std::any_of(registry.kv.begin(),registry.kv.end(),[&](const KV &entry){return entry.k==key;});
    };
    for(unsigned suffix=2;used.count(current.key)||registryHas(current.key);suffix++)
      current.key=base+"-"+std::to_string(suffix);
  }
  const std::string normalized=normalizeLocationPath(currentPath);
  if(!current.currentPath.empty()&&pathIdentity(current.currentPath)!=pathIdentity(normalized))
    rememberPreviousPath(current,current.currentPath);
  current.titleId=titleId;current.fingerprint=fingerprint;current.format=format;
  current.canonicalPath=canonical;current.currentPath=normalized;current.retired=false;
  used.insert(current.key); saveIdentityRecord(refreshed,current);
  reservedIds.erase(current.key);
  return current.key;
}

static void migrateIdentityFile(const std::string &directory,const std::string &oldKey,
                                const std::string &newKey,const char *extension) {
  if(oldKey.empty()||oldKey==newKey) return;
  const std::string oldPath=directory+"/"+oldKey+extension;
  const std::string newPath=directory+"/"+newKey+extension;
  struct stat oldStat{},newStat{};
  if(stat(oldPath.c_str(),&oldStat)!=0||stat(newPath.c_str(),&newStat)==0) return;
  (void)rename(oldPath.c_str(),newPath.c_str());
}

static void migrateGameIdentity(Game &game) {
  if(game.legacyKey.empty()||game.legacyKey==game.key) return;
  if(!storeGet(g_titles,game.key.c_str(),"")[0]){
    const char *legacy=storeGet(g_titles,game.legacyKey.c_str(),"");
    if(legacy[0]) storeSet(g_titles,game.key.c_str(),legacy);
  }
  if(!storeGet(g_recent,game.key.c_str(),"")[0]){
    const char *legacy=storeGet(g_recent,game.legacyKey.c_str(),"");
    if(legacy[0]) storeSet(g_recent,game.key.c_str(),legacy);
  }
  migrateIdentityFile(COVERS_DIR,game.legacyKey,game.key,".png");
  migrateIdentityFile(COVERS_DIR,game.legacyKey,game.key,".background.png");
  migrateIdentityFile(GAMECFG_DIR,game.legacyKey,game.key,".ini");
}


static bool gameFileExists(const char *dir, const Game &game, const char *extension) {
  return regularFileExists(std::string(dir) + "/" + game.key + extension);
}

static std::string fileNameOf(const std::string &path);

static bool nameIsBareTitleId(const std::string &name) {
  if(name.size()!=16) return false;
  for(unsigned char c:name) if(!isxdigit(c)) return false;
  return true;
}

static bool directoryIsTitle(const std::string &path) {
  struct stat info{};
  if(stat((path+"/code").c_str(),&info)==0) return true;
  const uint64_t nusTitleId=cemu_readNusTitleId(path);
  return nusTitleId!=0&&(uint32_t)(nusTitleId>>32)==0x00050000;
}

struct LibraryScanState {
  std::mutex mutex;
  std::deque<Game> ready;
  Store refreshedContainerTitles;
  Store refreshedIdentities;
  Store publishedIdentities;
  std::vector<std::string> completedSources;
  std::unordered_set<std::string> foundPaths;
  std::atomic<bool> cancel{false};
  std::atomic<bool> done{false};
  bool replace=true;
  bool cleared=false;
  size_t unsortedPublished=0;
  std::thread worker;
};
static std::shared_ptr<LibraryScanState> g_libraryScan;
static bool g_libraryStoragePending=false;
static bool g_libraryEmptyShown=false;

static void wakeUiFromWorker(int code) {
  if(!g_sdlReady) return;
  SDL_Event event{}; event.type=SDL_USEREVENT; event.user.code=code; SDL_PushEvent(&event);
}
static void usbStatusWake(void*) { wakeUiFromWorker(0x55534248); }

static void libraryScanWorker(const std::shared_ptr<LibraryScanState> &state,
                              std::vector<std::string> sources,Store titles,Store recent,
                              Store containerTitles,Store identities) {
  auto titleCache=cemu_loadTitleCache(std::string(DATA_DIR)+"/title_list_cache.xml");
  std::unordered_set<std::string> seenPaths;
  std::unordered_set<std::string> usedIdentities;
  std::unordered_set<std::string> reservedIds;
  std::unordered_set<std::string> reservedCanonicalPaths;
  state->refreshedIdentities=identities; // Offline/unmounted scopes remain reserved.
  for(const KV &entry:identities.kv){
    GameIdentityRecord record;
    if(!parseIdentityRecord(entry,record)||record.retired)continue;
    reservedIds.insert(record.key);
    if(!record.canonicalPath.empty())
      reservedCanonicalPaths.insert(record.canonicalPath);
  }
  auto publish=[&](Game game){
    if(state->cancel.load()) return;
    bool wake=false;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      const char *identity=storeGet(state->refreshedIdentities,game.key.c_str(),"");
      if(identity[0]) storeSet(state->publishedIdentities,game.key.c_str(),identity);
      state->ready.emplace_back(std::move(game));
      wake=state->ready.size()==1||state->ready.size()%8==0;
    }
    if(wake) wakeUiFromWorker(0x5343414e);
  };
  for(const std::string &source:sources){
    if(state->cancel.load()) break;
    if(isUnresolvedUsbSource(source)) continue;
    const std::string sourceStorageId=usbStableIdForPath(source);
    DIR *directory=opendir(source.c_str());
    if(!directory) continue;
    std::vector<std::string> names;
    struct dirent *entry=nullptr;
    while(!state->cancel.load()&&(entry=readdir(directory))) if(entry->d_name[0]!='.') names.emplace_back(entry->d_name);
    closedir(directory);
    state->completedSources.push_back(normalizeLocationPath(source));
    // display name, path
    std::vector<std::pair<std::string,std::string>> candidates;
    if(directoryIsTitle(source)) candidates.emplace_back(fileNameOf(source),source);
    for(const std::string &name:names) candidates.emplace_back(name,join(source,name));
    const std::string canonicalRoot=pathIdentity(canonicalGamePath(source));
    std::unordered_set<std::string> listedPaths;
    for(const auto &candidate:candidates) listedPaths.insert(canonicalGamePath(candidate.second));
    // A completed directory listing proves which old names disappeared without
    // probing saved ums aliases while their devices are still mounting.
    for(const KV &entry:identities.kv){
      GameIdentityRecord record;
      if(!parseIdentityRecord(entry,record)||record.retired) continue;
      const size_t slash=record.canonicalPath.find_last_of('/');
      if(slash!=std::string::npos&&record.canonicalPath.substr(0,slash)==canonicalRoot&&
         !listedPaths.count(record.canonicalPath)) reservedCanonicalPaths.erase(record.canonicalPath);
    }
    for(const auto &candidate:candidates){
      if(state->cancel.load()) break;
      const std::string &name=candidate.first;
      const std::string &full=candidate.second;
      struct stat info{};
      const bool exists=SwitchStorage::GetCachedSmbStat(full,&info)||stat(full.c_str(),&info)==0;
      const bool isDirectory=exists&&S_ISDIR(info.st_mode);
      const bool isGameDirectory=isDirectory&&directoryIsTitle(full);
      if(!isGameDirectory&&(isDirectory||!hasGameExt(name.c_str()))) continue;
      if(!seenPaths.insert(pathIdentity(full)).second) continue;
      Game game;
      game.file=name;game.path=full;game.storageId=sourceStorageId;
      game.legacyKey=makeGameKey(game.file,full); game.key=game.legacyKey;
      if(exists){ game.added=(long long)info.st_mtime; game.modified=game.added; game.fileSize=(long long)info.st_size; }
      if(!isDirectory&&exists){
        const char *cached=storeGet(containerTitles,game.legacyKey.c_str(),"");
        long long size=0,mtime=0; unsigned long long id=0,fingerprint=0; int consumed=0;
        const int parsed=sscanf(cached,"%lld,%lld,%llx,%llx%n",&size,&mtime,&id,&fingerprint,&consumed);
        if(parsed>=3&&cached[consumed]==0&&size==(long long)info.st_size&&mtime==(long long)info.st_mtime&&(id>>48)==0x0005){
          game.titleId=(uint64_t)id;
          if(parsed==4) game.fingerprint=(uint64_t)fingerprint;
        }
      }
      if(!game.titleId) game.titleId=cemu_resolveBaseTitleId(full,titleCache,&game.titleIdError);
      if(!isDirectory&&!game.fingerprint) game.fingerprint=fingerprintGameFile(full,info);
      if(isDirectory&&!game.fingerprint) game.fingerprint=fingerprintGameDirectory(full,game.titleId);
      const std::string canonical=canonicalGamePath(full);
      game.key=choosePersistentGameKey(state->refreshedIdentities,state->refreshedIdentities,usedIdentities,
                                        reservedIds,reservedCanonicalPaths,
                                        game.titleId,game.fingerprint,
                                        identityFormat(full,isDirectory),canonical,full);
      state->foundPaths.insert(pathIdentity(full));
      const char *custom=storeGet(titles,game.key.c_str(),"");
      if(!custom[0]) custom=storeGet(titles,game.legacyKey.c_str(),"");
      game.title=custom[0]?custom:cleanTitle(game.file);
      if(!custom[0]&&nameIsBareTitleId(game.file)){
        std::string cached=cemu_titleNameFromCache(titleCache,game.titleId);
        if(!cached.empty()) game.title=std::move(cached);
      }
      game.region=detectRegion(game.file);
      const char *played=storeGet(recent,game.key.c_str(),"");
      if(!played[0]) played=storeGet(recent,game.legacyKey.c_str(),"0");
      game.played=atoll(played);
      game.hasCfg=gameFileExists(GAMECFG_DIR,game,".ini") ||
                  regularFileExists(std::string(GAMECFG_DIR)+"/"+game.legacyKey+".ini");
      if(!isDirectory&&exists&&game.titleId){
        char cached[128]; snprintf(cached,sizeof(cached),"%lld,%lld,%016llx,%016llx",game.fileSize,game.modified,
                                  (unsigned long long)game.titleId,(unsigned long long)game.fingerprint);
        storeSet(state->refreshedContainerTitles,game.key.c_str(),cached);
        storeSet(state->refreshedContainerTitles,game.legacyKey.c_str(),cached);
      }
      publish(std::move(game));
    }
  }
  if(!state->cancel.load()) for(auto &title:cemu_scanMlcTitles(std::string(DATA_DIR)+"/mlc01")){
    if(state->cancel.load()) break;
    Game game; game.titleId=title.titleId; game.iconPath=title.iconPath;
    char id[40]; snprintf(id,sizeof(id),"wiiu-installed-%016llx",(unsigned long long)title.titleId);
    game.key=id; game.file=id;
    const char *custom=storeGet(titles,game.key.c_str(),"");
    game.title=custom[0]?custom:(title.name.empty()?game.key:title.name);
    game.played=atoll(storeGet(recent,game.key.c_str(),"0"));
    struct stat profile{};
    game.hasCfg=stat((std::string(GAMEPROFILES_DIR)+"/"+game.key+".ini").c_str(),&profile)==0;
    publish(std::move(game));
  }
  state->done=true;
  wakeUiFromWorker(0x5343414e);
}

static void stopGameScan() {
  const auto state=g_libraryScan;
  if(!state) return;
  state->cancel=true;
  if(state->worker.joinable()) state->worker.join();
  g_libraryScan.reset();
}

static void startGameScan(std::vector<std::string> sources,bool replace=true) {
  stopGameScan();
  if(replace)cancelQueuedCoverDecodes();
  // Publish the SD/local first page before slower removable/network roots.
  // stable_sort preserves the user's ordering within each storage class.
  std::stable_sort(sources.begin(),sources.end(),[](const std::string &left,const std::string &right){
    auto rank=[](const std::string &path){
      if(path.rfind("cemusmb_",0)==0) return 2;
      if(path.size()>=5&&path[0]=='u'&&path[1]=='m'&&path[2]=='s'&&
         isdigit((unsigned char)path[3])&&path[4]==':') return 1;
      return 0;
    };
    return rank(left)<rank(right);
  });
  auto state=std::make_shared<LibraryScanState>();
  state->replace=replace;
  state->worker=std::thread(libraryScanWorker,state,std::move(sources),g_titles,g_recent,
                            g_containerTitles,g_gameIdentities);
  g_libraryScan=std::move(state);
}

static bool pumpGameScan() {
  const auto state=g_libraryScan;
  if(!state) return false;
  std::vector<Game> batch;
  Store publishedIdentities;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    // Stable-identity migration and file checks still happen on the SDL
    // thread. Keep that work bounded so input/rendering never waits on a
    // large ready batch.
    const size_t count=std::min(size_t{2},state->ready.size());
    batch.reserve(count);
    for(size_t i=0;i<count;i++){
      batch.emplace_back(std::move(state->ready.front()));
      state->ready.pop_front();
    }
    publishedIdentities.kv.swap(state->publishedIdentities.kv);
  }
  if(!publishedIdentities.kv.empty()){
    // Persist identities at the same time games become visible. A user can
    // immediately launch or forward a progressive result, so cancellation or
    // teardown must not discard the key that was already exposed.
    for(const KV &entry:publishedIdentities.kv)
      storeSet(g_gameIdentities,entry.k.c_str(),entry.v.c_str());
    storeSave(g_gameIdentities,GAME_IDENTITIES_INI);
  }
  if(!batch.empty()&&state->replace&&!state->cleared){
    for(Game &game:g_games) releaseGameArtwork(game);
    g_games.clear(); state->cleared=true;
  }
  for(Game &game:batch){
    migrateGameIdentity(game);
    auto existing=std::find_if(g_games.begin(),g_games.end(),[&](const Game &candidate){return candidate.key==game.key;});
    if(existing==g_games.end()) g_games.emplace_back(std::move(game));
    else {
      const bool metadataUnchanged=existing->fingerprint==game.fingerprint;
      if(metadataUnchanged){
        game.cover=existing->cover;game.triedCover=existing->triedCover;
        game.coverQueued=existing->coverQueued;game.coverRequest=existing->coverRequest;
        game.coverAt=existing->coverAt;game.coverUse=existing->coverUse;
        game.background=existing->background;game.triedBackground=existing->triedBackground;
        game.backgroundQueued=existing->backgroundQueued;game.backgroundRequest=existing->backgroundRequest;
        game.backgroundUse=existing->backgroundUse;
      }else releaseGameArtwork(*existing);
      *existing=std::move(game);
    }
  }
  if(!batch.empty()){
    state->unsortedPublished+=batch.size();
    // Keep the first maximum-sized page ordered, then amortize sorting as the
    // library grows instead of sorting the whole vector for every tiny batch.
    if(g_games.size()<=24||state->unsortedPublished>=16){
      applySort();state->unsortedPublished=0;
    }else rebuildVisibleGames();
  }
  bool readyEmpty=false;
  { std::lock_guard<std::mutex> lock(state->mutex); readyEmpty=state->ready.empty(); }
  if(state->done.load()&&readyEmpty){
    if(state->worker.joinable()) state->worker.join();
    if(state->replace&&!state->cleared){
      for(Game &game:g_games) releaseGameArtwork(game);
      g_games.clear(); state->cleared=true;
      rebuildVisibleGames();
    } else if(!state->replace){
      for(auto iterator=g_games.begin();iterator!=g_games.end();){
        const bool targeted=std::any_of(state->completedSources.begin(),state->completedSources.end(),
          [&](const std::string &source){return pathAtOrBelow(iterator->path,source);});
        if(targeted&&!state->foundPaths.count(pathIdentity(iterator->path))){
          releaseGameArtwork(*iterator);
          iterator=g_games.erase(iterator);
        } else ++iterator;
      }
      applySort();
    }
    if(state->replace) g_containerTitles=std::move(state->refreshedContainerTitles);
    else for(const KV &entry:state->refreshedContainerTitles.kv)
      storeSet(g_containerTitles,entry.k.c_str(),entry.v.c_str());
    storeSave(g_containerTitles,CONTAINER_TITLES_INI);
    if(state->replace) g_gameIdentities=std::move(state->refreshedIdentities);
    else for(const KV &entry:state->refreshedIdentities.kv)
      storeSet(g_gameIdentities,entry.k.c_str(),entry.v.c_str());
    storeSave(g_gameIdentities,GAME_IDENTITIES_INI);
    storeSave(g_titles,TITLES_INI); storeSave(g_recent,RECENT_INI);
    applySort();
    g_libraryScan.reset();
  }
  return !batch.empty();
}

static void removeGamesFromUsbDevices(const std::unordered_set<std::string> &deviceIds) {
  if(deviceIds.empty()) return;
  for(auto iterator=g_games.begin();iterator!=g_games.end();){
    if(!iterator->storageId.empty()&&deviceIds.count(iterator->storageId)){
      releaseGameArtwork(*iterator);
      iterator=g_games.erase(iterator);
    } else ++iterator;
  }
  applySort();
}
static std::string coverPath(const Game &g) { return std::string(COVERS_DIR) + "/" + g.key + ".png"; }
static std::string backgroundPath(const Game &game){
  return std::string(COVERS_DIR)+"/"+game.key+".background.png";
}

static std::string artworkKey(const Game &game,const char *field){
  return "Artwork/"+game.key+"/"+field;
}

static std::string existingCoverPath(const Game &g) {
  return coverPath(g);
}

static Game *findGameByKey(const std::string &key) {
  if(key.empty()) return nullptr;
  const std::string folded=foldedKey(key);
  for (auto &game : g_games)
    if (foldedKey(game.key)==folded||foldedKey(game.legacyKey)==folded) return &game;
  return nullptr;
}

static constexpr int COVER_REQUEST_BUDGET = 48;
static constexpr int COVER_UPLOAD_BUDGET = 2;
static constexpr size_t COVER_JOB_LIMIT = 96;
static constexpr size_t COVER_READY_LIMIT = 4;
static int g_cover_budget = 1 << 30;

struct CoverDecodeJob {
  std::string key;
  bool background=false;
  std::vector<std::string> paths;
  Uint64 request=0;
  Uint64 epoch=0;
};
struct CoverDecodeResult {
  std::string key;
  bool background=false;
  Uint64 request=0;
  Uint64 epoch=0;
  int width=0,height=0;
  std::vector<Uint8> pixels;
};
static std::mutex g_coverDecodeMutex;
static std::condition_variable g_coverDecodeCondition;
static std::deque<CoverDecodeJob> g_coverDecodeJobs;
static std::deque<CoverDecodeResult> g_coverDecodeReady;
static std::thread g_coverDecodeWorker;
static bool g_coverDecodeStarted=false,g_coverDecodeStop=false;
static Uint64 g_coverDecodeEpoch=1,g_coverRequestSerial=0;

static std::vector<std::string> coverCandidatePaths(const Game &game){
  std::vector<std::string> paths{coverPath(game)};
  if(!game.iconPath.empty()&&game.iconPath!=paths.front())paths.emplace_back(game.iconPath);
  return paths;
}

static CoverDecodeResult decodeCover(const CoverDecodeJob &job) {
  CoverDecodeResult result;result.key=job.key;result.background=job.background;result.request=job.request;result.epoch=job.epoch;
  SDL_Surface *source=nullptr;
  for(const std::string &path:job.paths){source=IMG_Load(path.c_str());if(source)break;}
  if(!source||source->w<1||source->h<1||source->w>8192||source->h>8192||
     (Uint64)source->w*(Uint64)source->h>16ull*1024*1024){
    if(source)SDL_FreeSurface(source);
    return result;
  }
  const int maxWidth=job.background?1280:360,maxHeight=job.background?720:540;
  int width=source->w,height=source->h;
  if(width>maxWidth){height=(int)((long long)height*maxWidth/width);width=maxWidth;}
  if(height>maxHeight){width=(int)((long long)width*maxHeight/height);height=maxHeight;}
  width=std::max(1,width);height=std::max(1,height);
  SDL_Surface *rgba=SDL_CreateRGBSurfaceWithFormat(0,width,height,32,SDL_PIXELFORMAT_RGBA32);
  if(!rgba){SDL_FreeSurface(source);return result;}
  SDL_BlendMode blend=SDL_BLENDMODE_NONE;SDL_GetSurfaceBlendMode(source,&blend);
  SDL_SetSurfaceBlendMode(source,SDL_BLENDMODE_NONE);
  const bool converted=SDL_BlitScaled(source,nullptr,rgba,nullptr)==0;
  SDL_SetSurfaceBlendMode(source,blend);SDL_FreeSurface(source);
  if(!converted){SDL_FreeSurface(rgba);return result;}
  const bool mustLock=SDL_MUSTLOCK(rgba);
  if(mustLock&&SDL_LockSurface(rgba)!=0){SDL_FreeSurface(rgba);return result;}
  result.pixels.resize((size_t)width*(size_t)height*4);
  for(int row=0;row<height;row++)memcpy(
      result.pixels.data()+(size_t)row*(size_t)width*4,
      (const Uint8*)rgba->pixels+(size_t)row*(size_t)rgba->pitch,(size_t)width*4);
  if(mustLock)SDL_UnlockSurface(rgba);
  SDL_FreeSurface(rgba);result.width=width;result.height=height;
  return result;
}

static void coverDecodeThread(){
  for(;;){
    CoverDecodeJob job;
    {
      std::unique_lock<std::mutex> lock(g_coverDecodeMutex);
      g_coverDecodeCondition.wait(lock,[]{return g_coverDecodeStop||
          (!g_coverDecodeJobs.empty()&&g_coverDecodeReady.size()<COVER_READY_LIMIT);});
      if(g_coverDecodeStop)return;
      job=std::move(g_coverDecodeJobs.front());g_coverDecodeJobs.pop_front();
    }
    CoverDecodeResult result=decodeCover(job);bool publish=false;
    {
      std::lock_guard<std::mutex> lock(g_coverDecodeMutex);
      if(!g_coverDecodeStop&&job.epoch==g_coverDecodeEpoch){
        g_coverDecodeReady.emplace_back(std::move(result));publish=true;
      }
    }
    if(publish)wakeUiFromWorker(0x434f5652);
  }
}

static void startCoverDecodeWorker(){
  std::lock_guard<std::mutex> lock(g_coverDecodeMutex);
  if(g_coverDecodeStarted)return;
  g_coverDecodeStop=false;g_coverDecodeStarted=true;
  g_coverDecodeWorker=std::thread(coverDecodeThread);
}

static void stopCoverDecodeWorker(){
  {
    std::lock_guard<std::mutex> lock(g_coverDecodeMutex);
    if(!g_coverDecodeStarted)return;
    g_coverDecodeStop=true;g_coverDecodeJobs.clear();g_coverDecodeReady.clear();
  }
  g_coverDecodeCondition.notify_all();
  if(g_coverDecodeWorker.joinable())g_coverDecodeWorker.join();
  std::lock_guard<std::mutex> lock(g_coverDecodeMutex);g_coverDecodeStarted=false;
}

static void cancelQueuedCoverDecodes() {
  {
    std::lock_guard<std::mutex> lock(g_coverDecodeMutex);
    ++g_coverDecodeEpoch;g_coverDecodeJobs.clear();g_coverDecodeReady.clear();
  }
  for(Game &game:g_games){game.coverQueued=game.backgroundQueued=false;game.coverRequest=game.backgroundRequest=0;}
  g_coverDecodeCondition.notify_all();
}

static void queueCoverDecode(Game &game,bool priority,bool background=false) {
  bool &queued=background?game.backgroundQueued:game.coverQueued;
  bool &tried=background?game.triedBackground:game.triedCover;
  Uint64 &request=background?game.backgroundRequest:game.coverRequest;
  if((background?game.background:game.cover)||tried) return;
  if(queued){
    if(priority){
      std::lock_guard<std::mutex> lock(g_coverDecodeMutex);
      const auto found=std::find_if(g_coverDecodeJobs.begin(),g_coverDecodeJobs.end(),
          [&](const CoverDecodeJob &job){return job.request==request;});
      if(found!=g_coverDecodeJobs.end()&&found!=g_coverDecodeJobs.begin()){
        CoverDecodeJob job=std::move(*found);g_coverDecodeJobs.erase(found);
        g_coverDecodeJobs.emplace_front(std::move(job));g_coverDecodeCondition.notify_one();
      }
    }
    return;
  }
  if(g_cover_budget<=0)return;
  --g_cover_budget;
  CoverDecodeJob job;job.key=game.key;job.background=background;
  job.paths=background?std::vector<std::string>{backgroundPath(game)}:coverCandidatePaths(game);
  job.request=++g_coverRequestSerial;request=job.request;queued=true;
  CoverDecodeJob dropped;bool didDrop=false;
  {
    std::lock_guard<std::mutex> lock(g_coverDecodeMutex);job.epoch=g_coverDecodeEpoch;
    if(g_coverDecodeJobs.size()>=COVER_JOB_LIMIT){
      dropped=std::move(g_coverDecodeJobs.back());g_coverDecodeJobs.pop_back();didDrop=true;
    }
    if(priority)g_coverDecodeJobs.emplace_front(std::move(job));
    else g_coverDecodeJobs.emplace_back(std::move(job));
  }
  if(didDrop) if(Game *old=findGameByKey(dropped.key)){
    Uint64 &oldRequest=dropped.background?old->backgroundRequest:old->coverRequest;
    if(oldRequest==dropped.request){
      (dropped.background?old->backgroundQueued:old->coverQueued)=false;
      oldRequest=0;
    }
  }
  g_coverDecodeCondition.notify_one();
}

static void touchCover(Game &g) {
  if (g.cover) g.coverUse = ++g_coverUseSerial;
}

static void evictLeastRecentlyUsedCover() {
  Game *victim = nullptr;
  for (auto &candidate : g_games)
    if (candidate.cover && (!victim || candidate.coverUse < victim->coverUse)) victim = &candidate;
  if (!victim) return;
  SDL_DestroyTexture(victim->cover);
  victim->cover = nullptr;
  victim->coverUse = 0;
  victim->triedCover = false;
}

static void installCover(Game &g, SDL_Texture *cover) {
  if (!cover) return;
  size_t resident = 0;
  for (const auto &candidate : g_games) if (candidate.cover) resident++;
  if (resident >= COVER_CACHE_LIMIT) evictLeastRecentlyUsedCover();
  g.cover = cover;
  g.coverAt = SDL_GetTicks();
  touchCover(g);
}

static void installBackground(Game &game,SDL_Texture *texture){
  size_t resident=0;
  Game *victim=nullptr;
  for(Game &candidate:g_games){
    if(!candidate.background) continue;
    ++resident;
    if(candidate.key==g_backgroundCurrent||candidate.key==g_backgroundPrevious||candidate.key==g_backgroundTarget) continue;
    if(!victim||candidate.backgroundUse<victim->backgroundUse) victim=&candidate;
  }
  if(resident>=BACKGROUND_CACHE_LIMIT){
    if(!victim){ SDL_DestroyTexture(texture); game.triedBackground=false; return; }
    SDL_DestroyTexture(victim->background);victim->background=nullptr;
    victim->backgroundUse=0;victim->triedBackground=false;
  }
  game.background=texture;game.backgroundUse=++g_coverUseSerial;
}

static SDL_Texture *uploadCoverTexture(const CoverDecodeResult &result){
  if(result.width<1||result.height<1||result.pixels.empty()||!g_ren)return nullptr;
  SDL_Texture *texture=SDL_CreateTexture(g_ren,SDL_PIXELFORMAT_RGBA32,
      SDL_TEXTUREACCESS_STATIC,result.width,result.height);
  if(texture&&SDL_UpdateTexture(texture,nullptr,result.pixels.data(),result.width*4)!=0){
    SDL_DestroyTexture(texture);texture=nullptr;
  }
  if(!texture){
    SDL_Surface *surface=SDL_CreateRGBSurfaceWithFormatFrom(
        const_cast<Uint8*>(result.pixels.data()),result.width,result.height,
        32,result.width*4,SDL_PIXELFORMAT_RGBA32);
    if(surface){texture=SDL_CreateTextureFromSurface(g_ren,surface);SDL_FreeSurface(surface);}
  }
  if(texture)SDL_SetTextureBlendMode(texture,SDL_BLENDMODE_BLEND);
  return texture;
}

static void pumpCoverDecodeResults() {
  int uploads=0,processed=0;
  while(processed<12){
    CoverDecodeResult result;
    {
      std::lock_guard<std::mutex> lock(g_coverDecodeMutex);
      if(g_coverDecodeReady.empty())break;
      if(!g_coverDecodeReady.front().pixels.empty()&&uploads>=COVER_UPLOAD_BUDGET)break;
      result=std::move(g_coverDecodeReady.front());g_coverDecodeReady.pop_front();
    }
    g_coverDecodeCondition.notify_one();++processed;
    Game *game=findGameByKey(result.key);
    if(!game||result.epoch!=g_coverDecodeEpoch||
       (result.background?game->backgroundRequest:game->coverRequest)!=result.request)continue;
    (result.background?game->backgroundQueued:game->coverQueued)=false;
    (result.background?game->triedBackground:game->triedCover)=true;
    if(!result.pixels.empty()){
      SDL_Texture *texture=uploadCoverTexture(result);++uploads;
      if(texture){
        if(result.background) installBackground(*game,texture);
        else installCover(*game,texture);
      }
    }
  }
}

static void ensureCover(Game &g,bool priority=false) {
  if (g.cover) { touchCover(g); return; }
  queueCoverDecode(g,priority);
}
static void reloadCover(Game &g) {
  if (g.cover) { SDL_DestroyTexture(g.cover); g.cover = nullptr; }
  g.coverUse=0;g.triedCover=false;g.coverQueued=false;g.coverRequest=0;
  g_cover_budget=std::max(g_cover_budget,1);queueCoverDecode(g,true);
}

static void reloadBackground(Game &game){
  if(game.background) SDL_DestroyTexture(game.background);
  game.background=nullptr;game.backgroundUse=0;game.triedBackground=false;
  game.backgroundQueued=false;game.backgroundRequest=0;
}

static SDL_Rect fitArtworkRect(int width,int height,const SDL_Rect &bounds){
  if(width<1||height<1||bounds.w<1||bounds.h<1) return {bounds.x,bounds.y,0,0};
  int w=bounds.w,h=std::max(1,(int)((long long)height*w/width));
  if(h>bounds.h){ h=bounds.h;w=std::max(1,(int)((long long)width*h/height)); }
  return {bounds.x+(bounds.w-w)/2,bounds.y+(bounds.h-h)/2,w,h};
}

static void drawBackgroundArtwork(const std::string &key,Uint8 alpha){
  Game *game=findGameByKey(key);
  if(!game||!alpha) return;
  SDL_Texture *texture=game->background?game->background:game->triedBackground?game->cover:nullptr;
  if(!texture) return;
  if(game->background) game->backgroundUse=++g_coverUseSerial;
  else touchCover(*game);
  int width=0,height=0;SDL_QueryTexture(texture,nullptr,nullptr,&width,&height);
  if(width<1||height<1) return;
  SDL_Rect src{0,0,width,height},destination{0,0,SW,SH};
  if((long long)width*SH>(long long)height*SW){src.w=height*SW/SH;src.x=(width-src.w)/2;}
  else {src.h=width*SH/SW;src.y=(height-src.h)/2;}
  SDL_SetTextureColorMod(texture,205,205,205);SDL_SetTextureAlphaMod(texture,alpha);
  SDL_RenderCopy(g_ren,texture,&src,&destination);
  SDL_SetTextureColorMod(texture,255,255,255);SDL_SetTextureAlphaMod(texture,255);
}

static void drawClassicBackground(){
  Game *target=findGameByKey(g_backgroundTarget);
  bool ready=!target;
  if(target){
    if(target->background) target->backgroundUse=++g_coverUseSerial;
    else queueCoverDecode(*target,true,true);
    if(target->triedBackground&&!target->background) ensureCover(*target,true);
    ready=target->background||(target->triedBackground&&(target->cover||target->triedCover));
  }
  if(ready&&g_backgroundCurrent!=g_backgroundTarget){
    g_backgroundPrevious=g_backgroundCurrent;g_backgroundCurrent=g_backgroundTarget;
    g_backgroundSince=SDL_GetTicks();
  }
  const Uint32 elapsed=SDL_GetTicks()-g_backgroundSince;
  const Uint8 alpha=!g_uiAnimations||elapsed>=300?255:(Uint8)(elapsed*255/300);
  if(alpha==255) g_backgroundPrevious.clear();
  else g_redrawUntil=SDL_GetTicks()+100;
  drawBackgroundArtwork(g_backgroundPrevious,255);
  drawBackgroundArtwork(g_backgroundCurrent,alpha);
  fillRect(0,0,SW,SH,{0,0,0,25});
  drawGradient({0,0,SW*2/3,SH},{0,0,0,120},{0,0,0,0},true);
  drawGradient({0,SH/2,SW,SH-SH/2},{0,0,0,0},{0,0,0,220});
}

static bool promptTextMode(const char *header, const char *initial, char *out, size_t outSize,
                           bool password, bool allowEmpty,
                           const char *subText=nullptr, const char *guideText=nullptr) {
  SwkbdConfig kbd;
  out[0] = 0;
  if (R_FAILED(swkbdCreate(&kbd, 0))) return false;
  if(password) swkbdConfigMakePresetPassword(&kbd); else swkbdConfigMakePresetDefault(&kbd);
  if (header) swkbdConfigSetHeaderText(&kbd, header);
  if (subText) swkbdConfigSetSubText(&kbd, subText);
  if (guideText) swkbdConfigSetGuideText(&kbd, guideText);
  if (initial && *initial) swkbdConfigSetInitialText(&kbd, initial);
  swkbdConfigSetStringLenMax(&kbd, (u32)(outSize - 1));
  Result rc = swkbdShow(&kbd, out, outSize);
  swkbdClose(&kbd);
  return R_SUCCEEDED(rc) && (allowEmpty || out[0]);
}
static bool promptText(const char *header, const char *initial, char *out, size_t outSize) {
  return promptTextMode(header,initial,out,outSize,false,false);
}

template<size_t N>
static bool promptTextStatic(const char (&header)[N],const char *initial,char *out,size_t outSize) {
  return promptText(LauncherLocalization::Translate(header).data(),initial,out,outSize);
}

template<size_t N>
static bool promptTextModeStatic(const char (&header)[N],const char *initial,char *out,size_t outSize,
                                 bool password,bool allowEmpty,const char *subText=nullptr,
                                 const char *guideText=nullptr) {
  return promptTextMode(LauncherLocalization::Translate(header).data(),initial,out,outSize,
                        password,allowEmpty,subText,guideText);
}

static bool folderIsExtractedTitle(const std::string &dir);
static bool pathIsInstallable(const std::string &path, bool directory);
static void installFromPath(const std::string &picked);

struct FileClipboard {
  std::string path;
  bool move=false;
};
static FileClipboard g_fileClipboard;

static bool filesystemRoot(const std::string &path) {
  std::string normalized=normalizeLocationPath(path);
  size_t colon=normalized.find(':');
  if(colon==std::string::npos) return normalized=="/";
  for(size_t i=colon+1;i<normalized.size();i++) if(normalized[i]!='/') return false;
  return true;
}

static std::string parentFolder(const std::string &path) {
  std::string normalized=normalizeLocationPath(path);
  if(filesystemRoot(normalized)) return {};
  size_t slash=normalized.find_last_of('/');
  if(slash==std::string::npos) return {};
  size_t colon=normalized.find(':');
  if(colon!=std::string::npos && slash<=colon+1) return normalized.substr(0,colon+2);
  return normalized.substr(0,slash);
}

static std::string fileNameOf(const std::string &path) {
  std::string normalized=normalizeLocationPath(path);
  size_t slash=normalized.find_last_of('/');
  return slash==std::string::npos?normalized:normalized.substr(slash+1);
}

static std::string deviceOf(const std::string &path) {
  size_t colon=path.find(':');
  return foldedKey(colon==std::string::npos?std::string{}:path.substr(0,colon));
}

static bool pathAtOrBelow(const std::string &path,const std::string &root) {
  std::string candidate=pathIdentity(path), base=pathIdentity(root);
  if(base.empty()||candidate.size()<base.size()||candidate.compare(0,base.size(),base)!=0) return false;
  if(candidate.size()==base.size()) return true;
  return base.back()=='/'||candidate[base.size()]=='/';
}



static void replaceSavedPathPrefix(const std::string &oldPath,const std::string &newPath) {
  const std::string normalizedOld=normalizeLocationPath(oldPath);
  const std::string normalizedNew=normalizeLocationPath(newPath);
  const std::string oldIdentity=pathIdentity(normalizedOld);
  auto replace=[&](std::vector<std::string> &paths){
    for(auto &path:paths){
      const std::string normalizedPath=normalizeLocationPath(path);
      const std::string identity=pathIdentity(normalizedPath);
      if(identity==oldIdentity) path=normalizedNew;
      else if(identity.size()>oldIdentity.size() && identity.compare(0,oldIdentity.size(),oldIdentity)==0 && identity[oldIdentity.size()]=='/')
        path=normalizeLocationPath(normalizedNew+normalizedPath.substr(normalizedOld.size()));
    }
  };
  auto sources=loadGameSources(); replace(sources); saveGameSources(sources);
  auto favorites=loadFavoriteFolders(); replace(favorites); saveFavoriteFolders(favorites);
  if(!g_fileClipboard.path.empty() && pathAtOrBelow(g_fileClipboard.path,normalizedOld)){
    const std::string clipboardPath=normalizeLocationPath(g_fileClipboard.path);
    g_fileClipboard.path=normalizeLocationPath(normalizedNew+clipboardPath.substr(normalizedOld.size()));
  }
  g_rescanAfterSettings=true;
}

static void removeSavedPathsBelow(const std::string &root) {
  auto sources=loadGameSources();
  sources.erase(std::remove_if(sources.begin(),sources.end(),[&](const std::string &path){ return pathAtOrBelow(path,root); }),sources.end());
  saveGameSources(sources);
  auto favorites=loadFavoriteFolders();
  favorites.erase(std::remove_if(favorites.begin(),favorites.end(),[&](const std::string &path){ return pathAtOrBelow(path,root); }),favorites.end());
  saveFavoriteFolders(favorites);
  if(!g_fileClipboard.path.empty()&&pathAtOrBelow(g_fileClipboard.path,root)) g_fileClipboard={};
  g_rescanAfterSettings=true;
}

static bool validEntryName(const std::string &name) {
  if(name.empty()||name=="."||name==".."||name.size()>255) return false;
  for(unsigned char c:name) if(c<' '||c=='/'||c=='\\'||c==':') return false;
  return true;
}

static bool removeTreeInternal(const std::string &path) {
  if(filesystemRoot(path)) return false;
  struct stat st{};
  if(lstat(path.c_str(),&st)!=0) return errno==ENOENT;
  if(S_ISREG(st.st_mode)||S_ISLNK(st.st_mode)) return remove(path.c_str())==0;
  if(!S_ISDIR(st.st_mode)) return false;
  DIR *dir=opendir(path.c_str()); if(!dir) return false;
  bool ok=true; struct dirent *entry;
  while(ok&&(entry=readdir(dir))){
    if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,"..")) continue;
    ok=removeTreeInternal(join(path,entry->d_name));
  }
  if(closedir(dir)!=0) ok=false;
  return ok&&rmdir(path.c_str())==0;
}

struct TransferState {
  uint64_t total=0;
  std::atomic<uint64_t> done{0};
  std::string current,error;
  std::vector<unsigned char> buffer=std::vector<unsigned char>(1<<18);
  std::mutex detailMutex;
  std::atomic<bool> cancelled{false};
};

static void setTransferDetail(TransferState &state,const std::string &current,const std::string &error={}) {
  std::lock_guard<std::mutex> lock(state.detailMutex);
  if(!current.empty()) state.current=current;
  if(!error.empty()) state.error=error;
}

static std::string transferError(TransferState &state) {
  std::lock_guard<std::mutex> lock(state.detailMutex);
  return state.error;
}

static bool transferFrame(TransferState &state) {
  if(!beginUiFrame()){ state.cancelled.store(true); return false; }
  SDL_Event event;
  while(pollUiEvent(event)){
    pumpStick(event);
    int tx=0,ty=0;
    if(touchFeed(event,&tx,&ty)==TOUCH_TAP&&ty>=SH-100) state.cancelled.store(true);
    if(event.type==SDL_CONTROLLERBUTTONDOWN&&event.cbutton.button==BTN_CANCEL) state.cancelled.store(true);
  }
  std::string current;
  { std::lock_guard<std::mutex> lock(state.detailMutex); current=state.current; }
  clearUiBackground();
  drawHeaderStatic("File transfer",current.empty()?nullptr:current.c_str());
  const int panelW=std::min(900,SW-160),panelH=180;
  const int panelX=(SW-panelW)/2,panelY=(SH-panelH)/2-16;
  glassPanel(panelX,panelY,panelW,panelH);
  drawTextC(g_font_sm,SW/2,panelY+30,ellipsizedText(g_font_sm,current,panelW-64).c_str(),COL_DIM);
  const int bw=panelW-72,bx=(SW-bw)/2,by=panelY+82,bh=26;
  uint64_t done=state.done.load(std::memory_order_relaxed);
  uint64_t progress=state.total?std::min(done,state.total):0;
  drawProgressBar(bx,by,bw,bh,state.total?(double)done/state.total:0.0);
  char text[96];
  int percent=state.total?(int)(progress*100/state.total):0;
  snprintf(text,sizeof(text),"%d%%  -  %.1f / %.1f MiB",percent,done/1048576.0,state.total/1048576.0);
  drawTextC(g_font,SW/2,by+bh+22,text,COL_TXT);
  if(state.cancelled.load())
    drawStaticTextC(g_font_sm,SW/2,SH-72,"Cancelling...",COL_VAL);
  else
    drawLocalizedFooter("B  Cancel",SH-60);
  SDL_RenderPresent(g_ren);
  return !state.cancelled.load();
}

static bool measureTree(const std::string &path,TransferState &state) {
  if(state.cancelled.load(std::memory_order_relaxed)) return false;
  struct stat st{};
  if(lstat(path.c_str(),&st)!=0){ setTransferDetail(state,{},"Source is no longer available"); return false; }
  if(S_ISREG(st.st_mode)){ state.total+=(uint64_t)st.st_size; return true; }
  if(!S_ISDIR(st.st_mode)){ setTransferDetail(state,{},"Unsupported file type"); return false; }
  DIR *dir=opendir(path.c_str()); if(!dir){ setTransferDetail(state,{},"Could not open a source folder"); return false; }
  bool ok=true; struct dirent *entry;
  while(ok&&!state.cancelled.load(std::memory_order_relaxed)&&(entry=readdir(dir))){
    if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,"..")) continue;
    ok=measureTree(join(path,entry->d_name),state);
  }
  if(closedir(dir)!=0) ok=false;
  return ok;
}

static bool copyFileAtomic(const std::string &source,const std::string &destination,TransferState &state) {
  setTransferDetail(state,fileNameOf(source));
  const std::string partial=destination+".cemu-part", backup=destination+".cemu-old";
  remove(partial.c_str());
  FILE *input=fopen(source.c_str(),"rb");
  if(!input){ setTransferDetail(state,{},"Could not open the source file"); return false; }
  FILE *output=fopen(partial.c_str(),"wb");
  if(!output){ fclose(input); setTransferDetail(state,{},"Could not create the destination file"); return false; }
  bool ok=true;
  while(ok&&!state.cancelled.load(std::memory_order_relaxed)){
    size_t count=fread(state.buffer.data(),1,state.buffer.size(),input);
    if(count){
      if(fwrite(state.buffer.data(),1,count,output)!=count){ setTransferDetail(state,{},"Write failed; check free space and permissions"); ok=false; break; }
      state.done.fetch_add(count,std::memory_order_relaxed);
    }
    if(count<state.buffer.size()){
      if(ferror(input)){ setTransferDetail(state,{},"Read failed"); ok=false; }
      break;
    }
  }
  if(state.cancelled.load()) ok=false;
  if(ok&&fflush(output)!=0){ setTransferDetail(state,{},"Could not flush the destination file"); ok=false; }
  if(ok&&fsync(fileno(output))!=0){ setTransferDetail(state,{},"Could not commit the destination file"); ok=false; }
  if(fclose(input)!=0&&ok){ setTransferDetail(state,{},"Could not close the source file"); ok=false; }
  if(fclose(output)!=0&&ok){ setTransferDetail(state,{},"Could not close the destination file"); ok=false; }
  if(!ok||state.cancelled.load()){ remove(partial.c_str()); return false; }
  struct stat destinationStat{}; bool existed=stat(destination.c_str(),&destinationStat)==0;
  if(existed){
    struct stat backupStat{};
    if(lstat(backup.c_str(),&backupStat)==0){ setTransferDetail(state,{},"A previous backup file blocks this operation"); remove(partial.c_str()); return false; }
    if(rename(destination.c_str(),backup.c_str())!=0){ setTransferDetail(state,{},"Could not preserve the existing destination"); remove(partial.c_str()); return false; }
  }
  if(rename(partial.c_str(),destination.c_str())!=0){
    if(existed) rename(backup.c_str(),destination.c_str());
    setTransferDetail(state,{},"Could not finalize the copied file"); remove(partial.c_str()); return false;
  }
  if(existed) remove(backup.c_str());
  return true;
}

static bool copyTree(const std::string &source,const std::string &destination,TransferState &state) {
  struct stat st{};
  if(lstat(source.c_str(),&st)!=0){ setTransferDetail(state,{},"Source is no longer available"); return false; }
  if(S_ISREG(st.st_mode)) return copyFileAtomic(source,destination,state);
  if(!S_ISDIR(st.st_mode)){ setTransferDetail(state,{},"Unsupported file type"); return false; }
  if(mkdir(destination.c_str(),0777)!=0){ setTransferDetail(state,{},"Could not create a destination folder"); return false; }
  DIR *dir=opendir(source.c_str());
  if(!dir){ setTransferDetail(state,{},"Could not open a source folder"); return false; }
  bool ok=true; struct dirent *entry;
  while(ok&&!state.cancelled.load()&&(entry=readdir(dir))){
    if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,"..")) continue;
    ok=copyTree(join(source,entry->d_name),join(destination,entry->d_name),state);
  }
  if(closedir(dir)!=0&&ok){ setTransferDetail(state,{},"Could not close a source folder"); ok=false; }
  return ok&&!state.cancelled.load();
}

static bool enoughFreeSpace(const std::string &folder,uint64_t bytes) {
  struct statvfs info{};
  if(statvfs(folder.c_str(),&info)!=0||!info.f_frsize) return true;
  return bytes<=static_cast<uint64_t>(info.f_bavail)*info.f_frsize;
}

static bool executePaste(const std::string &folder) {
  if(g_fileClipboard.path.empty()) return false;
  struct stat sourceStat{};
  if(lstat(g_fileClipboard.path.c_str(),&sourceStat)!=0){ modalMessageStatic("Paste failed",{"The copied item is no longer available."}); g_fileClipboard={}; return false; }
  const std::string destination=join(folder,fileNameOf(g_fileClipboard.path));
  if(pathIdentity(destination)==pathIdentity(g_fileClipboard.path) ||
     (S_ISDIR(sourceStat.st_mode)&&pathAtOrBelow(destination,g_fileClipboard.path))){
    modalMessageStatic("Paste failed",{"The destination cannot be inside the source."}); return false;
  }
  struct stat destinationStat{}; bool destinationExists=lstat(destination.c_str(),&destinationStat)==0;
  if(destinationExists&&S_ISDIR(sourceStat.st_mode)){
    modalMessageStatic("Folder already exists",{"Choose another destination or rename the folder first.",destination}); return false;
  }
  if(destinationExists&&!S_ISREG(destinationStat.st_mode)){
    modalMessageStatic("Paste failed",{"The destination is not a regular file."}); return false;
  }
  if(destinationExists&&!confirmBoxStatic("Replace existing file?",{fileNameOf(destination),"","The existing file will be replaced."})) return false;

  bool sameDevice=deviceOf(g_fileClipboard.path)==deviceOf(destination);
  if(g_fileClipboard.move&&sameDevice){
    const std::string backup=destination+".cemu-old";
    bool preserved=false;
    if(destinationExists){
      struct stat backupStat{};
      if(lstat(backup.c_str(),&backupStat)==0||rename(destination.c_str(),backup.c_str())!=0){ modalMessageStatic("Move failed",{"Could not preserve the existing destination."}); return false; }
      preserved=true;
    }
    if(rename(g_fileClipboard.path.c_str(),destination.c_str())==0){
      if(preserved) remove(backup.c_str());
      replaceSavedPathPrefix(g_fileClipboard.path,destination);
      g_fileClipboard={}; toastStatic("Move complete"); return true;
    }
    if(preserved) rename(backup.c_str(),destination.c_str());
  }

  TransferState state;
  setTransferDetail(state,fileNameOf(g_fileClipboard.path));
  bool ok=false,measured=false,spaceAvailable=true;
  std::atomic<bool> complete{false};
  appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
  std::thread worker([&](){
    measured=measureTree(g_fileClipboard.path,state);
    if(measured&&!state.cancelled.load()) spaceAvailable=enoughFreeSpace(folder,state.total);
    if(measured&&spaceAvailable&&!state.cancelled.load())
      ok=copyTree(g_fileClipboard.path,destination,state);
    complete.store(true,std::memory_order_release);
    wakeUiFromWorker(0x46494c45);
  });
  while(!complete.load(std::memory_order_acquire)){
    transferFrame(state);
    waitForNextUiFrame();
  }
  worker.join();
  appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
  if(!measured&&!state.cancelled.load()){
    modalMessageStatic("Paste failed",{transferError(state)}); return false;
  }
  if(!spaceAvailable){
    modalMessageStatic("Not enough free space",{"The destination does not have enough available space."}); return false;
  }
  if(!ok&&S_ISDIR(sourceStat.st_mode)) removeTreeInternal(destination);
  if(ok&&g_fileClipboard.move){
    if(removeTreeInternal(g_fileClipboard.path)) replaceSavedPathPrefix(g_fileClipboard.path,destination);
    else { modalMessageStatic("Move incomplete",{"The copy completed, but the original could not be removed completely.","Review both locations before trying again."}); ok=false; }
  }
  if(ok){ g_rescanAfterSettings=true; if(g_fileClipboard.move) g_fileClipboard={}; toastStatic("Transfer complete"); }
  else if(state.cancelled.load()){ toastStatic("Transfer cancelled"); }
  else { std::string error=transferError(state); modalMessageStatic("Transfer failed",{error.empty()?"The file transfer could not be completed.":error}); }
  return ok;
}

static bool connectSmbInteractive(const SwitchStorage::SmbShare &share,
                                  bool reconnect,std::string &error) {
  std::atomic_bool cancel{false},complete{false};
  bool connected=false;
  std::thread worker([&]{
    connected=reconnect?SwitchStorage::ReconnectSmb(share.id,&error,&cancel):
                        SwitchStorage::MountSmb(share,&error,&cancel);
    complete=true; wakeUiFromWorker(0x534d4243);
  });
  while(!complete.load(std::memory_order_acquire)){
    if(!beginUiFrame()){ cancel=true; break; }
    SDL_Event event;
    while(pollUiEvent(event)){
      pumpStick(event); int x=0,y=0;
      if((event.type==SDL_CONTROLLERBUTTONDOWN&&event.cbutton.button==BTN_CANCEL)||
         (touchFeed(event,&x,&y)==TOUCH_TAP&&y>=SH-90)) cancel=true;
    }
    clearUiBackground(); drawHeaderStatic("SMB network shares",nullptr);
    const int waitW=std::min(620,SW-96),waitH=140;
    glassPanel((SW-waitW)/2,(SH-waitH)/2,waitW,waitH);
    drawTextC(g_font,SW/2,SH/2-10,LauncherLocalization::Translate(
      cancel.load()?"Cancelling...":"Connecting...").data(),COL_TXT);
    drawLocalizedFooter("B  Cancel");
    SDL_RenderPresent(g_ren); waitForNextUiFrame();
  }
  if(worker.joinable()) worker.join();
  return connected&&!cancel.load();
}

static bool editSmbShare(SwitchStorage::SmbShare &share,bool creating) {
  SwitchStorage::SmbShare edited=share;
  constexpr int fieldCount=7,saveRow=7,totalRows=8;
  int sel=0;
  bool done=false,saved=false;
  beginScreenFx();

  auto cleanServer=[&](){
    edited.server=trim(edited.server);
    if(edited.server.rfind("smb://",0)==0) edited.server.erase(0,6);
    while(!edited.server.empty()&&edited.server.back()=='/') edited.server.pop_back();
  };
  auto cleanShare=[&](){
    std::string combined=trim(edited.share);
    if(!edited.path.empty()) combined+="/"+edited.path;
    std::replace(combined.begin(),combined.end(),'\\','/');
    while(!combined.empty()&&combined.front()=='/') combined.erase(combined.begin());
    while(!combined.empty()&&combined.back()=='/') combined.pop_back();
    std::string normalized; bool slash=false;
    for(char value:combined){
      if(value=='/'){ if(slash) continue; slash=true; }
      else slash=false;
      normalized+=value;
    }
    size_t separator=normalized.find('/');
    edited.share=trim(normalized.substr(0,separator));
    edited.path=separator==std::string::npos?std::string{}:trim(normalized.substr(separator+1));
  };
  auto sharedFolder=[&](){ return edited.path.empty()?edited.share:edited.share+"/"+edited.path; };
  auto validate=[&](){
    edited.name=trim(edited.name); cleanServer(); cleanShare();
    if(edited.name.empty()){ modalMessageStatic("Display name required",{"Enter a name used to identify this share in Cemu."}); return false; }
    if(edited.server.empty()||edited.server.find('/')!=std::string::npos||edited.server.find('\\')!=std::string::npos){
      modalMessageStatic("Invalid SMB server",{"Enter only a host name or IP address.","Example: 192.168.1.20"}); return false;
    }
    bool invalidPath=edited.share.empty()||edited.share.find(':')!=std::string::npos;
    size_t start=0;
    while(!invalidPath&&start<=edited.path.size()){
      size_t slash=edited.path.find('/',start);
      std::string component=trim(edited.path.substr(start,slash==std::string::npos?std::string::npos:slash-start));
      if((component.empty()&&!edited.path.empty())||component=="."||component==".."||component.find(':')!=std::string::npos) invalidPath=true;
      if(slash==std::string::npos) break;
      start=slash+1;
    }
    if(invalidPath){
      modalMessageStatic("Invalid SMB share",{"Enter a share name, optionally followed by folders.","Do not include a drive letter or smb:// prefix."}); return false;
    }
    return true;
  };
  auto editField=[&](int index){
    char value[256]; bool accepted=false;
    if(index==0) accepted=promptTextModeStatic("SMB display name",edited.name.c_str(),value,sizeof(value),false,false,
      "Friendly name shown in the Cemu file browser.","Example: Living room NAS");
    else if(index==1) accepted=promptTextModeStatic("Server or IP address",edited.server.c_str(),value,sizeof(value),false,false,
      "Enter the network host only. Do not include smb:// or a folder.","Example: 192.168.1.20 or NAS.local");
    else if(index==2){ std::string folder=sharedFolder(); accepted=promptTextModeStatic("Shared folder",folder.c_str(),value,sizeof(value),false,false,
      "Enter the share and an optional folder path inside it.","Nested folders are supported."); }
    else if(index==3) accepted=promptTextModeStatic("Username",edited.user.c_str(),value,sizeof(value),false,true,
      "Account used by the SMB server. Leave blank for guest access.","Leave blank for guest");
    else if(index==4) accepted=promptTextModeStatic("Password",edited.password.c_str(),value,sizeof(value),true,true,
      "Password for the SMB account. It is stored in launcher.ini.","Leave blank when no password is required");
    else if(index==5) accepted=promptTextModeStatic("Workgroup",edited.domain.c_str(),value,sizeof(value),false,true,
      "Usually optional on a home network.","Example: WORKGROUP, or leave blank");
    if(!accepted) return;
    if(index==0) edited.name=value;
    else if(index==1){ edited.server=value; cleanServer(); }
    else if(index==2){ edited.share=value; edited.path.clear(); cleanShare(); }
    else if(index==3) edited.user=value;
    else if(index==4) edited.password=value;
    else if(index==5) edited.domain=value;
    beginScreenFx();
  };
  auto activate=[&](){
    if(sel<6) editField(sel);
    else if(sel==6) edited.autoMount=!edited.autoMount;
    else if(validate()){
      if(creating){
        std::unordered_set<std::string> ids;
        for(const auto &existing:loadSmbSharesFromStore()) ids.insert(existing.id);
        uint64_t seed=armGetSystemTick();
        do { char id[17]; snprintf(id,sizeof(id),"%08llx",(unsigned long long)(seed&0xffffffffULL)); edited.id=id; seed=seed*6364136223846793005ULL+1; } while(ids.count(edited.id));
      }
      share=std::move(edited); saved=true; done=true;
    }
  };

  while(!done){
    if(!beginUiFrame()) break;
    const int rowHeight=54,y0=settingsListY();
    const int margin=56,helpWidth=420,gap=28;
    const int formWidth=SW-margin*2-helpWidth-gap,helpX=margin+formWidth+gap;
    const int buttonY=y0+fieldCount*rowHeight+10;
    SDL_Event event; navRepeat();
    while(pollUiEvent(event)){
      pumpStick(event);
      int tx=0,ty=0; TouchKind touch=touchFeed(event,&tx,&ty);
      if(touch==TOUCH_TAP){
        if(ty>=SH-42){ done=true; continue; }
        for(int index=0;index<fieldCount;index++) if(tx>=margin&&tx<margin+formWidth&&ty>=y0+index*rowHeight&&ty<y0+(index+1)*rowHeight){ sel=index; activate(); break; }
        if(tx>=margin&&tx<margin+formWidth&&ty>=buttonY&&ty<buttonY+rowHeight){ sel=saveRow; activate(); }
        continue;
      }
      if(event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP) sel=(sel+totalRows-1)%totalRows;
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN) sel=(sel+1)%totalRows;
      else if((event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_LEFT||event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_RIGHT)&&sel==6) edited.autoMount=!edited.autoMount;
      else if(event.cbutton.button==BTN_CONFIRM) activate();
      else if(event.cbutton.button==BTN_CANCEL) done=true;
    }

    clearUiBackground();
    drawHeader(LauncherLocalization::Translate(
                 creating?"Add SMB network share":"Edit SMB network share").data(),
               edited.name.empty()?nullptr:edited.name.c_str());
    const int panelHeight=fieldCount*rowHeight+rowHeight+30;
    glassPanel(margin,y0-10,formWidth,panelHeight);
    glassPanel(helpX,y0-10,helpWidth,panelHeight);
    const char *labels[fieldCount]={"Display name","Server / IP address","Shared folder","Username","Password","Workgroup","Connect at startup"};
    std::string password=edited.password.empty()?"Not set":std::string(std::min<size_t>(16,edited.password.size()),'*');
    const std::string values[fieldCount]={
      edited.name.empty()?"Not set":edited.name,
      edited.server.empty()?"Not set":edited.server,
      edited.share.empty()?"Not set":sharedFolder(),
      edited.user.empty()?"Guest":edited.user,
      password,
      edited.domain.empty()?"Optional":edited.domain,
      edited.autoMount?"On":"Off"
    };
    for(int index=0;index<fieldCount;index++){
      int y=y0+index*rowHeight; bool current=sel==index;
      if(current){ drawRowHighlight(margin+8,y,formWidth-16,rowHeight-2); }
      drawText(g_font_sm,margin+30,y+(rowHeight-fontHeight(g_font_sm))/2,labels[index],current?COL_VAL:COL_DIM);
      drawScrollTextR(g_font,margin+formWidth-24,y+(rowHeight-fontHeight(g_font))/2,formWidth/2-30,values[index].c_str(),current?COL_VAL:COL_TXT);
    }
    const bool buttonSelected=sel==saveRow;
    drawButtonPanel(margin+14,buttonY,formWidth-28,rowHeight-4,buttonSelected);
    drawTextC(g_font,margin+formWidth/2,buttonY+(rowHeight-fontHeight(g_font))/2-2,
              LauncherLocalization::Translate(
                creating?"Connect and save":"Save changes").data(),
              buttonSelected?COL_VAL:COL_HI);

    static const char *helpTitle[totalRows]={"Display name","Server / IP address","Shared folder","Username","Password","Workgroup","Connect at startup","Save share"};
    static const char *helpLine1[totalRows]={
      "A friendly name shown only in Cemu.","The host name or IP of your SMB server.","The share name and optional folder path.","Leave blank when the share allows guests.",
      "The password for the selected account.","Usually optional on home networks.","Reconnect this share when the launcher opens.","Validate the fields and connect to the share."
    };
    static const char *helpLine2[totalRows]={
      "Example: Living room NAS","Example: 192.168.1.20 or NAS.local","Nested folders are supported.","Use the account configured on your NAS or PC.",
      "The value is masked on this screen.","Example: WORKGROUP","Turn this off for manually connected shares.","Connection errors will be shown after saving."
    };
    drawText(g_font_big,helpX+28,y0+22,helpTitle[sel],COL_HI);
    int helpLineHeight=fontHeight(g_font_sm)+4;
    drawWrapped(g_font_sm,helpX+28,y0+92,helpWidth-56,helpLineHeight,2,helpLine1[sel],COL_TXT);
    drawWrapped(g_font_sm,helpX+28,y0+156,helpWidth-56,helpLineHeight,2,helpLine2[sel],COL_DIM);
    std::string address="smb://"+(edited.server.empty()?std::string("server"):edited.server)+"/"+(edited.share.empty()?std::string("share"):sharedFolder());
    drawStaticText(g_font_sm,helpX+28,y0+210,"Connection preview",COL_DIM);
    drawScrollTextL(g_font,helpX+28,y0+244,helpWidth-56,address.c_str(),COL_VAL);
    drawLocalizedFooter(sel==saveRow?
      (creating?"A  Connect and save       B  Cancel":"A  Save changes       B  Cancel"):
      "A  Edit / toggle       B  Cancel");
    drawFadeIn(); SDL_RenderPresent(g_ren); waitForNextUiFrame();
  }
  return saved;
}

static void networkSharesScreen() {
  int sel=0,top=0;
  for(;;){
    auto shares=loadSmbSharesFromStore(); int n=1+(int)shares.size();
    const int listY=settingsListY(),rowHeight=60;
    int vis=std::max(1,(SH-listY-settingsFooterReserve())/rowHeight);
    sel=std::max(0,std::min(sel,n-1)); if(sel<top)top=sel; if(sel>=top+vis)top=sel-vis+1;
    bool rebuild=false;
    while(!rebuild){
      if(!beginUiFrame()) return;
      SDL_Event event; navRepeat();
      while(pollUiEvent(event)){
        pumpStick(event); int tx=0,ty=0; TouchKind touch=touchFeed(event,&tx,&ty);
        if(touchScrollList(touch,sel,top,n,vis)) continue;
        if(touch==TOUCH_TAP){
          if(ty>=SH-48) return;
          for(int row=0;row<vis&&top+row<n;row++){ int y=listY+row*rowHeight; if(ty>=y&&ty<y+rowHeight-4){ sel=top+row; SDL_Event press{}; press.type=SDL_CONTROLLERBUTTONDOWN; press.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&press); break; } }
          continue;
        }
        if(event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
        if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP) sel=(sel+n-1)%n;
        else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN) sel=(sel+1)%n;
        else if(event.cbutton.button==BTN_CANCEL) return;
        else if(event.cbutton.button==BTN_CONFIRM){
          if(sel==0){
            if(shares.size()>=8){ toastStatic("Maximum of 8 SMB shares"); continue; }
            SwitchStorage::SmbShare share;
            if(editSmbShare(share,true)){
              shares.push_back(share); saveSmbShares(shares);
              std::string error; if(!connectSmbInteractive(share,false,error)&&!error.empty()) modalMessageStatic("SMB connection failed",{error});
              sel=(int)shares.size(); rebuild=true;
            }
          } else {
            auto &share=shares[sel-1]; bool mounted=SwitchStorage::IsSmbMounted(share.id);
            const char *actions[]={mounted?"Disconnect":"Connect","Edit","Toggle connect at startup","Remove"};
            int action=dropdown(share.name.c_str(),actions,4,0);
            if(action==0){
              if(mounted) SwitchStorage::UnmountSmb(share.id);
              else { std::string error; if(!connectSmbInteractive(share,false,error)&&!error.empty()) modalMessageStatic("SMB connection failed",{error}); }
              rebuild=true;
            } else if(action==1){
              SwitchStorage::SmbShare edited=share;
              if(editSmbShare(edited,false)){
                bool reconnect=mounted||edited.autoMount;
                SwitchStorage::UnmountSmb(share.id); share=std::move(edited); saveSmbShares(shares);
                if(reconnect){ std::string error; if(!connectSmbInteractive(share,false,error)&&!error.empty()) modalMessageStatic("SMB connection failed",{error}); }
                rebuild=true;
              }
            } else if(action==2){ share.autoMount=!share.autoMount; saveSmbShares(shares); rebuild=true; }
            else if(action==3&&confirmBoxStatic("Remove SMB share?",{share.name,"","Saved folders on this share will also be removed."})){
              std::string root=SwitchStorage::SmbRootPath(share.id); SwitchStorage::UnmountSmb(share.id);
              shares.erase(shares.begin()+sel-1); saveSmbShares(shares); removeSavedPathsBelow(root);
              sel=std::max(0,sel-1); rebuild=true;
            }
          }
        }
        if(sel<top) top=sel;
        if(sel>=top+vis) top=sel-vis+1;
      }
      if(rebuild) break;
      clearUiBackground();
      std::string summary=std::to_string(shares.size())+(shares.size()==1?" saved share":" saved shares");
      drawHeaderStatic("SMB network shares",summary.c_str());
      glassPanel(44,listY-12,SW-88,std::min(vis,std::max(1,n-top))*rowHeight+18);
      for(int row=0;row<vis&&top+row<n;row++){
        int index=top+row,y=listY+row*rowHeight; bool current=index==sel;
        if(current){ drawRowHighlight(56,y-3,SW-112,rowHeight-4); }
        if(index==0) drawStaticText(g_font,82,y+(rowHeight-fontHeight(g_font))/2-2,"[ Add SMB share ]",current?COL_VAL:COL_HI);
        else { const auto &share=shares[index-1]; bool mounted=SwitchStorage::IsSmbMounted(share.id);
          drawText(g_font,82,y,share.name.c_str(),current?COL_VAL:COL_TXT);
          std::string status=mounted?"Connected":(share.autoMount?"Disconnected - auto":"Disconnected");
          drawTextR(g_font_sm,SW-82,y+4,status.c_str(),mounted?(SDL_Color){120,220,120,255}:COL_DIM);
          std::string address="smb://"+share.server+"/"+share.share+(share.path.empty()?std::string{}:"/"+share.path);
          drawText(g_font_sm,82,y+31,ellipsizedText(g_font_sm,address,SW-340).c_str(),COL_DIM); }
      }
      drawLocalizedFooter("A  Select       B  Back");
    SDL_RenderPresent(g_ren); waitForNextUiFrame();
    }
  }
}

enum class BrowserMode { SelectFolder, SelectImage, Manage };
enum class BrowserItemKind { Use, Up, Paste, Favorite, Directory, File, Location, Smb, ManageSmb };
struct BrowserItem {
  std::string label,path;
  BrowserItemKind kind=BrowserItemKind::File;
  bool directory=false;
  std::string stableId;
  BrowserItem(std::string itemLabel,std::string itemPath,BrowserItemKind itemKind,
              bool itemDirectory,std::string itemStableId={})
    : label(std::move(itemLabel)),path(std::move(itemPath)),kind(itemKind),
      directory(itemDirectory),stableId(std::move(itemStableId)) {}
};

static bool ensurePathMounted(const std::string &path) {
  for(const auto &share:loadSmbSharesFromStore()){
    std::string root=SwitchStorage::SmbRootPath(share.id);
    if(pathAtOrBelow(path,root)){
      if(SwitchStorage::IsSmbMounted(share.id)) return true;
      std::string error;
      if(connectSmbInteractive(share,false,error)) return true;
      if(!error.empty()) modalMessageStatic("SMB connection failed",{share.name,error});
      return false;
    }
  }
  return true;
}

static bool isUsbStoragePath(const std::string &path) {
  size_t colon=path.find(':');
  if(colon<4) return false;
  if(tolower((unsigned char)path[0])!='u'||tolower((unsigned char)path[1])!='m'||tolower((unsigned char)path[2])!='s') return false;
  for(size_t index=3;index<colon;index++) if(!isdigit((unsigned char)path[index])) return false;
  return true;
}

static bool hasConfiguredUsbSource(const std::vector<std::string> &paths) {
  return std::any_of(paths.begin(),paths.end(),[](const std::string &path){ return isUsbStoragePath(path); });
}

static std::vector<std::string> localGameSources(const std::vector<std::string> &paths) {
  std::vector<std::string> local;
  for(const std::string &path:paths)
    if(!isUsbStoragePath(path)&&!isUnresolvedUsbSource(path)&&path.rfind("cemusmb_",0)!=0)
      local.push_back(path);
  return local;
}

static bool hasConfiguredUsbBinding() {
  const int count=std::max(0,std::min(16,atoi(storeGet(g_global,"Wrapper/GamePathCount","1"))));
  for(int index=0;index<count;index++){
    const std::string key="Wrapper/GamePathStable"+std::to_string(index);
    if(storeGet(g_global,key.c_str(),"")[0]) return true;
  }
  return false;
}

static bool refreshConfiguredUsbSources(std::vector<std::string> &paths) {
  bool persistChanged=false;
  const auto records=resolvedGameSourceRecords(true,&persistChanged);
  std::vector<std::string> resolved;
  resolved.reserve(records.size());
  for(const auto &record:records) if(!record.runtimePath.empty()) resolved.push_back(record.runtimePath);
  const bool changed=resolved!=paths;
  paths=std::move(resolved);
  if(persistChanged){
    persistGameSourceRecords(records);
    storeSave(g_global,LAUNCHER_INI);
  }
  return changed;
}

static void renderUsbForwarderWait() {
  SDL_SetRenderDrawColor(g_ren,0,0,0,255);
  SDL_RenderClear(g_ren);
  SDL_RenderPresent(g_ren);
}

static void ensureSavedPathMountedAtStartup(const std::string &path) {
  auto shares=loadSmbSharesFromStore();
  bool changed=false;
  for(auto &share:shares){
    if(pathAtOrBelow(path,SwitchStorage::SmbRootPath(share.id))&&!share.autoMount){
      share.autoMount=true;
      changed=true;
    }
  }
  if(changed) saveSmbShares(shares);
}

static std::vector<BrowserItem> browserItems(const std::string &current,BrowserMode mode,bool &opened) {
  std::vector<BrowserItem> items; opened=true;
  if(current.empty()){
    SwitchStorage::InitializeUsb();
    items.push_back({"SD card","sdmc:/",BrowserItemKind::Location,true});
    for(const auto &usb:SwitchStorage::ListUsbLocations()) items.push_back({usb.label,usb.path,BrowserItemKind::Location,true,usb.id});
    for(const auto &share:loadSmbSharesFromStore()){
      bool mounted=SwitchStorage::IsSmbMounted(share.id);
      std::string label="SMB - "+(share.name.empty()?share.share:share.name)+(mounted?"":" (disconnected)");
      items.push_back({label,SwitchStorage::SmbBrowsePath(share),BrowserItemKind::Smb,true});
    }
    for(const auto &favorite:loadFavoriteFolders()) items.push_back({"Pinned - "+favorite,favorite,BrowserItemKind::Location,true});
    items.push_back({"Manage SMB shares","",BrowserItemKind::ManageSmb,true});
    return items;
  }
  if(mode==BrowserMode::SelectFolder) items.push_back({"[ Use this folder ]",current,BrowserItemKind::Use,true});
  if(mode==BrowserMode::Manage&&!g_fileClipboard.path.empty()) items.push_back({std::string("[ Paste ")+(g_fileClipboard.move?"moved":"copied")+" item here ]",current,BrowserItemKind::Paste,true});
  if(mode==BrowserMode::Manage){
    auto favorites=loadFavoriteFolders();
    bool pinned=std::any_of(favorites.begin(),favorites.end(),[&](const std::string &path){ return pathIdentity(path)==pathIdentity(current); });
    items.push_back({pinned?"[ Unpin this folder ]":"[ Pin this folder ]",current,BrowserItemKind::Favorite,true});
  }
  items.push_back({"[ .. locations / parent ]",parentFolder(current),BrowserItemKind::Up,true});
  DIR *dir=opendir(current.c_str());
  if(!dir){ opened=false; return items; }
  std::vector<BrowserItem> entries; struct dirent *entry;
  while((entry=readdir(dir))){
    if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,"..")) continue;
    std::string path=join(current,entry->d_name); bool directory=entry->d_type==DT_DIR;
    if(entry->d_type==DT_UNKNOWN){ struct stat st{}; if(stat(path.c_str(),&st)!=0) continue; directory=S_ISDIR(st.st_mode); }
    if(!directory&&mode==BrowserMode::SelectFolder) continue;
    if(!directory&&mode==BrowserMode::SelectImage){
      const size_t dot=path.find_last_of('.');std::string extension=dot==std::string::npos?std::string{}:path.substr(dot);
      std::transform(extension.begin(),extension.end(),extension.begin(),[](unsigned char value){return (char)std::tolower(value);});
      if(extension!=".png"&&extension!=".jpg"&&extension!=".jpeg"&&extension!=".webp"&&extension!=".bmp")continue;
    }
    entries.push_back({std::string(entry->d_name)+(directory?"/":""),path,directory?BrowserItemKind::Directory:BrowserItemKind::File,directory});
  }
  closedir(dir);
  std::sort(entries.begin(),entries.end(),[](const BrowserItem &left,const BrowserItem &right){
    if(left.directory!=right.directory) return left.directory>right.directory;
    return strcasecmp(left.label.c_str(),right.label.c_str())<0;
  });
  items.insert(items.end(),std::make_move_iterator(entries.begin()),std::make_move_iterator(entries.end()));
  return items;
}

static bool toggleFavorite(const std::string &path) {
  auto favorites=loadFavoriteFolders(); std::string identity=pathIdentity(path);
  auto iterator=std::find_if(favorites.begin(),favorites.end(),[&](const std::string &entry){ return pathIdentity(entry)==identity; });
  bool pinned=iterator==favorites.end();
  if(pinned){
    if(favorites.size()>=24){ toastStatic("Maximum of 24 pinned folders"); return false; }
    ensureSavedPathMountedAtStartup(path);
    favorites.push_back(normalizeLocationPath(path));
  }
  else favorites.erase(iterator);
  saveFavoriteFolders(favorites); toast(pinned?"Folder pinned":"Folder unpinned"); return true;
}

static bool browserActionsAvailable(const BrowserItem &item,BrowserMode mode) {
  if(item.kind!=BrowserItemKind::Directory&&item.kind!=BrowserItemKind::File&&
     item.kind!=BrowserItemKind::Use) return false;
  return mode==BrowserMode::Manage||item.directory;
}

static bool browserActions(const BrowserItem &item,BrowserMode mode) {
  if(!browserActionsAvailable(item,mode)) return false;
  std::vector<std::string> labels;
  const bool installable=mode==BrowserMode::Manage&&pathIsInstallable(item.path,item.directory);
  if(installable) labels.push_back("Install");
  if(mode==BrowserMode::Manage){ labels.insert(labels.end(),{"Copy","Move","Rename"}); }
  bool canPin=item.directory;
  bool pinned=false;
  if(canPin){
    auto favorites=loadFavoriteFolders();
    pinned=std::any_of(favorites.begin(),favorites.end(),[&](const std::string &path){ return pathIdentity(path)==pathIdentity(item.path); });
    labels.push_back(pinned?"Unpin folder":"Pin folder");
  }
  if(labels.empty()) return false;
  std::vector<const char*> choices; for(const auto &label:labels) choices.push_back(label.c_str());
  int action=dropdownStaticTitle("File options",choices.data(),(int)choices.size(),0);
  if(action<0) return false;
  if(installable&&action==0){
    installFromPath(item.path);
    return true;
  }
  const int manageBase=installable?1:0;
  if(mode==BrowserMode::Manage&&action==manageBase){ g_fileClipboard={item.path,false}; toastStatic("Copied to clipboard"); return false; }
  if(mode==BrowserMode::Manage&&action==manageBase+1){ g_fileClipboard={item.path,true}; toastStatic("Move queued"); return false; }
  if(mode==BrowserMode::Manage&&action==manageBase+2){
    char name[256]; std::string oldName=fileNameOf(item.path);
    if(!promptTextStatic("Rename",oldName.c_str(),name,sizeof(name))) return false;
    std::string newName=trim(name);
    if(!validEntryName(newName)){ modalMessageStatic("Invalid name",{"Names cannot contain /, \\, :, or control characters."}); return false; }
    std::string destination=join(parentFolder(item.path),newName); struct stat st{};
    if(lstat(destination.c_str(),&st)==0){ modalMessageStatic("Rename failed",{"An item with that name already exists."}); return false; }
    if(rename(item.path.c_str(),destination.c_str())!=0){ modalMessageStatic("Rename failed",{std::string(strerror(errno))}); return false; }
    replaceSavedPathPrefix(item.path,destination); toastStatic("Renamed"); return true;
  }
  if(canPin) return toggleFavorite(item.path);
  return false;
}

static std::string usbLocationForPath(const std::string &path) {
  if(path.empty()) return {};
  for(const auto &location:SwitchStorage::ListUsbLocations())
    if(pathAtOrBelow(path,normalizeLocationPath(location.path))) return location.id;
  return {};
}

static std::string runFileBrowser(const std::string &start,BrowserMode mode) {
  std::string current=normalizeLocationPath(start);
  if(!current.empty()&&!ensurePathMounted(current)) current.clear();
  int sel=0,top=0;
  for(;;){
    bool opened=false; auto items=browserItems(current,mode,opened);
    if(!opened){ modalMessageStatic("Folder unavailable",{current,"","The device may be disconnected."}); current.clear(); sel=top=0; continue; }
    const int listY=settingsListY(),rowHeight=48;
    int n=(int)items.size(),vis=std::max(1,(SH-listY-settingsFooterReserve())/rowHeight); if(n==0){ current.clear(); continue; }
    sel=std::max(0,std::min(sel,n-1)); if(sel<top)top=sel; if(sel>=top+vis)top=sel-vis+1;
    bool rebuild=false;
    while(!rebuild){
      if(!beginUiFrame()) return {};
      SDL_Event event; navRepeat();
      while(pollUiEvent(event)){
        pumpStick(event); int tx=0,ty=0; TouchKind touch=touchFeed(event,&tx,&ty);
        if(touchScrollList(touch,sel,top,n,vis)) continue;
        if(touch==TOUCH_TAP){
          if(ty>=SH-48){ uiAudioPlay(UiSound::Back); return {}; }
          for(int row=0;row<vis&&top+row<n;row++){ int y=listY+row*rowHeight; if(ty>=y&&ty<y+rowHeight){ sel=top+row; SDL_Event press{}; press.type=SDL_CONTROLLERBUTTONDOWN; press.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&press); break; } }
          continue;
        }
        if(event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
        if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP) sel=(sel+n-1)%n;
        else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN) sel=(sel+1)%n;
        else if(event.cbutton.button==BTN_CANCEL){ if(current.empty()) return {}; current=parentFolder(current); sel=top=0; rebuild=true; }
        else if(event.cbutton.button==BTN_X){ if(browserActions(items[sel],mode)) rebuild=true; }
        else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_X&&mode==BrowserMode::Manage&&!current.empty()&&!g_fileClipboard.path.empty()){ executePaste(current); rebuild=true; }
        else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_START&&mode==BrowserMode::Manage){
          std::string stableId=!items[sel].stableId.empty()?items[sel].stableId:usbLocationForPath(current.empty()?items[sel].path:current);
          if(!stableId.empty()){
            std::string label=items[sel].label;
            for(const auto &location:SwitchStorage::ListUsbLocations()) if(location.id==stableId){ label=location.label; break; }
            if(confirmBoxStatic("Safely eject USB drive?",{label,"Close files using this drive before ejecting."})){
              // A scanner can still own DIR/stat state for this filesystem.
              // Fence it before libusbhsfs unmounts the device, then request a
              // stable-ID reconciliation when returning to the library.
              stopGameScan();
              std::string error;
              if(SwitchStorage::SafelyEjectUsb(stableId,&error)){
                g_rescanAfterSettings=true;
                toastStatic("USB drive can now be removed"); current.clear(); sel=top=0; rebuild=true;
              } else modalMessageStatic("USB eject failed",{error});
            }
            beginScreenFx();
          }
        }
        else if(event.cbutton.button==BTN_CONFIRM){
          const BrowserItem item=items[sel];
          if(item.kind==BrowserItemKind::Use) return item.path;
          if(item.kind==BrowserItemKind::Paste){ executePaste(current); rebuild=true; }
          else if(item.kind==BrowserItemKind::Favorite){ toggleFavorite(current); rebuild=true; }
          else if(item.kind==BrowserItemKind::Up){ current=item.path; sel=top=0; rebuild=true; }
          else if(item.kind==BrowserItemKind::ManageSmb){ networkSharesScreen(); sel=top=0; rebuild=true; }
          else if(item.kind==BrowserItemKind::Directory){ current=item.path; sel=top=0; rebuild=true; }
          else if(item.kind==BrowserItemKind::File&&mode==BrowserMode::SelectImage) return item.path;
          else if(item.kind==BrowserItemKind::Location||item.kind==BrowserItemKind::Smb){
            if(ensurePathMounted(item.path)){ DIR *test=opendir(item.path.c_str()); if(test){ closedir(test); current=item.path; sel=top=0; rebuild=true; } else modalMessageStatic("Location unavailable",{item.path}); }
          }
        }
        if(sel<top) top=sel;
        if(sel>=top+vis) top=sel-vis+1;
      }
      if(rebuild) break;
      clearUiBackground();
      const char *title=mode==BrowserMode::Manage?"File manager":
                        mode==BrowserMode::SelectImage?"Select local cover":"Select game folder";
      drawHeader(LauncherLocalization::Translate(title).data(),
                 current.empty()?LauncherLocalization::Translate("Locations").data():current.c_str());
      glassPanel(42,listY-12,SW-84,std::min(vis,std::max(1,n-top))*rowHeight+18);
      for(int row=0;row<vis&&top+row<n;row++){
        const int index=top+row,y=listY+row*rowHeight; const bool selected=index==sel; const auto &item=items[index];
        if(selected){ drawRowHighlight(54,y,SW-108,rowHeight-4); }
        SDL_Color color=item.kind==BrowserItemKind::Use||item.kind==BrowserItemKind::Paste||item.kind==BrowserItemKind::Favorite?COL_HI:(item.directory?COL_TXT:(SDL_Color){120,220,120,255});
        const int nameY=y+(rowHeight-fontHeight(g_font))/2;
        if(selected) drawScrollTextL(g_font,80,nameY,SW-180,item.label.c_str(),COL_VAL);
        else drawText(g_font,80,nameY,ellipsizedText(g_font,item.label,SW-180).c_str(),color);
      }
      const bool canEject=mode==BrowserMode::Manage&&
        !usbLocationForPath(current.empty()?items[sel].path:current).empty();
      const bool canAct=browserActionsAvailable(items[sel],mode);
      std::string footer;
      if(mode==BrowserMode::Manage){
        if(items[sel].kind!=BrowserItemKind::File) footer="A  Open       ";
        if(canAct) footer+="X  Actions       ";
        if(!current.empty()&&!g_fileClipboard.path.empty()) footer+="Y  Paste       ";
        if(canEject) footer+="+  Eject       ";
      } else {
        footer="A  Open / Select       ";
        if(canAct) footer+="X  Pin       ";
      }
      footer+="B  Back";
      drawLocalizedFooter(footer.c_str());
    SDL_RenderPresent(g_ren); waitForNextUiFrame();
    }
  }
}

static std::string browseFolder(const std::string &start) {
  return runFileBrowser(start,BrowserMode::SelectFolder);
}

static std::string browseCoverImage(const std::string &start) {
  return runFileBrowser(start,BrowserMode::SelectImage);
}

static void runFileManager() {
  runFileBrowser({},BrowserMode::Manage);
}

static bool folderIsExtractedTitle(const std::string &dir) {
  struct stat st;
  if (stat((dir + "/code/app.xml").c_str(), &st) == 0) return true;
  bool c = stat((dir+"/content").c_str(),&st)==0 && S_ISDIR(st.st_mode);
  bool o = stat((dir+"/code").c_str(),&st)==0 && S_ISDIR(st.st_mode);
  bool m = stat((dir+"/meta").c_str(),&st)==0 && S_ISDIR(st.st_mode);
  return c && o && m;
}
static int choiceIdx(const Opt &o) {
  const char *cur = iniGet(o.key, o.def);
  for (int i=0;i<o.nch;i++) if (!strcmp(o.ch[i].val, cur)) return i;
  return -1;
}
static void optValue(const Opt &o, char *out, int n) {
  out[0]=0;
  if (o.type==OT_CHOICE){
    if(o.key&&!strcmp(o.key,"console_language")&&!strcmp(iniGet(o.key,o.def),"-1"))
      snprintf(out,n,"Auto (%s)",g_systemLanguageName);
    else { int i=choiceIdx(o); const char* value=i>=0?o.ch[i].label:iniGet(o.key,o.def);
           snprintf(out,n,"%s",LauncherLocalization::Translate(value).data()); }
  }
  else if (o.type==OT_RANGE){
    if (o.key && !strcmp(o.key,"AudioDelay")) snprintf(out,n,"%d ms", atoi(iniGet(o.key,o.def))*12);
    else if(o.key&&!strcmp(o.key,"in_deadzone")) snprintf(out,n,"%s%%",iniGet(o.key,o.def));
    else snprintf(out,n,"%s", iniGet(o.key,o.def));
  }
  else if (o.type==OT_ACTION) snprintf(out,n,">");
  else if (o.type==OT_STATUS){
    if(o.key&&!strcmp(o.key,"cpu-cores")) snprintf(out,n,"%d / 4",allowedCpuCores());
    else snprintf(out,n,"%s",regularFileExists(LSFG_DLL_FILE)?"Installed":"Missing");
  }
}
static void optAdjust(const Opt &o, int dir) {
  if (o.type==OT_CHOICE){ int i=choiceIdx(o); if(i<0)i=0; i=(i+dir+o.nch)%o.nch; iniSet(o.key,o.ch[i].val); }
  else if (o.type==OT_RANGE){ int v=atoi(iniGet(o.key,o.def))+dir*o.step; if(v<o.lo)v=o.lo; if(v>o.hi)v=o.hi; char b[24]; snprintf(b,sizeof(b),"%d",v); iniSet(o.key,b); }
}

static bool canResetOption(const Opt &option) {
  return option.key&&option.def&&option.type!=OT_ACTION&&option.type!=OT_STATUS;
}
static bool canAdjustOption(const Opt &option) {
  return option.key&&(option.type==OT_CHOICE||option.type==OT_RANGE);
}
static bool resetOption(const Opt &option) {
  if(!canResetOption(option)) return false;
  if(g_active==&g_global) storeSet(g_global,option.key,option.def);
  else storeRemove(*g_active,option.key);
  return true;
}

static bool optionEnabled(int screen,const Opt &option) {
  if(option.type==OT_STATUS) return false;
  const bool nativeVulkan=!strcmp(iniGet("Wrapper/Renderer","vk"),"vk");
  if(!nativeVulkan && screen==SCR_FRAMEGEN) return false;
  if(!nativeVulkan && option.key && !strcmp(option.key,"vkAccurateBarriers")) return false;
  if(screen==SCR_FRAMEGEN&&option.key&&
     (!strcmp(option.key,"Wrapper/LSFGFlowScale")||!strcmp(option.key,"Wrapper/LSFGPerformance")))
    return !strcmp(iniGet("Wrapper/LSFGEnabled","false"),"true")&&regularFileExists(LSFG_DLL_FILE);
  return true;
}


static float g_hy = -1;
static void beginScreenFx(){ g_fxT=SDL_GetTicks(); g_redrawUntil=g_fxT+220; g_hy=-1;
                             g_textScroll.clear(); g_footN=0;
                             g_libraryTriggers[0]=g_libraryTriggers[1]=false; }
static void drawFadeIn(){
  if(!g_uiAnimations) return;
  const int D = 160; int el = (int)(SDL_GetTicks() - g_fxT);
  if (el < D) fillRect(0,0,SW,SH,(SDL_Color){0,0,0,(Uint8)(200*(D-el)/D)});
}
// Top-bar height, shared by the grid header (gridLayout y0) and every page header.
static int topBarH(){ return 80; }
static void drawScrollTextR(TTF_Font*f,int xRight,int y,int maxW,const char*s,SDL_Color c);
static void drawScrollTextL(TTF_Font*f,int x,int y,int maxW,const char*s,SDL_Color c);
static void drawPageHeader(const char *title,const char *eyebrow,const char *summary,const char *detail=nullptr){
  const int height=topBarH(),margin=40,left=margin;
  drawGradient({0,0,SW,height+48},{0,0,0,125},{0,0,0,0});
  const int metadataWidth=std::min(SW/3,std::max(textW(g_font_sm,summary),textW(g_font_sm,detail)));
  const int titleWidth=SW-left-margin-(metadataWidth?metadataWidth+32:0);
  drawText(g_font_caption,left,9,fittedText(g_font_caption,eyebrow?eyebrow:"",titleWidth).c_str(),COL_DIM);
  drawScrollTextL(g_font_big,left,31,titleWidth,title?title:"",COL_TXT);
  if(summary) drawTextR(g_font_sm,SW-margin,16,fittedText(g_font_sm,summary,metadataWidth).c_str(),COL_TXT);
  if(detail) drawTextR(g_font_sm,SW-margin,44,fittedText(g_font_sm,detail,metadataWidth).c_str(),COL_DIM);
}
static void drawHeader(const char *title, const char *ctx){
  drawPageHeader(title,"Cemu",ctx);
}
// Labels a group of rows inside a page.
static void drawSectionHeading(const char *title,int x,int y,int width){
  const std::string shown=fittedText(g_font_sm,LauncherLocalization::Translate(title).data(),width-24);
  const int height=fontHeight(g_font_sm),textWidth=textW(g_font_sm,shown.c_str());
  fillRect(x,y+4,3,height-8,COL_SEL);
  drawText(g_font_sm,x+14,y,shown.c_str(),COL_HI);
  fillRect(x+textWidth+30,y+height/2,width-textWidth-30,1,(SDL_Color){255,255,255,28});
}
static int settingsRowH(){ return 44; }
static int settingsListY(){ return topBarH()+24; }
static int settingsFooterReserve(){ return 80; }
#define ROW_H (settingsRowH())
#define LIST_Y0 (settingsListY())
static void listCol(int *colX,int *colW,int *labelX,int *valX){
  int w = SW-180; if (w>980) w=980;
  *colW=w; *colX=(SW-w)/2; *labelX=*colX+40; *valX=*colX+w-40;
}
static int listVis(){ int v=(SH-LIST_Y0-settingsFooterReserve())/ROW_H; return v<1?1:v; }

static void drawSettingsRowText(const char *label,const char *value,
                                int slotY,int colW,int labelX,int valX,
                                bool current,SDL_Color labelColor,
                                SDL_Color valueColor,bool scrollValue=false,
                                int rowHeight=0){
  const int actualRowHeight=rowHeight>0?rowHeight:settingsRowH();
  if(!current)fillRect(labelX,slotY+actualRowHeight-1,valX-labelX,1,(SDL_Color){255,255,255,10});
  const bool submenu=value&&strcmp(value,">")==0;
  const int valueWidth=submenu?16:std::min(textW(g_font_sm,value),colW/2-32);
  const int labelWidth=std::max(40,valX-labelX-valueWidth-24);
  const int y=slotY+(actualRowHeight-fontHeight(g_font))/2;
  if(current)drawScrollTextL(g_font,labelX,y,labelWidth,label,labelColor);
  else drawText(g_font,labelX,y,fittedText(g_font,label?label:"",labelWidth).c_str(),labelColor);
  if(submenu){
    const int cy=slotY+actualRowHeight/2;
    SDL_SetRenderDrawColor(g_ren,valueColor.r,valueColor.g,valueColor.b,valueColor.a);
    SDL_RenderDrawLine(g_ren,valX-10,cy-6,valX-4,cy);
    SDL_RenderDrawLine(g_ren,valX-4,cy,valX-10,cy+6);
  }else{
    const int valueY=slotY+(actualRowHeight-fontHeight(g_font_sm))/2;
    if(scrollValue)drawScrollTextR(g_font_sm,valX,valueY,colW/2-32,value,valueColor);
    else drawTextR(g_font_sm,valX,valueY,fittedText(g_font_sm,value?value:"",colW/2-32).c_str(),valueColor);
  }
}

static void showHelpCard(const char *section,const char *title,const char *kind,
                         const std::string &description,const char *current,
                         const char *scope) {
  for(;;){
    if(!beginUiFrame()) return;
    SDL_Event event;
    while(pollUiEvent(event)){
      pumpStick(event);
      int touchX=0,touchY=0;
      if(touchFeed(event,&touchX,&touchY)==TOUCH_TAP) return;
      if(event.type==SDL_CONTROLLERBUTTONDOWN &&
         (event.cbutton.button==BTN_CONFIRM ||
          event.cbutton.button==BTN_CANCEL ||
          event.cbutton.button==BTN_X)) return;
    }

    clearUiBackground();
    const int panelWidth=std::min(SW-120,1000);
    const int panelHeight=std::min(SH-96,500);
    const int panelX=(SW-panelWidth)/2,panelY=(SH-panelHeight)/2;
    glassPanel(panelX,panelY,panelWidth,panelHeight);
    drawText(g_font_caption,panelX+40,panelY+22,section&&*section?section:
             LauncherLocalization::Translate("Settings").data(),COL_DIM);
    const char *helpTitle=title&&*title?title:LauncherLocalization::Translate("Setting info").data();
    drawText(g_font_big,panelX+40,panelY+44,
             fittedText(g_font_big,helpTitle,panelWidth-80).c_str(),COL_VAL);

    std::string metadata=kind&&*kind?std::string(kind):
                         std::string(LauncherLocalization::Translate("Setting"));
    if(scope&&*scope){ metadata+="  |  "; metadata+=scope; }
    drawText(g_font_sm,panelX+40,panelY+114,
             ellipsizedText(g_font_sm,metadata,panelWidth-80).c_str(),COL_SEL);
    int bodyY=panelY+164;
    if(current&&*current){
      const char *prefix=LauncherLocalization::Translate("Current: ").data();
      drawText(g_font_sm,panelX+40,panelY+146,prefix,COL_DIM);
      drawScrollTextL(g_font_sm,panelX+40+textW(g_font_sm,prefix),panelY+146,
                      panelWidth-80-textW(g_font_sm,prefix),current,COL_TXT);
      bodyY=panelY+198;
    }
    fillRect(panelX+40,bodyY-18,panelWidth-80,2,(SDL_Color){70,78,92,210});
    drawWrapped(g_font,panelX+40,bodyY,panelWidth-80,32,7,description.c_str(),COL_TXT);
    drawLocalizedFooter("A  Close",panelY+panelHeight-48);
    drawStaticTextC(g_font_sm,SW/2,panelY+panelHeight-22,"Touch anywhere to close",COL_DIM);
    SDL_RenderPresent(g_ren);
    waitForNextUiFrame();
  }
}

static void showOptionHelp(const char *section,const Opt &option,const char *scope) {
  SettingHelpInfo help=settingHelpFor(option);
  char value[256]={};
  const char *current=nullptr;
  if(option.type!=OT_ACTION){ optValue(option,value,sizeof(value)); current=value; }
  showHelpCard(LauncherLocalization::Translate(section).data(),
               LauncherLocalization::Translate(option.label).data(),
               LauncherLocalization::Translate(help.kind).data(),
               std::string(LauncherLocalization::Translate(help.text)),current,
               LauncherLocalization::Translate(scope).data());
}

static const char *settingsScreenDescription(int screen) {
  switch(screen){
    case SCR_CPU: return "Controls Cemu's CPU execution mode, emulated timer rate, and hardware video decoding. Compatibility overrides should normally be applied per game.";
    case SCR_GRAPHICS: return "Controls Vulkan presentation, shader compilation, synchronization, image scaling, and how the Wii U TV and GamePad screens are arranged.";
    case SCR_FRAMEGEN: return "Configures Vulkan-only LSFG 2x frame generation. It creates intermediate display frames but does not make Wii U emulation run faster.";
    case SCR_AUDIO: return "Controls the Wii U TV and GamePad audio streams, their volume, and the balance between output latency and stability.";
    case SCR_OVERLAY: return "Controls Cemu's in-game FPS and shader-compilation status displays and where the performance overlay appears.";
    case SCR_INPUT: return "Selects the emulated Wii U controller, local player count, rumble, stick dead zone, press-to-bind controller mappings, and Cemu's virtual USB portals for Skylanders, Disney Infinity, and LEGO Dimensions.";
    default: return "Opens this group of Cemu settings.";
  }
}

static void renderSettings(int scr,int sel,int top,const char *ctx){
  clearUiBackground();
  const Screen &S=g_screens[scr];
  drawHeader(LauncherLocalization::Translate(S.title).data(), ctx);
  int colX,colW,labelX,valX; listCol(&colX,&colW,&labelX,&valX);
  int vis=listVis();
  glassPanel(colX-12,LIST_Y0-10,colW+24,std::min(vis,std::max(1,S.n-top))*ROW_H+18);
  float ty = (float)(LIST_Y0 + (sel-top)*ROW_H + 1);
  g_hy = (!g_uiAnimations||g_hy<0) ? ty : g_hy + (ty-g_hy)*0.30f;
  drawRowHighlight(colX,(int)g_hy,colW,ROW_H-2);
  for(int r=0;r<vis && top+r<S.n;r++){
    int i=top+r,slotY=LIST_Y0+r*ROW_H; bool cur=(i==sel);
    const bool enabled=optionEnabled(scr,S.opts[i]);
    SDL_Color lc = enabled?(cur?COL_VAL:COL_TXT):(SDL_Color){92,98,110,255};
    SDL_Color vc = enabled?(cur?COL_VAL:COL_DIM):(SDL_Color){92,98,110,255};
    if(S.opts[i].type==OT_STATUS){
      const bool ok=(S.opts[i].key&&!strcmp(S.opts[i].key,"cpu-cores"))
        ? allowedCpuCores()>=4 : regularFileExists(LSFG_DLL_FILE);
      vc=ok?(SDL_Color){120,220,120,255}:(SDL_Color){235,125,115,255};
    }
    char v[96]; optValue(S.opts[i],v,sizeof(v));
    drawSettingsRowText(LauncherLocalization::Translate(S.opts[i].label).data(),v,
                        slotY,colW,labelX,valX,cur,lc,vc);
  }
  if(S.n>vis){
    int trH=vis*ROW_H, trX=colX+colW+16, trY=LIST_Y0-2;
    fillRect(trX,trY,4,trH,(SDL_Color){40,44,54,255});
    int thH=trH*vis/S.n, denom=(S.n-vis>0?S.n-vis:1);
    fillRect(trX,trY+(trH-thH)*top/denom,4,thH,COL_SEL);
  }
  std::string controls;
  if(sel>=0&&sel<S.n){
    const Opt &row=S.opts[sel];
    const bool live=optionEnabled(scr,row);
    if(live&&canAdjustOption(row)) controls+="Left / Right  Change       ";
    if(live&&(canAdjustOption(row)||row.type==OT_ACTION)) controls+="A  Choose       ";
    if(canResetOption(row)) controls+="Y  Reset       ";
  }
  controls+="X  Info       B  Back";
  drawLocalizedFooter(controls.c_str());
  drawFadeIn();
  SDL_RenderPresent(g_ren);
}

static int dropdown(const char *title, const char *const *labels, int n, int cur) {
  if(n<1) return -1;
  int sel = (cur < 0 || cur >= n) ? 0 : cur, top = 0;
  const int rowH = settingsRowH()+8;
  const int vis = std::min(n,std::max(1,(SH-240)/rowH));
  const int pw = std::min(900,SW-64), ph = 140+vis*rowH;
  const int px = (SW-pw)/2, py = (SH-ph)/2, ly = py+80;
  beginScreenFx();
  for (;;) {
    if (!beginUiFrame()) return -1;
    SDL_Event e;
    navRepeat();
    while (pollUiEvent(e)) {
      pumpStick(e);
      { int tx=0,ty=0; TouchKind tk=touchFeed(e,&tx,&ty);
        if(touchScrollList(tk,sel,top,n,vis)) continue;
        if(tk==TOUCH_TAP&&tx>=px+8&&tx<px+pw-8){
          for(int r=0;r<vis&&top+r<n;r++){ int y=ly+r*rowH; if(ty>=y&&ty<y+rowH) return top+r; }
        } }
      if (e.type != SDL_CONTROLLERBUTTONDOWN) continue;
      switch (e.cbutton.button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:   sel=(sel+n-1)%n; break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: sel=(sel+1)%n;   break;
        case BTN_CONFIRM: return sel;
        case BTN_CANCEL:  return -1;
      }
      if(sel<top) top=sel;
      if(sel>=top+vis) top=sel-vis+1;
      if(top<0) top=0;
    }
    clearUiBackground();
    glassPanel(px,py,pw,ph);
    drawScrollTextL(g_font_big,px+28,py+22,pw-56,title?title:"",COL_TXT);
    fillRect(px+28,py+66,pw-56,1,(SDL_Color){255,255,255,24});
    for(int r=0;r<vis && top+r<n;r++){
      const int i=top+r, y=ly+r*rowH; const bool curr=(i==sel);
      if(curr) drawRowHighlight(px+8,y,pw-16,rowH-4);
      const int textY=y+(rowH-fontHeight(g_font))/2;
      if(curr) drawScrollTextL(g_font,px+32,textY,pw-76,labels[i],COL_VAL);
      else drawText(g_font,px+32,textY,fittedText(g_font,labels[i]?labels[i]:"",pw-76).c_str(),COL_TXT);
    }
    if(n>vis){ const int height=vis*rowH,thumb=std::max(12,height*vis/n);
      fillRect(px+pw-18,ly,3,height,COL_CARD);
      fillRect(px+pw-18,ly+(height-thumb)*top/(n-vis),3,thumb,COL_SEL); }
    FootItem footer[]={{"A",LauncherLocalization::Translate("Select").data()},
                       {"B",LauncherLocalization::Translate("Back").data()}};
    drawFooterHints(footer,2,py+ph-28);
    drawFadeIn();
    SDL_RenderPresent(g_ren);
    waitForNextUiFrame();
  }
}
static void optChoosePopup(const Opt &o) {
  if(o.type!=OT_CHOICE || o.nch<=0) return;
  const char* labels[32]; int n = o.nch>32?32:o.nch;
  std::string automaticLanguage;
  for(int i=0;i<n;i++){
    if(i==0&&o.key&&!strcmp(o.key,"console_language")&&!strcmp(o.ch[i].val,"-1")){
      automaticLanguage="Auto ("+std::string(g_systemLanguageName)+")";
      labels[i]=automaticLanguage.c_str();
    } else labels[i]=o.ch[i].label;
  }
  int idx = dropdown(o.label, labels, n, choiceIdx(o));
  if(idx>=0 && idx<o.nch) iniSet(o.key, o.ch[idx].val);
}

static const char *mappingTokenForButton(Uint8 button) {
  switch (button) {
    case SDL_CONTROLLER_BUTTON_B: return "A";
    case SDL_CONTROLLER_BUTTON_A: return "B";
    case SDL_CONTROLLER_BUTTON_Y: return "X";
    case SDL_CONTROLLER_BUTTON_X: return "Y";
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return "L";
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return "R";
    case SDL_CONTROLLER_BUTTON_START: return "PLUS";
    case SDL_CONTROLLER_BUTTON_BACK: return "MINUS";
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return "DUP";
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return "DDOWN";
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return "DLEFT";
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return "DRIGHT";
    case SDL_CONTROLLER_BUTTON_LEFTSTICK: return "LCLICK";
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return "RCLICK";
    default: return nullptr;
  }
}

static const char *mappingTokenLabel(const char *token) {
  if (!strcmp(token,"PLUS")) return "Plus";
  if (!strcmp(token,"MINUS")) return "Minus";
  if (!strcmp(token,"DUP")) return "D-Pad Up";
  if (!strcmp(token,"DDOWN")) return "D-Pad Down";
  if (!strcmp(token,"DLEFT")) return "D-Pad Left";
  if (!strcmp(token,"DRIGHT")) return "D-Pad Right";
  if (!strcmp(token,"LCLICK")) return "L-Stick click";
  if (!strcmp(token,"RCLICK")) return "R-Stick click";
  if (!strcmp(token,"NONE")) return "Unmapped";
  return token;
}

static bool mappingInputHeld() {
  if (!g_pad || !SDL_GameControllerGetAttached(g_pad)) return false;
  SDL_GameControllerUpdate();
  for (int button=0; button<SDL_CONTROLLER_BUTTON_MAX; button++)
    if (SDL_GameControllerGetButton(g_pad,(SDL_GameControllerButton)button)) return true;
  return SDL_GameControllerGetAxis(g_pad,SDL_CONTROLLER_AXIS_TRIGGERLEFT)>12000 ||
         SDL_GameControllerGetAxis(g_pad,SDL_CONTROLLER_AXIS_TRIGGERRIGHT)>12000;
}

static void renderMappingPrompt(const InputMapping &mapping,int index,bool releasing,const char *status) {
  clearUiBackground();
  drawHeaderStatic("Control mapping", "Press-to-bind");
  int panelW=std::min(900,SW-180),panelH=330,panelX=(SW-panelW)/2,panelY=(SH-panelH)/2-20;
  glassPanel(panelX,panelY,panelW,panelH);
  char step[48]; snprintf(step,sizeof(step),"Control %d of %d",index+1,(int)(sizeof(C_inputMappings)/sizeof(*C_inputMappings)));
  drawTextC(g_font_sm,SW/2,panelY+36,step,COL_DIM);
  drawTextC(g_font_big,SW/2,panelY+94,mapping.label,COL_VAL);
  if(releasing) drawStaticTextC(g_font,SW/2,panelY+180,"Release the current button",COL_TXT);
  else drawStaticTextC(g_font,SW/2,panelY+180,"Press the button or trigger to assign",COL_TXT);
  const char *current=iniGet(mapping.key,mapping.def);
  std::string currentLine=std::string("Current: ")+mappingTokenLabel(current);
  drawTextC(g_font_sm,SW/2,panelY+238,status&&*status?status:currentLine.c_str(),status&&*status?COL_HI:COL_DIM);
  {
    const char *clearLabel=LauncherLocalization::Translate("Hold to clear").data();
    const char *cancelLabel=LauncherLocalization::Translate("Hold to cancel").data();
    int plusW=0,plusH=0,minusW=0,minusH=0;
    buttonHintSize("+",plusW,plusH);
    buttonHintSize("-",minusW,minusH);
    const int hintGap=48;
    const int total=plusW+8+textW(g_font_sm,clearLabel)+hintGap+
                    minusW+8+textW(g_font_sm,cancelLabel);
    int hintX=(SW-total)/2;
    hintX+=drawButtonHint(hintX,SH-72,"+",clearLabel);
    drawButtonHint(hintX+hintGap,SH-72,"-",cancelLabel);
  }
  drawStaticTextC(g_font_sm,SW/2,SH-38,"Touch left to clear       Touch right to cancel",COL_DIM);
  SDL_RenderPresent(g_ren);
}

static bool captureMappingInput(const InputMapping &mapping,int index,std::string &token) {
  g_footN=0;
  bool releasing=true;
  while (mappingInputHeld()) {
    if (!beginUiFrame()) return false;
    SDL_Event event; while (pollUiEvent(event)) { int x=0,y=0; if(touchFeed(event,&x,&y)==TOUCH_TAP&&y>=SH-92) return false; }
    renderMappingPrompt(mapping,index,true,nullptr);
    waitForNextUiFrame();
  }
  releasing=false;
  int heldSpecial=-1;
  Uint32 heldSince=0;
  for (;;) {
    if (!beginUiFrame()) return false;
    SDL_Event event;
    while (pollUiEvent(event)) {
      int x=0,y=0;
      if (touchFeed(event,&x,&y)==TOUCH_TAP&&y>=SH-92) {
        if (x<SW/2) { token="NONE"; return true; }
        return false;
      }
      if (!g_pad || !SDL_GameControllerGetAttached(g_pad)) return false;
      if (event.type==SDL_CONTROLLERBUTTONDOWN) {
        const char *captured=mappingTokenForButton(event.cbutton.button);
        if (!captured) continue;
        if (event.cbutton.button==SDL_CONTROLLER_BUTTON_START || event.cbutton.button==SDL_CONTROLLER_BUTTON_BACK) {
          heldSpecial=event.cbutton.button;
          heldSince=SDL_GetTicks();
        } else {
          token=captured;
          return true;
        }
      } else if (event.type==SDL_CONTROLLERBUTTONUP && event.cbutton.button==heldSpecial) {
        token=mappingTokenForButton((Uint8)heldSpecial);
        return true;
      } else if (event.type==SDL_CONTROLLERAXISMOTION && event.caxis.value>16000) {
        if (event.caxis.axis==SDL_CONTROLLER_AXIS_TRIGGERLEFT) { token="ZL"; return true; }
        if (event.caxis.axis==SDL_CONTROLLER_AXIS_TRIGGERRIGHT) { token="ZR"; return true; }
      }
    }
    const Uint32 heldFor=heldSpecial<0?0:SDL_GetTicks()-heldSince;
    if (heldFor>=800) {
      if (heldSpecial==SDL_CONTROLLER_BUTTON_START) { token="NONE"; return true; }
      return false;
    }
    const char *status=heldSpecial==SDL_CONTROLLER_BUTTON_START?"Keep holding Plus to clear":"";
    if (heldSpecial==SDL_CONTROLLER_BUTTON_BACK) status="Keep holding Minus to cancel";
    renderMappingPrompt(mapping,index,releasing,status);
    waitForNextUiFrame();
  }
}

static void runInputMappingScreen() {
  const int count=(int)(sizeof(C_inputMappings)/sizeof(*C_inputMappings));
  static int savedSelection=0;
  int sel=std::max(0,std::min(savedSelection,count-1));
  int top=std::max(0,sel-std::min(listVis(),count)+1);

  auto assignSelected=[&]() {
    if (!g_pad || !SDL_GameControllerGetAttached(g_pad)) {
      modalMessageStatic("Control mapping", {"No controller is connected."});
      beginScreenFx();
      return;
    }
    std::string token;
    if (captureMappingInput(C_inputMappings[sel],sel,token)) {
      iniSet(C_inputMappings[sel].key,token.c_str());
      toastStatic("Control assigned");
    }
    beginScreenFx();
  };

  beginScreenFx();
  for (;;) {
    if (!beginUiFrame()) { savedSelection=sel; return; }
    SDL_Event event;
    navRepeat();
    while (pollUiEvent(event)) {
      pumpStick(event);
      int tx=0,ty=0;
      const TouchKind touch=touchFeed(event,&tx,&ty);
      const int visible=std::min(listVis(),count);
      if (touchScrollList(touch,sel,top,count,visible)) continue;
      if (touch==TOUCH_TAP) {
        if (ty<topBarH() || ty>=SH-40) { savedSelection=sel; return; }
        int colX,colW,labelX,valX;
        listCol(&colX,&colW,&labelX,&valX);
        for (int row=0; row<visible && top+row<count; ++row) {
          const int y=LIST_Y0+row*ROW_H;
          if (tx>=colX && tx<colX+colW && ty>=y && ty<y+ROW_H) {
            sel=top+row;
            assignSelected();
            break;
          }
        }
        continue;
      }
      if (event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      if (event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP) sel=(sel+count-1)%count;
      else if (event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN) sel=(sel+1)%count;
      else if (event.cbutton.button==BTN_CONFIRM) assignSelected();
      else if (event.cbutton.button==SDL_CONTROLLER_BUTTON_X) {
        iniSet(C_inputMappings[sel].key,C_inputMappings[sel].def);
        toast(LauncherLocalization::Translate("Setting reset to default").data());
      }
      else if (event.cbutton.button==BTN_CANCEL) { savedSelection=sel; return; }

      if (sel<top) top=sel;
      if (sel>=top+visible) top=sel-visible+1;
      if (top<0) top=0;
    }

    clearUiBackground();
    drawHeaderStatic("Control mapping","Select a control");
    int colX,colW,labelX,valX;
    listCol(&colX,&colW,&labelX,&valX);
    const int visible=std::min(listVis(),count);
    glassPanel(colX-12,LIST_Y0-10,colW+24,std::min(visible,std::max(1,count-top))*ROW_H+18);
    const float target=(float)(LIST_Y0+(sel-top)*ROW_H+1);
    g_hy=(!g_uiAnimations||g_hy<0)?target:g_hy+(target-g_hy)*0.30f;
    drawRowHighlight(colX,(int)g_hy,colW,ROW_H-2);
    for (int row=0; row<visible && top+row<count; ++row) {
      const int index=top+row;
      const int slotY=LIST_Y0+row*ROW_H;
      const bool current=index==sel;
      drawSettingsRowText(C_inputMappings[index].label,
                          mappingTokenLabel(iniGet(C_inputMappings[index].key,C_inputMappings[index].def)),
                          slotY,colW,labelX,valX,current,
                          current?COL_VAL:COL_TXT,current?COL_VAL:COL_DIM);
    }
    if (count>visible) {
      const int trackHeight=visible*ROW_H;
      const int trackX=colX+colW+16;
      const int trackY=LIST_Y0-2;
      fillRect(trackX,trackY,4,trackHeight,(SDL_Color){40,44,54,255});
      const int thumbHeight=trackHeight*visible/count;
      const int denominator=std::max(1,count-visible);
      fillRect(trackX,trackY+(trackHeight-thumbHeight)*top/denominator,4,thumbHeight,COL_SEL);
    }
    drawLocalizedFooter("A  Assign       Y  Reset       B  Back");
    drawFadeIn();
    SDL_RenderPresent(g_ren);
    waitForNextUiFrame();
  }
}

static int s_setSel[SCR_COUNT]={0}, s_setTop[SCR_COUNT]={0};
static void runSettings(int scr, const char *ctx) {
  const Screen &S=g_screens[scr];
  int sel=s_setSel[scr], top=s_setTop[scr];
  if(sel<0||sel>=S.n) sel=0;
  if(top<0||top>=S.n) top=0;
  auto nav=[&](int dir){ sel=(sel+dir+S.n)%S.n; };
  auto adjust=[&](const Opt &option,int direction){
    if(!optionEnabled(scr,option)) return;
    optAdjust(option,direction);
    if(scr==SCR_FRAMEGEN && option.key && !strcmp(option.key,"Wrapper/LSFGEnabled") &&
       !strcmp(iniGet(option.key,option.def),"true") && !regularFileExists(LSFG_DLL_FILE)){
      iniSet(option.key,"false");
      modalMessageStatic("LSFG is not ready", {
        "Lossless.dll was not found.",
        "Copy it to sdmc:/switch/cemu/lsfg/Lossless.dll",
        "then enable LSFG again."
      });
      beginScreenFx();
    }
  };
  beginScreenFx();
  for(;;){
    if (!beginUiFrame()) return;
    SDL_Event e;
    navRepeat();
    while(pollUiEvent(e)){
      pumpStick(e);
      { int tx=0,ty=0; TouchKind tk=touchFeed(e,&tx,&ty);
        int visible=listVis();
        if(touchScrollList(tk,sel,top,S.n,visible)){ s_setSel[scr]=sel; s_setTop[scr]=top; continue; }
        if(tk==TOUCH_SWIPE_L){ adjust(S.opts[sel],-1); continue; }
        if(tk==TOUCH_SWIPE_R){ adjust(S.opts[sel],+1); continue; }
        if(tk==TOUCH_TAP){
          if(ty<topBarH() || ty>=SH-40){ return; }
          int colX,colW,labelX,valX; listCol(&colX,&colW,&labelX,&valX); int vis=listVis();
          for(int r=0;r<vis && top+r<S.n;r++){ int y=LIST_Y0+r*ROW_H;
            if(ty>=y && ty<y+ROW_H){ int ni=top+r; sel=ni;
              if(tx>=colX+colW/2){ SDL_Event a; memset(&a,0,sizeof(a)); a.type=SDL_CONTROLLERBUTTONDOWN; a.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&a); }
              break; } }
          continue;
        }
      }
      if(e.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      switch(e.cbutton.button){
        case SDL_CONTROLLER_BUTTON_DPAD_UP:   nav(-1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: nav(+1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  adjust(S.opts[sel],-1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: adjust(S.opts[sel], 1); break;
        case BTN_CONFIRM: {
          const Opt &o=S.opts[sel];
          if(o.type==OT_ACTION && scr==SCR_INPUT){ runInputMappingScreen(); beginScreenFx(); }
          else if(o.type==OT_CHOICE && o.nch>2){ optChoosePopup(o); beginScreenFx(); }
          else adjust(o,1);
          break;
        }
        case BTN_X:
          showOptionHelp(S.title,S.opts[sel],ctx&&*ctx?"Per-game setting":"Global setting");
          beginScreenFx();
          break;
        case SDL_CONTROLLER_BUTTON_X:
          if(resetOption(S.opts[sel])) { toast(LauncherLocalization::Translate("Setting reset to default").data()); }
          break;
        case BTN_CANCEL: return;
      }
      int vis=listVis(); if(sel<top) top=sel; if(sel>=top+vis) top=sel-vis+1; if(top<0)top=0;
      s_setSel[scr]=sel; s_setTop[scr]=top;
    }
    renderSettings(scr,sel,top,ctx);
    waitForNextUiFrame();
  }
}

static std::string launcherUpdateStatusText() {
  const LauncherUpdateSnapshot snapshot=LauncherUpdate_GetSnapshot();
  switch(snapshot.state){
    case LauncherUpdateState::Checking: return "Checking...";
    case LauncherUpdateState::UpdateAvailable: return snapshot.release.tag+" available";
    case LauncherUpdateState::UpToDate: return "Up to date";
    case LauncherUpdateState::Downloading: {
      const uint64_t total=snapshot.total?snapshot.total:snapshot.release.assetSize;
      const uint64_t percent=total?std::min<uint64_t>(100,snapshot.downloaded*100/total):0;
      return "Downloading "+std::to_string(percent)+"%";
    }
    case LauncherUpdateState::ReadyToInstall: return "Ready to install";
    case LauncherUpdateState::Installing: return "Installing...";
    case LauncherUpdateState::Installed: return "Ready to exit";
    case LauncherUpdateState::Cancelled: return "Cancelled";
    case LauncherUpdateState::Error: return "Check failed";
    case LauncherUpdateState::Idle: break;
  }
  return std::string("Installed ")+installedReleaseTag();
}

static std::string steamGridDbKey(){
  return trim(storeGet(g_global,"Wrapper/SteamGridDBKey",""));
}

static bool promptAndSaveSteamGridDbKey(const char *header,bool allowEmpty){
  const std::string current=steamGridDbKey();
  char value[256];
  if(!promptTextMode(header,current.c_str(),value,sizeof(value),true,allowEmpty,
                     "Used for cover and shortcut artwork downloads.",
                     allowEmpty?"Leave blank to remove the saved key":"Enter a valid key to continue"))
    return false;
  const std::string normalized=trim(value);
  if(!allowEmpty&&normalized.empty()) return false;
  storeSet(g_global,"Wrapper/SteamGridDBKey",normalized.c_str());
  storeSave(g_global,LAUNCHER_INI);
  return true;
}

static void launcherSettingsScreen() {
  static int savedSelection=0,savedTop=0;
  const int optionCount=(int)(sizeof(S_launcher)/sizeof(Opt));
  const int apiKeyRow=optionCount,listCount=optionCount+1;
  const int updateRow=listCount,selectionCount=listCount+1;
  int sel=std::max(0,std::min(savedSelection,selectionCount-1)),top=std::max(0,savedTop);
  auto applyChange=[&](){
    const std::string previousLanguage(LauncherLocalization::Preference());
    LauncherLocalization::Initialize(storeGet(g_global,"Wrapper/Language","system"));
    if(!loadLauncherFonts()){
      LauncherLocalization::Initialize(previousLanguage);
      storeSet(g_global,"Wrapper/Language",previousLanguage.c_str());
      (void)loadLauncherFonts();
      toast("Could not load the selected language font");
    }
    applyLauncherAppearance();
    uiAudioSetEnabled(strcmp(storeGet(g_global,"Wrapper/UiSounds","true"),"false")!=0);
  };
  auto finish=[&](){ savedSelection=sel; savedTop=top; storeSave(g_global,LAUNCHER_INI); };
  beginScreenFx();
  for(;;){
    if(!beginUiFrame()){ finish(); return; }
    SDL_Event event; navRepeat();
    while(pollUiEvent(event)){
      pumpStick(event);
      int tx=0,ty=0; TouchKind touch=touchFeed(event,&tx,&ty);
      const int visible=std::min(std::max(1,(SH-LIST_Y0-190)/ROW_H),listCount);
      const int buttonWidth=std::min(500,SW-80),buttonHeight=58;
      const int buttonX=(SW-buttonWidth)/2;
      const int buttonY=std::min(SH-buttonHeight-104,LIST_Y0+visible*ROW_H+24);
      if(touchScrollList(touch,sel,top,listCount,visible)) continue;
      if(touch==TOUCH_SWIPE_L&&sel<optionCount){ optAdjust(S_launcher[sel],-1); applyChange(); continue; }
      if(touch==TOUCH_SWIPE_R&&sel<optionCount){ optAdjust(S_launcher[sel],1); applyChange(); continue; }
      if(touch==TOUCH_TAP){
        if(ty<topBarH()||ty>=SH-40){ finish(); return; }
        if(tx>=buttonX&&tx<buttonX+buttonWidth&&ty>=buttonY&&ty<buttonY+buttonHeight){
          sel=updateRow;
          SDL_Event press{}; press.type=SDL_CONTROLLERBUTTONDOWN; press.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&press);
          continue;
        }
        for(int row=0;row<visible&&top+row<listCount;row++){
          int y=LIST_Y0+row*ROW_H;
          if(ty>=y&&ty<y+ROW_H){
            sel=top+row;
            SDL_Event press{}; press.type=SDL_CONTROLLERBUTTONDOWN; press.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&press);
            break;
          }
        }
        continue;
      }
      if(event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP) sel=(sel+selectionCount-1)%selectionCount;
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN) sel=(sel+1)%selectionCount;
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_LEFT&&sel<optionCount){ optAdjust(S_launcher[sel],-1); applyChange(); }
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_RIGHT&&sel<optionCount){ optAdjust(S_launcher[sel],1); applyChange(); }
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_X){
        if(sel<optionCount&&resetOption(S_launcher[sel])){
          applyChange(); toast(LauncherLocalization::Translate("Setting reset to default").data());
        } else if(sel==apiKeyRow){
          storeSet(g_global,"Wrapper/SteamGridDBKey","");
          toast(LauncherLocalization::Translate("Setting reset to default").data());
        }
      }
      else if(event.cbutton.button==BTN_CONFIRM){
        if(sel==updateRow){ runUpdateScreen(); beginScreenFx(); }
        else if(sel==apiKeyRow){
          if(promptAndSaveSteamGridDbKey("SteamGridDB API key",true)){
            toast(steamGridDbKey().empty()?"SteamGridDB API key removed":"SteamGridDB API key updated");
          }
          beginScreenFx();
        }
        else {
          const Opt &option=S_launcher[sel];
          if(option.type==OT_CHOICE&&option.nch>2){ optChoosePopup(option); beginScreenFx(); }
          else optAdjust(option,1);
          applyChange();
        }
      } else if(event.cbutton.button==BTN_X){
        if(sel<optionCount)
          showOptionHelp("Launcher",S_launcher[sel],"Launcher setting");
        else if(sel==apiKeyRow)
          showHelpCard("Launcher","SteamGridDB API key","Online artwork",
                       "Stores the personal SteamGridDB API key used to search and download cover or HOME shortcut artwork. Leave it blank to remove the saved key.",
                       steamGridDbKey().empty()?"Not set":"Configured","Launcher setting");
        else
          showHelpCard("Launcher","Check for Updates","Launcher updates",
                       "Checks the latest published Cemu-nx release, displays its notes, verifies the downloaded NRO, and safely replaces this launcher.",
                       launcherUpdateStatusText().c_str(),"Launcher action");
        beginScreenFx();
      } else if(event.cbutton.button==BTN_CANCEL){ finish(); return; }
      if(sel<listCount){
        if(sel<top) top=sel;
        if(sel>=top+visible) top=sel-visible+1;
      }
    }

    clearUiBackground();
    drawHeader(LauncherLocalization::Translate("Launcher").data(),nullptr);
    int colX,colW,labelX,valX; listCol(&colX,&colW,&labelX,&valX);
    const int visible=std::min(std::max(1,(SH-LIST_Y0-190)/ROW_H),listCount);
    glassPanel(colX-12,LIST_Y0-10,colW+24,std::min(visible,std::max(1,listCount-top))*ROW_H+18);
    if(sel<listCount){
      float target=(float)(LIST_Y0+(sel-top)*ROW_H+1);
      g_hy=(!g_uiAnimations||g_hy<0)?target:g_hy+(target-g_hy)*0.30f;
      drawRowHighlight(colX,(int)g_hy,colW,ROW_H-2);
    }
    for(int row=0;row<visible&&top+row<listCount;row++){
      int index=top+row,slotY=LIST_Y0+row*ROW_H; bool current=index==sel;
      if(index==apiKeyRow){
        drawSettingsRowText(LauncherLocalization::Translate("SteamGridDB API key").data(),
                            steamGridDbKey().empty()?
                              LauncherLocalization::Translate("Not set").data():
                              LauncherLocalization::Translate("Configured").data(),
                            slotY,colW,labelX,valX,current,
                            current?COL_VAL:COL_TXT,current?COL_VAL:COL_DIM);
      } else {
        char value[96]; optValue(S_launcher[index],value,sizeof(value));
        drawSettingsRowText(LauncherLocalization::Translate(S_launcher[index].label).data(),value,
                            slotY,colW,labelX,valX,current,
                            current?COL_VAL:COL_TXT,current?COL_VAL:COL_DIM);
      }
    }
    const int buttonWidth=std::min(500,SW-80),buttonHeight=58;
    const int buttonX=(SW-buttonWidth)/2;
    const int buttonY=std::min(SH-buttonHeight-104,LIST_Y0+visible*ROW_H+24);
    const bool updateSelected=sel==updateRow;
    drawButtonPanel(buttonX,buttonY,buttonWidth,buttonHeight,updateSelected);
    drawTextC(g_font,SW/2,buttonY+(buttonHeight-fontHeight(g_font))/2,LauncherLocalization::Translate("Check for Updates").data(),updateSelected?COL_VAL:COL_TXT);
    const std::string updateStatus=launcherUpdateStatusText();
    drawTextC(g_font_sm,SW/2,buttonY+buttonHeight+8,updateStatus.c_str(),updateSelected?COL_VAL:COL_DIM);
    std::string controls;
    if(sel<optionCount&&canAdjustOption(S_launcher[sel])) controls+="Left / Right  Change       ";
    controls+="A  Choose       ";
    if((sel<optionCount&&canResetOption(S_launcher[sel]))||sel==apiKeyRow) controls+="Y  Reset       ";
    controls+="X  Info       B  Back";
    drawLocalizedFooter(controls.c_str());
    drawFadeIn(); SDL_RenderPresent(g_ren); waitForNextUiFrame();
  }
}
static void installProgress(int pct) {
  clearUiBackground();
  drawHeaderStatic("Installing title...", nullptr);
  const int panelW=std::min(900,SW-160),panelH=150;
  const int panelX=(SW-panelW)/2,panelY=(SH-panelH)/2-16;
  glassPanel(panelX,panelY,panelW,panelH);
  const int bw=panelW-72,bx=(SW-bw)/2,by=panelY+48,bh=26;
  drawProgressBar(bx,by,bw,bh,pct/100.0);
  char t[16]; snprintf(t,sizeof(t),"%d%%",pct);
  drawTextC(g_font,SW/2,by+bh+22,t,COL_TXT);
  SDL_RenderPresent(g_ren);
}
static bool pathIsInstallable(const std::string &path, bool directory) {
  return directory && folderIsExtractedTitle(path);
}

static const char *titleIdKindName(uint64_t titleId) {
  const uint32_t high = (uint32_t)(titleId >> 32);
  return high == 0x0005000E ? "update" : high == 0x0005000C ? "DLC" : "game";
}

static void installFromPath(const std::string &picked) {
  const uint64_t titleId = cemu_peekTitleId(picked);
  if (!titleId) { modalMessageStatic("Not installable", {"No code/app.xml with a Wii U title id here."}); return; }
  char detail[80];
  snprintf(detail, sizeof(detail), "%016llx", (unsigned long long)titleId);
  if (!confirmBoxStatic("Install this title?",
                        {fileNameOf(picked), std::string("Type: ") + titleIdKindName(titleId),
                         std::string("Title id: ") + detail, "", "It will be installed to mlc01."}))
    return;
  std::string msg;
  int r = cemu_installTitle(picked, std::string(DATA_DIR)+"/mlc01", installProgress, msg);
  toast(msg.c_str());
  if (r == 0) { ensureDefaultGameSource(); g_rescanAfterSettings = true; }
}

static void ensureDefaultGameSource() {
  auto sources=loadGameSources();
  std::string identity=pathIdentity(DEF_GAMEDIR);
  if(std::none_of(sources.begin(),sources.end(),[&](const std::string &path){ return pathIdentity(path)==identity; })){
    sources.push_back(DEF_GAMEDIR); saveGameSources(sources); g_rescanAfterSettings=true;
  }
}

static void gameSourcesScreen() {
  int sel=0,top=0;
  for(;;){
    auto sources=loadGameSources(); int n=1+(int)sources.size();
    // Shared list metrics, read by both the tap test and the row renderer.
    const int listY=settingsListY(),rowHeight=50;
    int vis=std::max(1,(SH-listY-settingsFooterReserve())/rowHeight);
    sel=std::max(0,std::min(sel,n-1)); if(sel<top) top=sel; if(sel>=top+vis) top=sel-vis+1;
    bool rebuild=false;
    while(!rebuild){
      if(!beginUiFrame()) return;
      SDL_Event event; navRepeat();
      while(pollUiEvent(event)){
        pumpStick(event); int tx=0,ty=0; TouchKind touch=touchFeed(event,&tx,&ty);
        if(touchScrollList(touch,sel,top,n,vis)) continue;
        if(touch==TOUCH_TAP){
          if(ty>=SH-48) return;
          for(int row=0;row<vis&&top+row<n;row++){ int y=listY+row*rowHeight; if(ty>=y&&ty<y+rowHeight){ sel=top+row; SDL_Event press{}; press.type=SDL_CONTROLLERBUTTONDOWN; press.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&press); break; } }
          continue;
        }
        if(event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
        if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP) sel=(sel+n-1)%n;
        else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN) sel=(sel+1)%n;
        else if(event.cbutton.button==BTN_CANCEL) return;
        else if(event.cbutton.button==BTN_CONFIRM){
          if(sel==0){
            if(sources.size()>=16){ toastStatic("Maximum of 16 game folders"); continue; }
            std::string selected=browseFolder({});
            if(!selected.empty()){
              std::string identity=pathIdentity(selected);
              if(std::any_of(sources.begin(),sources.end(),[&](const std::string &path){ return pathIdentity(path)==identity; })){
                toastStatic("Folder already added");
              } else { ensureSavedPathMountedAtStartup(selected); sources.push_back(selected); saveGameSources(sources); g_rescanAfterSettings=true; sel=(int)sources.size(); }
              rebuild=true;
            }
          } else {
            const char *actions[]={"Change folder","Move up","Move down","Remove"};
            int action=dropdownStaticTitle("Game folder",actions,4,0); size_t index=(size_t)(sel-1);
            if(action==0){
              std::string selected=browseFolder(sources[index]);
              if(!selected.empty()){
                std::string identity=pathIdentity(selected); bool duplicate=false;
                for(size_t i=0;i<sources.size();i++) if(i!=index&&pathIdentity(sources[i])==identity) duplicate=true;
                if(duplicate){ toastStatic("Folder already added"); }
                else { ensureSavedPathMountedAtStartup(selected); sources[index]=selected; saveGameSources(sources); g_rescanAfterSettings=true; }
                rebuild=true;
              }
            } else if(action==1&&index>0){ std::swap(sources[index],sources[index-1]); saveGameSources(sources); sel--; g_rescanAfterSettings=true; rebuild=true; }
            else if(action==2&&index+1<sources.size()){ std::swap(sources[index],sources[index+1]); saveGameSources(sources); sel++; g_rescanAfterSettings=true; rebuild=true; }
            else if(action==3&&confirmBoxStatic("Remove game folder?",{sources[index],"","No files will be deleted."})){
              sources.erase(sources.begin()+index); saveGameSources(sources); sel=std::max(0,sel-1); g_rescanAfterSettings=true; rebuild=true;
            }
          }
        }
        if(sel<top) top=sel;
        if(sel>=top+vis) top=sel-vis+1;
      }
      if(rebuild) break;
      clearUiBackground();
      drawHeaderStatic("Game folders",
        LauncherLocalization::Translate("All folders are scanned and passed to Cemu").data());
      glassPanel(44,listY-12,SW-88,std::min(vis,std::max(1,n-top))*rowHeight+18);
      for(int row=0;row<vis&&top+row<n;row++){
        const int index=top+row,y=listY+row*rowHeight; const bool current=index==sel;
        if(current){ drawRowHighlight(56,y,SW-112,rowHeight-4); }
        const std::string label=index==0?std::string("[ Add game folder ]"):sources[index-1];
        const int nameY=y+(rowHeight-fontHeight(g_font))/2;
        if(current) drawScrollTextL(g_font,82,nameY,SW-170,label.c_str(),COL_VAL);
        else drawText(g_font,82,nameY,ellipsizedText(g_font,label,SW-170).c_str(),index==0?COL_HI:COL_TXT);
      }
      drawLocalizedFooter("A  Select       B  Back");
    SDL_RenderPresent(g_ren); waitForNextUiFrame();
    }
  }
}

static const char *installedKindName(CemuInstalledKind kind) {
  switch (kind) {
    case CemuInstalledKind::Base: return "Base game";
    case CemuInstalledKind::Update: return "Update";
    case CemuInstalledKind::Dlc: return "DLC";
  }
  return "Content";
}

static std::string installedSizeText(uint64_t bytes) {
  char text[32];
  if (bytes >= 1024ull * 1024 * 1024)
    snprintf(text, sizeof(text), "%.2f GiB", bytes / (1024.0 * 1024 * 1024));
  else if (bytes >= 1024ull * 1024)
    snprintf(text, sizeof(text), "%.1f MiB", bytes / (1024.0 * 1024));
  else if (bytes >= 1024)
    snprintf(text, sizeof(text), "%.1f KiB", bytes / 1024.0);
  else
    snprintf(text, sizeof(text), "%llu B", (unsigned long long)bytes);
  return text;
}

static bool installedContentScreen(uint64_t baseTitleFilter = 0) {
  const std::string mlcRoot = std::string(DATA_DIR) + "/mlc01";
  bool baseDeleted = false;
  int sel = 0, top = 0;
  for (;;) {
    auto components = cemu_scanInstalledComponents(mlcRoot, true);
    if (baseTitleFilter)
      components.erase(std::remove_if(components.begin(), components.end(),
        [&](const auto &component) { return component.baseTitleId != baseTitleFilter; }),
        components.end());
    const int count = (int)components.size();
    // Shared list metrics, read by both the tap test and the row renderer.
    const int listY = settingsListY(), rowHeight = 70;
    const int visible = std::max(1, (SH - listY - settingsFooterReserve()) / rowHeight);
    sel = count ? std::max(0, std::min(sel, count - 1)) : 0;
    if (sel < top) top = sel;
    if (sel >= top + visible) top = sel - visible + 1;
    bool refresh = false;
    while (!refresh) {
      if (!beginUiFrame()) return baseDeleted;
      SDL_Event event; navRepeat();
      while (pollUiEvent(event)) {
        pumpStick(event);
        int tx = 0, ty = 0;
        TouchKind touch = touchFeed(event, &tx, &ty);
        if (count && touchScrollList(touch, sel, top, count, visible))
          continue;
        if (touch == TOUCH_TAP) {
          if (ty >= SH - 44) return baseDeleted;
          for (int row = 0; row < visible && top + row < count; row++) {
            const int y = listY + row * rowHeight;
            if (ty >= y && ty < y + rowHeight) {
              sel = top + row;
              SDL_Event press{};
              press.type = SDL_CONTROLLERBUTTONDOWN;
              press.cbutton.button = BTN_CONFIRM;
              SDL_PushEvent(&press);
              break;
            }
          }
          continue;
        }
        if (event.type != SDL_CONTROLLERBUTTONDOWN)
          continue;
        if (event.cbutton.button == BTN_CANCEL)
          return baseDeleted;
        if (!count)
          continue;
        if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_UP)
          sel = (sel + count - 1) % count;
        else if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)
          sel = (sel + 1) % count;
        else if (event.cbutton.button == BTN_CONFIRM) {
          const auto component = components[sel];
          const char *actions[] = {"View details", "Delete installed component"};
          const int action = dropdown(installedKindName(component.kind), actions, 2, 0);
          char titleId[32];
          snprintf(titleId, sizeof(titleId), "%016llX",
                   (unsigned long long)component.titleId);
          if (action == 0) {
            modalMessageStatic("Installed component", {
              component.name.empty() ? "Unnamed Wii U title" : component.name,
              std::string(installedKindName(component.kind)) + "  -  " + titleId,
              installedSizeText(component.sizeBytes),
              component.path,
              std::string(LauncherLocalization::Translate(component.metadataValid ?
                "Metadata verified" : "Metadata missing or does not match the title ID"))
            });
            beginScreenFx();
          } else if (action == 1 && confirmBox(
              (std::string("Delete ") + installedKindName(component.kind) + "?").c_str(), {
                component.name.empty() ? titleId : component.name,
                titleId,
                component.path,
                "",
                "Only this installed component will be deleted.",
                "Saved data is not deleted. This cannot be undone."
              })) {
            std::string error;
            if (!cemu_removeInstalledComponent(mlcRoot, component.titleId, error)) {
              modalMessageStatic("Delete failed", {error, "Saved data was not touched."});
              beginScreenFx();
            } else {
              baseDeleted = baseDeleted || component.kind == CemuInstalledKind::Base;
              g_rescanAfterSettings = true;
              toastStatic("Installed component deleted");
              refresh = true;
            }
          }
        }
        if (sel < top) top = sel;
        if (sel >= top + visible) top = sel - visible + 1;
      }
      if (refresh) break;

      clearUiBackground();
      drawHeader(LauncherLocalization::Translate(
                   baseTitleFilter ? "Installed game content" : "Installed content").data(),
                 LauncherLocalization::Translate("Base games, updates, and DLC").data());
      if (!count) {
        const int emptyW = std::min(760, SW - 96), emptyH = 140;
        glassPanel((SW - emptyW) / 2, (SH - emptyH) / 2 - 8, emptyW, emptyH);
        drawTextC(g_font, SW / 2, SH / 2 - 20,LauncherLocalization::Translate(
                  baseTitleFilter ? "No installed content for this game" : "No installed content found").data(),
                  COL_DIM);
      }
      if (count)
        glassPanel(42, listY - 12, SW - 84,
                   std::min(visible, std::max(1, count - top)) * rowHeight + 18);
      for (int row = 0; row < visible && top + row < count; row++) {
        const int index = top + row, y = listY + row * rowHeight;
        const bool current = index == sel;
        if (current) drawRowHighlight(54, y, SW - 108, rowHeight - 4);
        const auto &component = components[index];
        std::string label = std::string("[") + installedKindName(component.kind) + "] " +
                            (component.name.empty() ? "Unnamed Wii U title" : component.name);
        if (current) drawScrollTextL(g_font, 78, y + 8, SW - 310, label.c_str(), COL_VAL);
        else drawText(g_font, 78, y + 8,
                      ellipsizedText(g_font, label, SW - 310).c_str(), COL_TXT);
        char titleId[32];
        snprintf(titleId, sizeof(titleId), "%016llX",
                 (unsigned long long)component.titleId);
        drawText(g_font_sm, 80, y + 40, titleId,
                 component.metadataValid ? COL_DIM : (SDL_Color){230, 150, 110, 255});
        const std::string size = installedSizeText(component.sizeBytes);
        drawTextR(g_font_sm, SW - 74, y + 26, size.c_str(), current ? COL_VAL : COL_DIM);
      }
      drawLocalizedFooter(count ? "A  Details / delete       B  Back" : "B  Back");
      drawFadeIn(); SDL_RenderPresent(g_ren); waitForNextUiFrame();
    }
  }
}

static void downloadAllCovers();
static void libraryStorageScreen() {
  static int savedSelection=0;
  constexpr int rowCount=6;
  const int rowHeight=std::max(settingsRowH(),52);
  const int startY=settingsListY()+40;
  int sel=std::max(0,std::min(savedSelection,rowCount-1));
  size_t installedCount = cemu_scanInstalledComponents(std::string(DATA_DIR) + "/mlc01").size();
  size_t folderCount=loadSavedGameSourceRecords().size();
  auto shares=loadSmbSharesFromStore();
  auto openRow=[&](){
    if(sel==0) gameSourcesScreen();
    else if(sel==1) runFileManager();
    else if(sel==2) networkSharesScreen();
    else if(sel==3) downloadAllCovers();
    else if(sel==4) installedContentScreen();
    else gfxPackScreen(0);
    installedCount = cemu_scanInstalledComponents(std::string(DATA_DIR) + "/mlc01").size();
    folderCount=loadSavedGameSourceRecords().size();
    shares=loadSmbSharesFromStore();
    beginScreenFx();
  };
  beginScreenFx();
  for(;;){
    if(!beginUiFrame()){ savedSelection=sel; return; }
    SDL_Event event; navRepeat();
    while(pollUiEvent(event)){
      pumpStick(event);
      int tx=0,ty=0; TouchKind touch=touchFeed(event,&tx,&ty);
      if(touch==TOUCH_TAP){
        if(ty<topBarH()||ty>=SH-40){ savedSelection=sel; return; }
        for(int row=0;row<rowCount;row++){
          int y=startY+row*rowHeight;
          if(ty>=y&&ty<y+rowHeight){ sel=row; openRow(); break; }
        }
        continue;
      }
      if(event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP) sel=(sel+rowCount-1)%rowCount;
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN) sel=(sel+1)%rowCount;
      else if(event.cbutton.button==BTN_CONFIRM) openRow();
      else if(event.cbutton.button==BTN_X&&sel==3){
        showHelpCard("Library & storage","Download covers","Library artwork",
                     "Downloads missing covers and Classic backgrounds from SteamGridDB. Existing local artwork is kept.",
                     nullptr,"Launcher action");
        beginScreenFx();
      }
      else if(event.cbutton.button==BTN_CANCEL){ savedSelection=sel; return; }
    }

    clearUiBackground();
    drawHeader(LauncherLocalization::Translate("Library & storage").data(),nullptr);
    int colX,colW,labelX,valX; listCol(&colX,&colW,&labelX,&valX);
    drawSectionHeading("General",colX,startY-44,colW);
    glassPanel(colX-12,startY-10,colW+24,rowCount*rowHeight+18);
    float target=(float)(startY+sel*rowHeight+2);
    g_hy=(!g_uiAnimations||g_hy<0)?target:g_hy+(target-g_hy)*0.30f;
    drawRowHighlight(colX,(int)g_hy,colW,rowHeight-4);

    size_t mounted=0;
    for(const auto &share:shares) if(SwitchStorage::IsSmbMounted(share.id)) mounted++;
    std::string folderValue=std::to_string(folderCount)+(folderCount==1?" folder":" folders");
    std::string smbValue=std::to_string(mounted)+" / "+std::to_string(shares.size())+" connected";
    std::string installedValue = std::to_string(installedCount) +
                                 (installedCount == 1 ? " component" : " components");
    const char *labels[rowCount]={"Game folders","File manager","SMB network shares","Download covers","Installed content","Graphics packs"};
    const char *values[rowCount]={folderValue.c_str(),"SD / USB / SMB",smbValue.c_str(),"SteamGridDB",installedValue.c_str(),"browse / download"};
    for(int row=0;row<rowCount;row++){
      const int slot=startY+row*rowHeight;
      const bool current=row==sel;
      drawSettingsRowText(LauncherLocalization::Translate(labels[row]).data(),
                          LauncherLocalization::Translate(values[row]).data(),
                          slot,colW,labelX,valX,current,
                          current?COL_VAL:COL_TXT,current?COL_VAL:COL_DIM,false,rowHeight);
    }
    if(sel==3) drawLocalizedFooter("A  Select       X  Info       B  Back");
    else drawLocalizedFooter("A  Open       B  Back");
    drawFadeIn(); SDL_RenderPresent(g_ren); waitForNextUiFrame();
  }
}

struct ImportedOnlineAccount {
  uint32_t persistentId{};
  std::string label;
};

static bool parsePersistentId(const char *name, uint32_t &persistentId) {
  if (!name || strlen(name) != 8)
    return false;
  uint32_t value = 0;
  for (int i = 0; i < 8; i++) {
    const char c = name[i];
    const int digit = c >= '0' && c <= '9' ? c - '0' :
                      c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                      c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    if (digit < 0)
      return false;
    value = (value << 4) | (uint32_t)digit;
  }
  if (value < 0x80000001)
    return false;
  persistentId = value;
  return true;
}

static std::string readAccountLabel(const std::string &path) {
  FILE *file = fopen(path.c_str(), "rb");
  if (!file)
    return {};
  char buffer[65537];
  const size_t size = fread(buffer, 1, sizeof(buffer) - 1, file);
  const bool complete = !ferror(file) && feof(file);
  fclose(file);
  if (!complete)
    return {};
  buffer[size] = 0;
  for (const char *key : {"AccountId=", "MiiName="}) {
    const char *value = strstr(buffer, key);
    if (!value)
      continue;
    value += strlen(key);
    const char *end = strpbrk(value, "\r\n");
    const size_t length = std::min<size_t>(end ? (size_t)(end - value) : strlen(value), 32);
    std::string result(value, length);
    if (!result.empty() && std::all_of(result.begin(), result.end(),
        [](unsigned char c) { return c >= 0x20 && c < 0x7f; }))
      return result;
  }
  return {};
}

static std::vector<ImportedOnlineAccount> scanOnlineAccounts() {
  const std::string root = std::string(DATA_DIR) + "/mlc01/usr/save/system/act";
  std::vector<ImportedOnlineAccount> accounts;
  DIR *dir = opendir(root.c_str());
  if (!dir)
    return accounts;
  while (dirent *entry = readdir(dir)) {
    uint32_t persistentId = 0;
    if (!parsePersistentId(entry->d_name, persistentId))
      continue;
    const std::string folder = root + "/" + entry->d_name;
    const std::string accountFile = folder + "/account.dat";
    struct stat folderStat{}, fileStat{};
    if (lstat(folder.c_str(), &folderStat) != 0 || !S_ISDIR(folderStat.st_mode) ||
        S_ISLNK(folderStat.st_mode) || lstat(accountFile.c_str(), &fileStat) != 0 ||
        !S_ISREG(fileStat.st_mode) || S_ISLNK(fileStat.st_mode))
      continue;
    accounts.push_back({persistentId, readAccountLabel(accountFile)});
  }
  closedir(dir);
  std::sort(accounts.begin(), accounts.end(), [](const auto &left, const auto &right) {
    return left.persistentId < right.persistentId;
  });
  return accounts;
}

static const char *credentialState(const std::string &path, off_t expectedSize) {
  struct stat st{};
  if (lstat(path.c_str(), &st) != 0)
    return "Missing";
  return S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode) && st.st_size == expectedSize
           ? "Ready" : "Invalid";
}

static bool directoryHasFiles(const std::string &path) {
  DIR *dir = opendir(path.c_str());
  if (!dir)
    return false;
  bool found = false;
  while (dirent *entry = readdir(dir)) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
      continue;
    struct stat st{};
    const std::string child = path + "/" + entry->d_name;
    if (lstat(child.c_str(), &st) == 0 && S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode)) {
      found = true;
      break;
    }
  }
  closedir(dir);
  return found;
}

static void onlineSettingsScreen() {
  static const char *serviceNames[] = {"Offline", "Nintendo", "Pretendo", "Custom"};
  auto accounts = scanOnlineAccounts();
  uint32_t persistentId = 0x80000001;
  int service = 0;
  if (!cemu_readAccountSelection(SETTINGS_XML, persistentId, service)) {
    modalMessageStatic("Online settings unavailable", {"settings.xml could not be read safely."});
    return;
  }
  auto accountIndex = [&]() {
    for (size_t i = 0; i < accounts.size(); i++)
      if (accounts[i].persistentId == persistentId) return (int)i;
    return -1;
  };
  int sel = 0;
  const int panelX = 170, panelW = SW - 340, rowH = settingsRowH() + 16;
  const int listY = settingsListY() + 12;
  const auto rowTop = [&](int index) { return listY + index * (rowH + 8); };
  const int optionsPanelY = listY - 10, optionsPanelH = rowH * 2 + 28;
  beginScreenFx();
  for (;;) {
    const int selectedAccount = accountIndex();
    if (!beginUiFrame()) return;
    SDL_Event event; navRepeat();
    while (pollUiEvent(event)) {
      pumpStick(event);
      int tx = 0, ty = 0;
      const TouchKind touch = touchFeed(event, &tx, &ty);
      if (touch == TOUCH_TAP) {
        if (ty >= SH - 42) return;
        if (ty >= rowTop(0) && ty < rowTop(0) + rowH) sel = 0;
        else if (ty >= rowTop(1) && ty < rowTop(1) + rowH) sel = 1;
        else continue;
        SDL_Event press{};
        press.type = SDL_CONTROLLERBUTTONDOWN;
        press.cbutton.button = BTN_CONFIRM;
        SDL_PushEvent(&press);
        continue;
      }
      if (event.type != SDL_CONTROLLERBUTTONDOWN)
        continue;
      if (event.cbutton.button == BTN_CANCEL)
        return;
      if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_UP ||
          event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)
        sel = 1 - sel;
      else if (event.cbutton.button == BTN_CONFIRM) {
        if (sel == 0) {
          if (accounts.empty()) {
            modalMessageStatic("No imported accounts", {
              "Copy an existing Wii U account.dat into:",
              std::string(DATA_DIR) + "/mlc01/usr/save/system/act/<persistent-id>/"
            });
          } else {
            std::vector<std::string> labels;
            std::vector<const char *> pointers;
            for (const auto &account : accounts) {
              char id[16];
              snprintf(id, sizeof(id), "%08X", account.persistentId);
              labels.push_back(account.label.empty() ? id : account.label + "  (" + id + ")");
            }
            for (const auto &label : labels) pointers.push_back(label.c_str());
            const int choice = dropdownStaticTitle("Imported Wii U account", pointers.data(),
                                        (int)pointers.size(), selectedAccount);
            if (choice >= 0) {
              persistentId = accounts[choice].persistentId;
              cemu_readAccountService(SETTINGS_XML, persistentId, service);
              if (!cemu_writeAccountSelection(SETTINGS_XML, persistentId, service))
                modalMessageStatic("Settings not saved", {"settings.xml could not be updated safely."});
            }
          }
          beginScreenFx();
        } else if (selectedAccount < 0) {
          modalMessageStatic("Select an account first", {"Online services require an imported account.dat."});
          beginScreenFx();
        } else {
          const int choice = dropdownStaticTitle("Network service", serviceNames, 4, service);
          if (choice >= 0) {
            service = choice;
            if (!cemu_writeAccountSelection(SETTINGS_XML, persistentId, service)) {
              modalMessageStatic("Settings not saved", {"settings.xml could not be updated safely."});
            } else if (service == 3) {
              struct stat st{};
              if (lstat((std::string(DATA_DIR) + "/network_services.xml").c_str(), &st) != 0 ||
                  !S_ISREG(st.st_mode) || S_ISLNK(st.st_mode))
                modalMessageStatic("Custom service needs configuration", {
                  "Add network_services.xml to the Cemu directory.",
                  "The service remains offline until that file is present."
                });
            }
          }
          beginScreenFx();
        }
      }
    }

    clearUiBackground();
    drawHeaderStatic("Online & accounts", "Offline / Nintendo / Pretendo / Custom");
    glassPanel(panelX, optionsPanelY, panelW, optionsPanelH);
    drawRowHighlight(panelX + 10, rowTop(sel) + 1, panelW - 20, rowH - 2);
    std::string accountValue = "None imported";
    if (selectedAccount >= 0) {
      char id[16]; snprintf(id, sizeof(id), "%08X", persistentId);
      accountValue = accounts[selectedAccount].label.empty() ? id : accounts[selectedAccount].label + "  (" + id + ")";
    } else if (!accounts.empty()) {
      char id[32]; snprintf(id, sizeof(id), "%08X (account.dat missing)", persistentId);
      accountValue = id;
    }
    drawSettingsRowText(LauncherLocalization::Translate("Account").data(), accountValue.c_str(),
                        rowTop(0), panelW, panelX + 34, panelX + panelW - 28, sel == 0,
                        sel == 0 ? COL_VAL : COL_TXT, sel == 0 ? COL_VAL : COL_DIM, true, rowH);
    drawSettingsRowText(LauncherLocalization::Translate("Service").data(),
                        selectedAccount >= 0 ? serviceNames[service] :
                          LauncherLocalization::Translate("Select an account").data(),
                        rowTop(1), panelW, panelX + 34, panelX + panelW - 28, sel == 1,
                        sel == 1 ? COL_VAL : COL_TXT, sel == 1 ? COL_VAL : COL_DIM, false, rowH);

    const std::string certRoot = std::string(DATA_DIR) +
      "/mlc01/sys/title/0005001b/10054000/content";
    const bool certsPresent = directoryHasFiles(certRoot + "/ccerts") &&
                              directoryHasFiles(certRoot + "/scerts");
    const char *otp = credentialState(std::string(DATA_DIR) + "/otp.bin", 1024);
    const char *seeprom = credentialState(std::string(DATA_DIR) + "/seeprom.bin", 512);
    struct stat customConfigStat{};
    const std::string customConfigPath = std::string(DATA_DIR) + "/network_services.xml";
    const bool customConfig = lstat(customConfigPath.c_str(), &customConfigStat) == 0 &&
                              S_ISREG(customConfigStat.st_mode) && !S_ISLNK(customConfigStat.st_mode);
    const int statusPanelY = optionsPanelY + optionsPanelH + 14;
    const int statusX = panelX + 28, statusY = statusPanelY + 68, statusStep = 43;
    glassPanel(panelX, statusPanelY, panelW, 4 * statusStep + fontHeight(g_font_sm) + 92);
    drawSectionHeading("Credential readiness", statusX, statusY - 46, panelW - 56);
    const std::string statusLines[] = {
      std::string("Selected account.dat: ") + (selectedAccount >= 0 ? "Ready" : "Missing"),
      std::string("otp.bin (1024 bytes): ") + otp,
      std::string("seeprom.bin (512 bytes): ") + seeprom,
      std::string("Wii U certificate store: ") + (certsPresent ? "Present" : "Missing / incomplete"),
      std::string("Custom service XML: ") + (customConfig ? "Ready" : "Missing")
    };
    for (int i = 0; i < 5; i++) {
      const bool ready = statusLines[i].find("Ready") != std::string::npos ||
                         statusLines[i].find("Present") != std::string::npos;
      drawText(g_font_sm, statusX, statusY + i * statusStep, statusLines[i].c_str(),
               ready ? (SDL_Color){145, 220, 155, 255} : (SDL_Color){235, 145, 120, 255});
    }
    drawStaticTextC(g_font_sm, SW / 2, SH - settingsFooterReserve() - 20,
              "Selecting a service does not create credentials or guarantee server access.", COL_DIM);
    drawLocalizedFooter("A  Change       B  Back");
    drawFadeIn(); SDL_RenderPresent(g_ren); waitForNextUiFrame();
  }
}

static void runSettingsRoot(const char *ctx) {
  bool global = !(ctx && *ctx);
  static const int globalOrder[] = { SCR_CPU, SCR_GRAPHICS, SCR_AUDIO, SCR_OVERLAY,
                                     SCR_INPUT };
  static const int gameOrder[] = { SCR_FRAMEGEN, SCR_CPU, SCR_GRAPHICS, SCR_AUDIO,
                                   SCR_OVERLAY, SCR_INPUT };
  const int *order=global?globalOrder:gameOrder;
  const int nscr=global?(int)(sizeof(globalOrder)/sizeof(*globalOrder)):
                        (int)(sizeof(gameOrder)/sizeof(*gameOrder));
  const int launcherRow=0,libraryRow=1,onlineRow=2,framegenRow=3,screenStart=4;
  const int langRow=screenStart+nscr;
  const int n=global?langRow+1:nscr;
  int sel=0,top=0;
  int rowH=0,y0=0,sectionGap=0,vis=1;
  const auto layout=[&](){
    rowH=settingsRowH(); y0=settingsListY()+40; sectionGap=global?56:0;
    vis=std::max(1,(SH-y0-settingsFooterReserve()-sectionGap)/rowH);
  };
  layout();
  auto rowY=[&](int index){ return y0+(index-top)*rowH+((global&&index>=screenStart&&top<screenStart)?sectionGap:0); };
  beginScreenFx();
  for(;;){
    if (!beginUiFrame()) return;
    layout();
    SDL_Event e;
    navRepeat();
    while(pollUiEvent(e)){
      pumpStick(e);
      { int tx=0,ty=0; TouchKind tk=touchFeed(e,&tx,&ty);
        if(touchScrollList(tk,sel,top,n,vis)) continue;
        if(tk==TOUCH_TAP){
          if(ty<topBarH() || ty>=SH-40){ return; }
          for(int row=0;row<vis&&top+row<n;row++){ int index=top+row,y=rowY(index); if(ty>=y && ty<y+rowH){ sel=index;
            SDL_Event a; memset(&a,0,sizeof(a)); a.type=SDL_CONTROLLERBUTTONDOWN; a.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&a); break; } }
          continue;
        }
      }
      if(e.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      switch(e.cbutton.button){
        case SDL_CONTROLLER_BUTTON_DPAD_UP: sel=(sel+n-1)%n; break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: sel=(sel+1)%n; break;
        case BTN_CONFIRM:
          if(global && sel==launcherRow){ launcherSettingsScreen(); }
          else if(global && sel==libraryRow){ libraryStorageScreen(); }
          else if(global && sel==onlineRow){ onlineSettingsScreen(); }
          else if(global && sel==framegenRow){ runSettings(SCR_FRAMEGEN,ctx); }
          else if(global && sel==langRow){ optChoosePopup(O_console_language); }
          else runSettings(order[global?sel-screenStart:sel],ctx);
          beginScreenFx();
          break;
        case BTN_X:
          if(global && sel==launcherRow)
            showHelpCard("Settings","Launcher","Launcher appearance",
                         "Changes the SDL launcher's theme, library grid, labels, animations, sounds, artwork service, and update behavior.",
                         nullptr,"Settings category");
          else if(global && sel==libraryRow)
            showHelpCard("Settings","Library & storage","Game and file management",
                         "Manages game folders, local or removable storage, installed Wii U content, files, and SMB network shares used by the launcher.",
                         nullptr,"Settings category");
          else if(global && sel==onlineRow)
            showHelpCard("Settings","Online & accounts","Wii U network services",
                         "Selects offline, Nintendo, Pretendo, or custom network services and imports the account files Cemu needs for online play.",
                         nullptr,"Settings category");
          else if(global && sel==framegenRow)
            showHelpCard("Settings","Frame Generation","Display processing",
                         settingsScreenDescription(SCR_FRAMEGEN),nullptr,"Settings category");
          else if(global && sel==langRow)
            showOptionHelp("Settings",O_console_language,"Global setting");
          else {
            const int screen=order[global?sel-screenStart:sel];
            showHelpCard(global?"Settings":"Game settings",g_screens[screen].title,
                         "Settings category",settingsScreenDescription(screen),nullptr,
                         global?"Global settings":"Per-game overrides");
          }
          beginScreenFx();
          break;
        case SDL_CONTROLLER_BUTTON_X:
          if(global&&sel==langRow&&resetOption(O_console_language)){
            toast(LauncherLocalization::Translate("Setting reset to default").data());
          }
          break;
        case BTN_CANCEL: return;
      }
      if(sel<top) top=sel;
      if(sel>=top+vis) top=sel-vis+1;
    }
    clearUiBackground();
    drawHeader(LauncherLocalization::Translate(global ? "Settings" : "Game settings").data(),
               global?LauncherLocalization::Translate("Global settings").data():ctx);
    int colX,colW,labelX,valX; listCol(&colX,&colW,&labelX,&valX);
    if(global){
      auto drawVisibleSection=[&](const char *heading,int begin,int end){
        const int first=std::max(begin,top);
        const int last=std::min(end,top+vis);
        if(first>=last) return;
        const int panelY=rowY(first)-10;
        const int panelBottom=rowY(last-1)+rowH+8;
        drawSectionHeading(heading,colX,panelY-34,colW);
        glassPanel(colX-12,panelY,colW+24,panelBottom-panelY);
      };
      drawVisibleSection("General",0,screenStart);
      drawVisibleSection("Emulator",screenStart,n);
    } else {
      drawSectionHeading("Emulator",colX,y0-44,colW);
      glassPanel(colX-12,y0-10,colW+24,std::min(vis,std::max(1,n-top))*rowH+18);
    }
    float ty=(float)(rowY(sel)+2);
    g_hy=(!g_uiAnimations||g_hy<0)?ty:g_hy+(ty-g_hy)*0.30f;
    drawRowHighlight(colX,(int)g_hy,colW,rowH-4);
    for(int row=0;row<vis&&top+row<n;row++){ int i=top+row,slot=rowY(i); bool cur=i==sel;
      const SDL_Color labelColor=cur?COL_VAL:COL_TXT,valueColor=cur?COL_VAL:COL_DIM;
      if(global && i==langRow){
        char lv[64]; optValue(O_console_language, lv, sizeof(lv));
        drawSettingsRowText(LauncherLocalization::Translate("Console language").data(),lv,
                            slot,colW,labelX,valX,cur,labelColor,valueColor,false,rowH);
      } else if(global && i==launcherRow){
        const char *theme=storeGet(g_global,"Wrapper/Theme","wiiu");
        const char *value=!strcmp(theme,"classic")?"Classic":"Wii U";
        drawSettingsRowText(LauncherLocalization::Translate("Launcher").data(),value,
                            slot,colW,labelX,valX,cur,labelColor,valueColor,false,rowH);
      } else if(global && i==libraryRow){
        drawSettingsRowText(LauncherLocalization::Translate("Library & storage").data(),
                            LauncherLocalization::Translate("games / files / network").data(),
                            slot,colW,labelX,valX,cur,labelColor,valueColor,false,rowH);
      } else if(global && i==onlineRow){
        drawSettingsRowText(LauncherLocalization::Translate("Online & accounts").data(),
                            LauncherLocalization::Translate("Offline / Nintendo / Pretendo").data(),
                            slot,colW,labelX,valX,cur,labelColor,valueColor,false,rowH);
      } else if(global && i==framegenRow){
        drawSettingsRowText(LauncherLocalization::Translate("Frame Generation").data(),
                            LauncherLocalization::Translate("LSFG 2x / Vulkan").data(),
                            slot,colW,labelX,valX,cur,labelColor,valueColor,false,rowH);
      } else {
        drawSettingsRowText(LauncherLocalization::Translate(g_screens[order[global?i-screenStart:i]].title).data(),">",
                            slot,colW,labelX,valX,cur,labelColor,valueColor,false,rowH);
      }
    }
    if(n>vis){
      const bool spansSections=global&&top<screenStart&&top+vis>screenStart;
      int trackH=vis*rowH+(spansSections?sectionGap:0),trackX=colX+colW+16;
      fillRect(trackX,y0,4,trackH,(SDL_Color){40,44,54,255});
      int thumbH=std::max(16,trackH*vis/n),denom=std::max(1,n-vis); fillRect(trackX,y0+(trackH-thumbH)*top/denom,4,thumbH,COL_SEL); }
    drawLocalizedFooter(global&&sel==langRow?
      "A  Open       Y  Reset       X  Info       B  Back":"A  Open       X  Info       B  Back");
    drawFadeIn();
    SDL_RenderPresent(g_ren);
    waitForNextUiFrame();
  }
}

static void toast(const char *msg) {
  g_toastMessage=msg?msg:"";
  g_toastUntil=SDL_GetTicks()+1800;
  wakeUiFromWorker(0x544f4153);
}

static void drawToastOverlay() {
  if(g_toastMessage.empty()) return;
  const Uint32 now=SDL_GetTicks();
  if(SDL_TICKS_PASSED(now,g_toastUntil)){ g_toastMessage.clear(); return; }
  const int pw=std::min(820,SW-64),ph=120,px=(SW-pw)/2,py=(SH-ph)/2;
  glassPanel(px,py,pw,ph);
  drawTextC(g_font,SW/2,py+46,ellipsizedText(g_font,g_toastMessage,pw-48).c_str(),COL_TXT);
}

static std::vector<std::string> wrapDialogLines(const std::vector<std::string> &lines,
                                                int maxWidth) {
  std::vector<std::string> wrapped;
  for(const std::string &source:lines){
    if(source.empty()){ wrapped.emplace_back(); continue; }
    std::string line;
    size_t cursor=0;
    while(cursor<source.size()){
      while(cursor<source.size()&&source[cursor]==' ') cursor++;
      size_t end=source.find(' ',cursor);
      std::string word=source.substr(cursor,end==std::string::npos?std::string::npos:end-cursor);
      std::string candidate=line.empty()?word:line+" "+word;
      if(!line.empty()&&textW(g_font,candidate.c_str())>maxWidth){
        wrapped.push_back(std::move(line));
        line=std::move(word);
      } else line=std::move(candidate);
      if(end==std::string::npos) break;
      cursor=end+1;
    }
    if(!line.empty())
      wrapped.push_back(textW(g_font,line.c_str())<=maxWidth?line:
                        ellipsizedText(g_font,line,maxWidth));
  }
  return wrapped;
}

static void modalMessage(const char *title, const std::vector<std::string> &lines) {
  const int pw=SW*3/4;
  const std::vector<std::string> displayLines=wrapDialogLines(lines,pw-64);
  const int lineHeight=40;
  const int ph=std::min(SH-64,180+(int)displayLines.size()*lineHeight);
  const int px=(SW-pw)/2,py=(SH-ph)/2;
  for (;;) {
    if (!beginUiFrame()) return;
    SDL_Event e;
    navRepeat();
    while (pollUiEvent(e)) {
      pumpStick(e);
      { int tx=0,ty=0; if(touchFeed(e,&tx,&ty)==TOUCH_TAP) return; }
      if (e.type == SDL_CONTROLLERBUTTONDOWN &&
          (e.cbutton.button == BTN_CONFIRM || e.cbutton.button == BTN_CANCEL)) return;
    }
    clearUiBackground();
    glassPanel(px,py,pw,ph);
    drawText(g_font_big,px+28,py+22,fittedText(g_font_big,title?title:"",pw-56).c_str(),COL_TXT);
    fillRect(px+28,py+72,pw-56,1,(SDL_Color){255,255,255,24});
    int y = py+96;
    for(const std::string &line:displayLines){
      if(y+fontHeight(g_font)>=py+ph-54) break;
      drawTextC(g_font,SW/2,y,line.c_str(),COL_TXT);
      y+=lineHeight;
    }
    FootItem messageFooter[]={{"A",LauncherLocalization::Translate("Continue").data()}};
    drawFooterHints(messageFooter,1,py+ph-30);
    SDL_RenderPresent(g_ren); waitForNextUiFrame();
  }
}

static bool confirmBox(const char *title, const std::vector<std::string> &lines) {
  int pw=SW*3/4;
  const std::vector<std::string> displayLines=wrapDialogLines(lines,pw-64);
  int ph=std::min(SH-64,220+(int)displayLines.size()*40),px=(SW-pw)/2,py=(SH-ph)/2;
  int bw=std::min(210,(pw-54)/2),bh=56,bby=py+ph-bh-22,yesx=SW/2-bw-12,nox=SW/2+12;
  for(;;){
    if (!beginUiFrame()) return false;
    SDL_Event e;
    navRepeat();
    while(pollUiEvent(e)){
      pumpStick(e);
      { int tx=0,ty=0; if(touchFeed(e,&tx,&ty)==TOUCH_TAP && ty>=bby && ty<bby+bh){
          if(tx>=yesx && tx<yesx+bw) return true;
          if(tx>=nox  && tx<nox+bw)  return false;
      } }
      if(e.type==SDL_QUIT) return false;
      if(e.type==SDL_CONTROLLERBUTTONDOWN){
        if(e.cbutton.button==BTN_CONFIRM) return true;
        if(e.cbutton.button==BTN_CANCEL) return false;
      }
    }
    clearUiBackground();
    glassPanel(px,py,pw,ph);
    drawText(g_font_big,px+28,py+22,fittedText(g_font_big,title?title:"",pw-56).c_str(),(SDL_Color){238,135,135,255});
    fillRect(px+28,py+72,pw-56,1,(SDL_Color){255,255,255,24});
    int y=py+96;
    for(const std::string &line:displayLines){
      if(y+40>=bby-8) break;
      drawTextC(g_font,SW/2,y,line.c_str(),COL_TXT);
      y+=40;
    }
    const auto drawChoice=[&](int x,SDL_Texture *glyph,const char *label){
      const std::string_view shown=LauncherLocalization::Translate(label);
      int gw=0,gh=0;
      if(glyph){ SDL_QueryTexture(glyph,nullptr,nullptr,&gw,&gh); gw/=GLYPH_SS; gh/=GLYPH_SS; }
      const int labelWidth=textW(g_font,shown.data());
      const int left=x+(bw-gw-8-labelWidth)/2;
      if(glyph){ SDL_Rect icon={left,bby+(bh-gh)/2,gw,gh}; SDL_RenderCopy(g_ren,glyph,nullptr,&icon); }
      drawText(g_font,left+gw+8,bby+(bh-fontHeight(g_font))/2,shown.data(),COL_TXT);
    };
    roundedPanel(yesx,bby,bw,bh,SDL_Color{115,44,51,255},SDL_Color{235,125,125,255},6);
    drawChoice(yesx,g_gA,"Yes");
    roundedPanel(nox,bby,bw,bh,COL_FOCUS,COL_SEL,6);
    drawChoice(nox,g_gB,"No");
    SDL_RenderPresent(g_ren); waitForNextUiFrame();
  }
}

static std::string installedReleaseTag() {
  const std::string built=LauncherUpdate_BuiltReleaseTag();
#if defined(CEMU_SWITCH_UPDATE_TEST)
  return built;
#else
  const std::string stored=storeGet(g_global,"Wrapper/InstalledReleaseTag","");
  if(stored.empty()) return built;
  return LauncherUpdate_IsNewer(stored,built)?stored:built;
#endif
}

static std::vector<size_t> utf8Boundaries(const std::string &text) {
  std::vector<size_t> boundaries{0};
  for(size_t index=0;index<text.size();){
    const unsigned char lead=(unsigned char)text[index];
    size_t length=lead<0x80?1:(lead&0xe0)==0xc0?2:(lead&0xf0)==0xe0?3:(lead&0xf8)==0xf0?4:1;
    if(index+length>text.size()) length=1;
    for(size_t part=1;part<length;part++) if(((unsigned char)text[index+part]&0xc0)!=0x80){ length=1; break; }
    index+=length;
    boundaries.push_back(index);
  }
  return boundaries;
}

static std::vector<std::string> wrapReleaseNotes(const std::string &notes,int maxWidth) {
  std::vector<std::string> lines;
  size_t paragraphStart=0;
  while(paragraphStart<=notes.size()){
    size_t paragraphEnd=notes.find('\n',paragraphStart);
    if(paragraphEnd==std::string::npos) paragraphEnd=notes.size();
    std::string paragraph=notes.substr(paragraphStart,paragraphEnd-paragraphStart);
    if(!paragraph.empty()&&paragraph.back()=='\r') paragraph.pop_back();
    for(char &value:paragraph) if(value=='\t'||(unsigned char)value<0x20) value=' ';
    while(!paragraph.empty()&&paragraph.back()==' ') paragraph.pop_back();
    if(paragraph.empty()) lines.emplace_back();
    else {
      bool continuation=false;
      while(!paragraph.empty()){
        while(!paragraph.empty()&&paragraph.front()==' ') paragraph.erase(paragraph.begin());
        if(paragraph.empty()) break;
        const std::string prefix=continuation&&paragraph.rfind("- ",0)!=0?"  ":"";
        if(textW(g_font_sm,(prefix+paragraph).c_str())<=maxWidth){ lines.push_back(prefix+paragraph); break; }
        const auto boundaries=utf8Boundaries(paragraph);
        size_t low=1,high=boundaries.size()-1;
        while(low<high){
          size_t middle=(low+high+1)/2;
          if(textW(g_font_sm,(prefix+paragraph.substr(0,boundaries[middle])).c_str())<=maxWidth) low=middle;
          else high=middle-1;
        }
        size_t split=boundaries[low];
        size_t space=paragraph.rfind(' ',split);
        if(space!=std::string::npos&&space>0&&space>=split/3) split=space;
        lines.push_back(prefix+paragraph.substr(0,split));
        paragraph.erase(0,split);
        continuation=true;
      }
    }
    if(paragraphEnd==notes.size()) break;
    paragraphStart=paragraphEnd+1;
  }
  if(lines.empty()) lines.emplace_back("No release notes were provided.");
  return lines;
}

static void requestLauncherExitAfterUpdate() {
  g_exitRequested=true;
}

static void runUpdateScreen() {
  if(!g_griddbReady){
    modalMessageStatic("Update check unavailable",{
      "The launcher could not initialize its network connection.",
      "Check the connection and try again."
    });
    return;
  }
  LauncherUpdateSnapshot initial=LauncherUpdate_GetSnapshot();
  if(initial.state==LauncherUpdateState::Idle)
    LauncherUpdate_StartCheck(installedReleaseTag());

  int scroll=0;
  bool cancelRequested=false,installedSaved=false;
  std::string wrappedTag,wrappedBody;
  std::vector<std::string> wrappedLines;
  beginScreenFx();
  for(;;){
    if(!beginUiFrame()){
      LauncherUpdate_Cancel();
      return;
    }
    LauncherUpdateSnapshot snapshot=LauncherUpdate_GetSnapshot();
    if(snapshot.state==LauncherUpdateState::ReadyToInstall){
      if(g_romfsReady){ romfsExit(); g_romfsReady=false; }
      LauncherUpdate_InstallDownloaded(g_launcherNroPath);
      snapshot=LauncherUpdate_GetSnapshot();
      if(snapshot.state==LauncherUpdateState::Error&&!g_romfsReady&&R_SUCCEEDED(romfsInit()))
        g_romfsReady=true;
    }
    if(snapshot.state==LauncherUpdateState::Installed&&!installedSaved){
      storeSet(g_global,"Wrapper/InstalledReleaseTag",snapshot.release.tag.c_str());
      storeSave(g_global,LAUNCHER_INI);
      g_updateNoticeTag.clear();
      installedSaved=true;
    }

    const int panelWidth=SW*7/8,panelHeight=SH*4/5;
    const int panelX=(SW-panelWidth)/2,panelY=(SH-panelHeight)/2;
    const int bodyX=panelX+42,bodyY=panelY+126,bodyWidth=panelWidth-84;
    const int footerHeight=108,bodyBottom=panelY+panelHeight-footerHeight;
    const int lineHeight=fontHeight(g_font_sm)+8;
    const int visibleLines=std::max(1,(bodyBottom-bodyY)/lineHeight);
    if(snapshot.release.tag!=wrappedTag||snapshot.release.notes!=wrappedBody){
      wrappedTag=snapshot.release.tag;
      wrappedBody=snapshot.release.notes;
      wrappedLines=wrapReleaseNotes(wrappedBody.empty()?"Release notes will appear here.":wrappedBody,bodyWidth-20);
      scroll=0;
    }
    const int maxScroll=std::max(0,(int)wrappedLines.size()-visibleLines);
    scroll=std::max(0,std::min(scroll,maxScroll));

    SDL_Event event;
    navRepeat();
    while(pollUiEvent(event)){
      pumpStick(event);
      int tx=0,ty=0; TouchKind touch=touchFeed(event,&tx,&ty);
      if(touch==TOUCH_SCROLL_UP) scroll=std::min(maxScroll,scroll+std::max(1,g_touchScrollSteps));
      else if(touch==TOUCH_SCROLL_DOWN) scroll=std::max(0,scroll-std::max(1,g_touchScrollSteps));
      if(event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP) scroll=std::max(0,scroll-1);
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN) scroll=std::min(maxScroll,scroll+1);
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_LEFTSHOULDER) scroll=std::max(0,scroll-visibleLines);
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) scroll=std::min(maxScroll,scroll+visibleLines);
      else if(event.cbutton.button==BTN_CANCEL){
        if(snapshot.state==LauncherUpdateState::Downloading){ LauncherUpdate_Cancel(); cancelRequested=true; }
        else if(snapshot.state!=LauncherUpdateState::Installed) return;
      } else if(event.cbutton.button==BTN_CONFIRM){
        if(snapshot.state==LauncherUpdateState::UpdateAvailable){
          if(LauncherUpdate_StartDownload(g_launcherNroPath)) cancelRequested=false;
        } else if(snapshot.state==LauncherUpdateState::Error||snapshot.state==LauncherUpdateState::Cancelled){
          cancelRequested=false;
          LauncherUpdate_StartCheck(installedReleaseTag());
        } else if(snapshot.state==LauncherUpdateState::Installed){
          requestLauncherExitAfterUpdate();
          return;
        }
      }
    }

    snapshot=LauncherUpdate_GetSnapshot();
    clearUiBackground();
    fillRect(0,0,SW,SH,(SDL_Color){0,0,0,105});
    glassPanel(panelX,panelY,panelWidth,panelHeight);
    drawText(g_font_big,panelX+28,panelY+18,
             fittedText(g_font_big,std::string(LauncherLocalization::Translate("Cemu Update")),panelWidth-56).c_str(),
             COL_TXT);
    fillRect(panelX+28,panelY+68,panelWidth-56,1,(SDL_Color){255,255,255,24});

    std::string status;
    switch(snapshot.state){
      case LauncherUpdateState::Idle: status="Ready to check for updates"; break;
      case LauncherUpdateState::Checking: status="Checking GitHub for the latest release..."; break;
      case LauncherUpdateState::UpdateAvailable:
        status="Version "+snapshot.release.tag+" is available    Installed: "+installedReleaseTag(); break;
      case LauncherUpdateState::UpToDate:
        status="You are up to date    Installed: "+installedReleaseTag(); break;
      case LauncherUpdateState::Downloading:
        status=cancelRequested?"Cancelling download...":"Downloading "+snapshot.release.assetName; break;
      case LauncherUpdateState::ReadyToInstall: status="Preparing installation..."; break;
      case LauncherUpdateState::Installing: status="Installing update..."; break;
      case LauncherUpdateState::Installed: status="Update installed successfully - relaunch Cemu manually"; break;
      case LauncherUpdateState::Cancelled: status="Update cancelled"; break;
      case LauncherUpdateState::Error: status=snapshot.error.empty()?"Update failed":snapshot.error; break;
    }
    drawScrollTextL(g_font_sm,bodyX,panelY+92,bodyWidth,status.c_str(),
      snapshot.state==LauncherUpdateState::Error?(SDL_Color){235,125,115,255}:COL_VAL);

    SDL_Rect clip={bodyX,bodyY,bodyWidth,bodyBottom-bodyY};
    SDL_RenderSetClipRect(g_ren,&clip);
    for(int row=0;row<visibleLines&&scroll+row<(int)wrappedLines.size();row++)
      drawText(g_font_sm,bodyX,bodyY+row*lineHeight,wrappedLines[scroll+row].c_str(),COL_TXT);
    SDL_RenderSetClipRect(g_ren,nullptr);
    if((int)wrappedLines.size()>visibleLines){
      const int trackX=panelX+panelWidth-25,trackHeight=bodyBottom-bodyY;
      fillRect(trackX,bodyY,4,trackHeight,(SDL_Color){40,44,54,255});
      const int thumbHeight=std::max(18,trackHeight*visibleLines/(int)wrappedLines.size());
      fillRect(trackX,bodyY+(trackHeight-thumbHeight)*scroll/std::max(1,maxScroll),4,thumbHeight,COL_SEL);
    }

    if(snapshot.state==LauncherUpdateState::Downloading){
      const uint64_t total=snapshot.total?snapshot.total:snapshot.release.assetSize;
      const int percent=total?(int)std::min<uint64_t>(100,snapshot.downloaded*100/total):0;
      const int barX=bodyX,barY=panelY+panelHeight-82,barWidth=bodyWidth,barHeight=24;
      drawProgressBar(barX,barY,barWidth,barHeight,total?(double)snapshot.downloaded/total:0.0);
      char progress[96];
      snprintf(progress,sizeof(progress),"%d%%    %.1f / %.1f MiB",percent,
        snapshot.downloaded/(1024.0*1024.0),total/(1024.0*1024.0));
      drawTextC(g_font_sm,SW/2,barY+30,progress,COL_DIM);
    } else {
      std::string controls;
      if(snapshot.state==LauncherUpdateState::UpdateAvailable) controls="A  Download       B  Back";
      else if(snapshot.state==LauncherUpdateState::Error||snapshot.state==LauncherUpdateState::Cancelled) controls="A  Retry       B  Back";
      else if(snapshot.state==LauncherUpdateState::Installed) controls="A  Exit Cemu";
      else controls="B  Back";
      if(maxScroll>0) controls+="       Up / Down  Scroll       L / R  Page";
      drawLocalizedFooter(controls.c_str(),panelY+panelHeight-38);
    }
    drawFadeIn();
    SDL_RenderPresent(g_ren);
    waitForNextUiFrame();
  }
}

static void pollUpdateNotification() {
  const LauncherUpdateSnapshot snapshot=LauncherUpdate_GetSnapshot();
  if(snapshot.state==LauncherUpdateState::UpdateAvailable&&!snapshot.release.tag.empty()&&
     snapshot.release.tag!=g_updateNotifiedTag){
    g_updateNotifiedTag=snapshot.release.tag;
    g_updateNoticeTag=snapshot.release.tag;
    g_updateNoticeUntil=SDL_GetTicks()+9000;
  }
}

static void drawUpdateNotification() {
  if(g_updateNoticeTag.empty()||SDL_TICKS_PASSED(SDL_GetTicks(),g_updateNoticeUntil)){
    g_updateNoticeTag.clear();
    return;
  }
  const int width=std::min(540,SW-40),height=92,x=SW-width-24,y=SH-height-58;
  roundedPanel(x,y,width,height,COL_CARD,COL_SEL);
  const std::string title="Cemu "+g_updateNoticeTag+" is available";
  drawText(g_font,x+22,y+16,ellipsizedText(g_font,title,width-44).c_str(),COL_VAL);
  drawStaticText(g_font_sm,x+22,y+54,"Open Settings > Launcher > Check for Updates",COL_TXT);
}

static const char *gridDbErrorText(int result) {
  if(result==GRIDDB_NO_KEY) return "The SteamGridDB API key was rejected.";
  if(result==GRIDDB_NO_NET) return "Could not connect to SteamGridDB.";
  if(result==GRIDDB_NOT_FOUND) return tr("No matching artwork was found.");
  return "SteamGridDB returned an unexpected error.";
}

template <typename Work>
static bool runCancellableNetworkTask(const char *title,const char *detail,Work work) {
  std::atomic_bool cancel{false},complete{false};
  std::thread worker([&]{ work(cancel); complete=true; wakeUiFromWorker(0x4e455457); });
  while(!complete.load(std::memory_order_acquire)){
    if(!beginUiFrame()){ cancel=true; break; }
    SDL_Event event;
    while(pollUiEvent(event)){
      pumpStick(event);
      int x=0,y=0;
      if((event.type==SDL_CONTROLLERBUTTONDOWN&&event.cbutton.button==BTN_CANCEL)||
         (touchFeed(event,&x,&y)==TOUCH_TAP&&y>=SH-90)) cancel=true;
    }
    clearUiBackground();
    drawHeader(title,nullptr);
    const int panelWidth=std::min(940,SW-64),lineHeight=fontHeight(g_font)+8;
    const auto lines=wrapDialogLines({cancel.load()?"Cancelling...":(detail?detail:"")},panelWidth-64);
    const int panelHeight=std::max(140,(int)lines.size()*lineHeight+64);
    glassPanel((SW-panelWidth)/2,(SH-panelHeight)/2,panelWidth,panelHeight);
    int lineY=(SH-(int)lines.size()*lineHeight)/2;
    for(const std::string &line:lines){ drawTextC(g_font,SW/2,lineY,line.c_str(),COL_TXT); lineY+=lineHeight; }
    drawLocalizedFooter("B  Cancel");
    SDL_RenderPresent(g_ren); waitForNextUiFrame();
  }
  cancel = cancel.load() || !complete.load();
  if(worker.joinable()) worker.join();
  return !cancel.load();
}

struct GameDetailLayout { SDL_Rect preview,content; };
static GameDetailLayout gameDetailLayout(){
  const int top=topBarH()+24,bottom=SH-settingsFooterReserve()-12;
  const int width=260,height=width*3/2;
  return {{56,top+std::max(0,(bottom-top-height)/2),width,height},{360,top,SW-416,bottom-top}};
}
static void drawArtworkPreview(SDL_Texture *texture,const SDL_Rect &rect,bool selected=false){
  if(selected) roundedPanel(rect.x-10,rect.y-10,rect.w+20,rect.h+20,COL_PANEL,COL_SEL);
  else glassPanel(rect.x-10,rect.y-10,rect.w+20,rect.h+20);
  fillRect(rect.x,rect.y,rect.w,rect.h,COL_CARD);
  if(texture){
    SDL_SetTextureAlphaMod(texture,255); SDL_SetTextureColorMod(texture,255,255,255);
    int w=0,h=0;SDL_QueryTexture(texture,nullptr,nullptr,&w,&h);
    const SDL_Rect fitted=fitArtworkRect(w,h,rect);
    SDL_RenderCopy(g_ren,texture,nullptr,&fitted);
  } else {
    drawTextC(g_font_sm,rect.x+rect.w/2,rect.y+(rect.h-fontHeight(g_font_sm))/2,
              tr("NO COVER"),COL_DIM);
  }
}
static void drawGamePreview(Game &game,const SDL_Rect &rect){
  g_cover_budget=1;ensureCover(game);
  drawArtworkPreview(game.cover,rect);
}

static GameDetailLayout artworkPickerLayout(bool background){
  if(!background) return gameDetailLayout();
  const int top=topBarH()+24,bottom=SH-settingsFooterReserve()-12;
  const int width=680,height=width*9/16;
  return {{40,top+(bottom-top-height)/2,width,height},{768,top,SW-824,bottom-top}};
}

static void drawArtworkPicker(const std::vector<GridDbArtwork> &artworks,const char *gameName,
                              bool background,int sel,int top,SDL_Texture *preview,bool previewFailed){
  const GameDetailLayout layout=artworkPickerLayout(background);
  const int rowHeight=settingsRowH()+8,listX=layout.content.x,listWidth=layout.content.w;
  const int startY=layout.content.y+44,visible=std::max(1,(layout.content.h-52)/rowHeight);
  clearUiBackground(); drawHeader(tr(background?"Choose background artwork":"Choose cover artwork"),gameName);
  drawSectionHeading("Online artwork",listX,startY-44,listWidth);
  glassPanel(listX-8,startY-8,listWidth+16,std::min(visible,(int)artworks.size())*rowHeight+16);
  for(int row=0;row<visible&&top+row<(int)artworks.size();row++){
    int index=top+row,itemY=startY+row*rowHeight,textY=itemY+(rowHeight-fontHeight(g_font))/2; bool current=index==sel;
    if(current) drawRowHighlight(listX,itemY,listWidth,rowHeight-3);
    std::string label=uiText("Artwork")+" "+std::to_string(index+1);
    drawText(g_font,listX+26,textY,label.c_str(),current?COL_VAL:COL_TXT);
    if(artworks[index].width>0&&artworks[index].height>0){
      std::string dimensions=std::to_string(artworks[index].width)+"x"+std::to_string(artworks[index].height);
      drawTextR(g_font_sm,listX+listWidth-20,textY+(fontHeight(g_font)-fontHeight(g_font_sm))/2,dimensions.c_str(),current?COL_VAL:COL_DIM);
    }
  }
  drawArtworkPreview(preview,layout.preview,preview!=nullptr);
  if(previewFailed){
    const SDL_Rect &rect=layout.preview;
    fillRect(rect.x,rect.y,rect.w,rect.h,COL_CARD);
    drawWrapped(g_font_sm,rect.x+16,rect.y+rect.h/2-30,rect.w-32,fontHeight(g_font_sm)+6,3,
          tr("Preview unavailable"),COL_DIM);
  }
  FootItem footer[]={{"A","Use artwork"},{"B",background?"Skip background":"Back"}};
  drawFooterHints(footer,2,SH-26);
}

static int chooseCoverArtwork(const std::vector<GridDbArtwork> &artworks,const char *gameName,bool background=false) {
  if(artworks.empty()) return -1;
  const GameDetailLayout layout=artworkPickerLayout(background);
  const int rowHeight=settingsRowH()+8,listX=layout.content.x,listWidth=layout.content.w;
  const int startY=layout.content.y+44;
  const int visible=std::max(1,(layout.content.h-52)/rowHeight);
  const int previewWidth=layout.preview.w,previewHeight=layout.preview.h;
  const std::string temporary=std::string(COVERS_DIR)+"/.sgdb-preview.img";
  int sel=0,top=0,loaded=-1;
  SDL_Texture *preview=nullptr;
  bool previewFailed=false;
  auto releasePreview=[&](){ if(preview) SDL_DestroyTexture(preview); preview=nullptr; remove(temporary.c_str()); };
  auto loadPreview=[&](int index){
    releasePreview(); loaded=index; previewFailed=false;
    const std::string &url=artworks[index].thumbnailUrl.empty()?artworks[index].url:artworks[index].thumbnailUrl;
    int result=GRIDDB_CANCELLED;
    runCancellableNetworkTask("Loading artwork preview",gameName?gameName:"",
      [&](const std::atomic_bool &cancel){ result=griddb_download_image(url,temporary,&cancel); });
    if(result==GRIDDB_OK) preview=loadScaledTexture(temporary,previewWidth,previewHeight,true);
    previewFailed=preview==nullptr; remove(temporary.c_str()); beginScreenFx();
  };
  mkdir(COVERS_DIR,0777);
  loadPreview(0);
  for(;;){
    if(!beginUiFrame()){ releasePreview(); return -1; }
    SDL_Event event; navRepeat();
    while(pollUiEvent(event)){
      pumpStick(event); int tx=0,ty=0; TouchKind touch=touchFeed(event,&tx,&ty); int oldSelection=sel;
      if(touchScrollList(touch,sel,top,(int)artworks.size(),visible)){ if(sel!=oldSelection) loadPreview(sel); continue; }
      if(touch==TOUCH_TAP){
        if(ty>=SH-48){ releasePreview(); return -1; }
        if(tx>=listX&&tx<listX+listWidth) for(int row=0;row<visible&&top+row<(int)artworks.size();row++){
          int itemY=startY+row*rowHeight;
          if(ty>=itemY&&ty<itemY+rowHeight){ sel=top+row; if(loaded!=sel) loadPreview(sel); break; }
        }
        continue;
      }
      if(event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      int previous=sel;
      if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP) sel=(sel+(int)artworks.size()-1)%(int)artworks.size();
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN) sel=(sel+1)%(int)artworks.size();
      else if(event.cbutton.button==BTN_CONFIRM){ releasePreview(); return sel; }
      else if(event.cbutton.button==BTN_CANCEL){ releasePreview(); return -1; }
      if(sel<top) top=sel;
      if(sel>=top+visible) top=sel-visible+1;
      if(sel!=previous) loadPreview(sel);
    }
    drawArtworkPicker(artworks,gameName,background,sel,top,loaded==sel?preview:nullptr,loaded==sel&&previewFailed);
    drawFadeIn(); SDL_RenderPresent(g_ren); waitForNextUiFrame();
  }
}

static void downloadCover(Game &g) {
  std::string key=steamGridDbKey();
  if (key.empty()) {
    if(promptAndSaveSteamGridDbKey("Enter your free SteamGridDB API key",false)) key=steamGridDbKey();
    else { toastStatic("A SteamGridDB API key is required"); return; }
  }
  mkdir(COVERS_DIR, 0777);
  std::string query=g.title;
  GridDbGameResult selectedGame;
  for(;;){
    toastStatic("Searching SteamGridDB...");
    std::vector<GridDbGameResult> matches;
    int searchResult=GRIDDB_CANCELLED;
    if(!runCancellableNetworkTask("Download cover","Searching SteamGridDB...",
       [&](const std::atomic_bool &cancel){ searchResult=griddb_search_games(key,query,matches,&cancel); })) return;
    if(searchResult==GRIDDB_NO_KEY){
      if(!promptAndSaveSteamGridDbKey("SteamGridDB API key rejected",false)) return;
      key=steamGridDbKey();
      continue;
    }
    if(searchResult!=GRIDDB_OK&&searchResult!=GRIDDB_NOT_FOUND){
      modalMessageStatic("Cover search failed",{std::string(gridDbErrorText(searchResult))});
      return;
    }
    std::vector<std::string> labels={"Custom search..."};
    for(const auto &match:matches) labels.push_back(match.name);
    std::vector<const char*> names;
    names.reserve(labels.size());
    for(const auto &label:labels) names.push_back(label.c_str());
    int gameIndex=dropdownStaticTitle("Choose matching title",names.data(),(int)names.size(),-1);
    if(gameIndex<0) return;
    if(gameIndex==0){
      char custom[256];
      if(!promptTextStatic("Custom SteamGridDB search",query.c_str(),custom,sizeof(custom))) continue;
      std::string nextQuery=trim(custom);
      if(!nextQuery.empty()) query=std::move(nextQuery);
      continue;
    }
    selectedGame=matches[gameIndex-1];
    break;
  }

  toastStatic("Loading available artwork...");
  std::vector<GridDbArtwork> artworks;
  int result=GRIDDB_CANCELLED;
  if(!runCancellableNetworkTask("Download cover","Loading available artwork...",
     [&](const std::atomic_bool &cancel){ result=griddb_fetch_artworks(key,selectedGame.id,artworks,&cancel); })) return;
  if(result!=GRIDDB_OK){ modalMessageStatic("Artwork search failed",{std::string(gridDbErrorText(result))}); return; }
  int artworkIndex=chooseCoverArtwork(artworks,selectedGame.name.c_str());
  if(artworkIndex<0) return;

  toastStatic("Downloading selected cover...");
  const bool continueArtwork=runCancellableNetworkTask("Download cover","Downloading selected cover...",
     [&](const std::atomic_bool &cancel){ result=griddb_download_image(artworks[artworkIndex].url,coverPath(g),&cancel); });
  if(result!=GRIDDB_OK){ if(continueArtwork) toastStatic("Cover download failed"); return; }
  reloadCover(g);
  storeSet(g_global,artworkKey(g,"GameId").c_str(),std::to_string(selectedGame.id).c_str());
  storeSet(g_global,artworkKey(g,"BackgroundMissing").c_str(),"false");
  storeSave(g_global,LAUNCHER_INI);
  if(!continueArtwork) return;
  std::vector<GridDbArtwork> backgrounds;
  if(!runCancellableNetworkTask("Loading background artwork",selectedGame.name.c_str(),
     [&](const std::atomic_bool &cancel){ result=griddb_fetch_backgrounds(key,selectedGame.id,backgrounds,&cancel); })) return;
  if(result==GRIDDB_NOT_FOUND){
    storeSet(g_global,artworkKey(g,"BackgroundMissing").c_str(),"true");
    storeSave(g_global,LAUNCHER_INI);
    toastStatic("Cover downloaded. No background artwork was found."); return;
  }
  if(result!=GRIDDB_OK){
    modalMessageStatic("Background search failed",{"Cover downloaded",uiText(gridDbErrorText(result))}); return;
  }
  const int backgroundIndex=chooseCoverArtwork(backgrounds,selectedGame.name.c_str(),true);
  if(backgroundIndex<0){ toastStatic("Cover downloaded"); return; }
  runCancellableNetworkTask("Downloading selected background",selectedGame.name.c_str(),
     [&](const std::atomic_bool &cancel){ result=griddb_download_image(backgrounds[backgroundIndex].url,backgroundPath(g),&cancel); });
  if(result==GRIDDB_OK){ reloadBackground(g); toastStatic("Cover and background downloaded"); }
  else if(result!=GRIDDB_CANCELLED)
    modalMessageStatic("Background download failed",{"Cover downloaded",uiText(gridDbErrorText(result))});
}

static void importCoverFromFile(Game &g){
  const std::string selected=browseCoverImage(parentFolder(g.path));if(selected.empty())return;
  mkdir(COVERS_DIR,0777);const std::string destination=coverPath(g),temporary=destination+".tmp";
  bool imported=false;std::string reason,detail;
  if(!runCancellableNetworkTask("Importing local cover",fileNameOf(selected).c_str(),[&](const std::atomic_bool &cancel){
    const auto fail=[&](const char *message,const char *technical=nullptr){reason=message;if(technical)detail=technical;remove(temporary.c_str());};
    struct stat info{};if(cancel.load())return;
    if(stat(selected.c_str(),&info)!=0||!S_ISREG(info.st_mode)){fail("The selected cover file is unavailable.",strerror(errno));return;}
    if(info.st_size<1||(uint64_t)info.st_size>32ull*1024*1024){fail("The selected cover file is too large.");return;}
    if(!recoverAtomicFile(destination)){fail("Cemu could not prepare the cover file safely.",strerror(errno));return;}
    using Surface=std::unique_ptr<SDL_Surface,decltype(&SDL_FreeSurface)>;Surface source{IMG_Load(selected.c_str()),SDL_FreeSurface};
    if(!source){fail("The selected file is not a supported image.",IMG_GetError());return;}
    if(source->w<=0||source->h<=0||source->w>8192||source->h>8192||(uint64_t)source->w*(uint64_t)source->h>16ull*1024*1024){fail("The selected image dimensions are too large.");return;}
    if(cancel.load())return;
    Surface converted{SDL_ConvertSurfaceFormat(source.get(),SDL_PIXELFORMAT_RGBA32,0),SDL_FreeSurface};source.reset();
    if(!converted||IMG_SavePNG(converted.get(),temporary.c_str())!=0){fail("Cemu could not convert the selected image to PNG.",IMG_GetError());return;}
    converted.reset();if(cancel.load()){remove(temporary.c_str());return;}Surface verify{IMG_Load(temporary.c_str()),SDL_FreeSurface};
    if(!verify||verify->w<=0||verify->h<=0){fail("Cemu could not verify the converted cover.",IMG_GetError());return;}verify.reset();
    FILE *saved=fopen(temporary.c_str(),"rb+");if(!saved){fail("Cemu could not save the converted cover.",strerror(errno));return;}
    const bool synced=fsync(fileno(saved))==0,closed=fclose(saved)==0;if(!synced||!closed){fail("Cemu could not save the converted cover.",strerror(errno));return;}
    if(cancel.load()){remove(temporary.c_str());return;}if(!replaceAtomic(destination,temporary)){fail("Cemu could not replace the current cover safely.",strerror(errno));return;}imported=true;
  }))return;
  if(imported){reloadCover(g);toastStatic("Cover imported");return;}
  std::vector<std::string> lines{std::string(LauncherLocalization::Translate(reason.empty()?"The selected cover could not be imported safely.":reason))};if(!detail.empty())lines.push_back(detail);
  modalMessage(LauncherLocalization::Translate("Cover import failed").data(),lines);
}

static void coverSettings(Game &g){
  int selection=0;const GameDetailLayout layout=gameDetailLayout();
  const int gap=20,cardH=(layout.content.h-gap)/2;
  const SDL_Rect cards[2]={{layout.content.x,layout.content.y,layout.content.w,cardH},
                           {layout.content.x,layout.content.y+cardH+gap,layout.content.w,cardH}};
  const char *titles[2]={"Download from SteamGridDB","Import cover from file"};const char *kinds[2]={"Online artwork","Local image"};
  const char *descriptions[2]={"Choose a game cover, then select optional background artwork for the Classic theme.","Choose a PNG, JPEG, WebP or BMP image from SD, USB or SMB storage. It is validated and saved safely as PNG."};
  const auto inside=[](const SDL_Rect&r,int x,int y){return x>=r.x&&x<r.x+r.w&&y>=r.y&&y<r.y+r.h;};
  const auto removeCustom=[&]{const std::string path=coverPath(g);if(!regularFileExists(path)||!confirmBoxStatic("Remove custom cover?",{"The downloaded or imported cover will be deleted.","The launcher will use the game's embedded artwork when available."}))return;
    if(remove(path.c_str())!=0&&errno!=ENOENT)modalMessageStatic("Cover removal failed",{std::string(strerror(errno))});else{fsdevCommitDevice("sdmc");reloadCover(g);toastStatic("Custom cover removed");}};
  // Hoisted out of the frame loop: this used to stat the cover file every frame.
  bool hasCustom=regularFileExists(coverPath(g));
  beginScreenFx();for(;;){
    if(!beginUiFrame())return;
    SDL_Event event;navRepeat();
    while(pollUiEvent(event)){pumpStick(event);int tx=0,ty=0;TouchKind touch=touchFeed(event,&tx,&ty);bool choose=false;
      if(touch==TOUCH_TAP){if(inside(cards[0],tx,ty)){selection=0;choose=true;}else if(inside(cards[1],tx,ty)){selection=1;choose=true;}else if(ty>=SH-40)return;}
      if(event.type==SDL_CONTROLLERBUTTONDOWN){
        if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_LEFT||event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP)selection=0;
        else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_RIGHT||event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN)selection=1;
        else if(event.cbutton.button==BTN_CONFIRM)choose=true;
        else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_X&&hasCustom){removeCustom();hasCustom=regularFileExists(coverPath(g));beginScreenFx();}
        else if(event.cbutton.button==BTN_CANCEL)return;}
      if(choose){if(selection==0)downloadCover(g);else importCoverFromFile(g);hasCustom=regularFileExists(coverPath(g));beginScreenFx();}}
    clearUiBackground();drawHeaderStatic("Cover settings",g.title.c_str());
    drawGamePreview(g,layout.preview);
    for(int index=0;index<2;index++){
      const SDL_Rect&card=cards[index];const bool current=index==selection;
      glassPanel(card.x,card.y,card.w,card.h);
      if(current)drawRowHighlight(card.x+8,card.y+8,card.w-16,card.h-16);
      const int textX=card.x+28,textWidth=card.w-56;
      drawText(g_font_sm,textX,card.y+24,LauncherLocalization::Translate(kinds[index]).data(),COL_HI);
      drawScrollTextL(g_font,textX,card.y+62,textWidth,
                      LauncherLocalization::Translate(titles[index]).data(),current?COL_VAL:COL_TXT);
      drawWrapped(g_font_sm,textX,card.y+112,textWidth,fontHeight(g_font_sm)+7,
                  std::max(1,(card.h-132)/(fontHeight(g_font_sm)+7)),
                  LauncherLocalization::Translate(descriptions[index]).data(),COL_DIM);
    }
    if(hasCustom){
      FootItem footer[]={{"A",LauncherLocalization::Translate("Choose").data()},
                         {"Y",LauncherLocalization::Translate("Remove custom cover").data()},
                         {"B",LauncherLocalization::Translate("Back").data()}};
      drawFooterHints(footer,3,SH-26);
    } else {
      FootItem footer[]={{"A",LauncherLocalization::Translate("Choose").data()},
                         {"B",LauncherLocalization::Translate("Back").data()}};
      drawFooterHints(footer,2,SH-26);
    }
    drawFadeIn();SDL_RenderPresent(g_ren);waitForNextUiFrame();}
}

static void downloadAllCovers() {
  std::string key=steamGridDbKey();
  if(key.empty()){
    if(promptAndSaveSteamGridDbKey("Enter your free SteamGridDB API key",false)) key=steamGridDbKey();
    else { toastStatic("A SteamGridDB API key is required"); return; }
  }
  mkdir(COVERS_DIR,0777);
  struct CoverJob { std::string gameKey,title,cover,background; long id=0; };
  struct ArtworkResult { std::string gameKey; long id=0; bool cover=false,background=false,backgroundMissing=false; };
  std::vector<CoverJob> pending;
  for(const Game &game:g_games){
    const bool cover=!regularFileExists(existingCoverPath(game));
    const bool background=!regularFileExists(backgroundPath(game))&&
        strcmp(storeGet(g_global,artworkKey(game,"BackgroundMissing").c_str(),"false"),"true");
    if(cover||background)
      pending.push_back({game.key,game.title,cover?coverPath(game):std::string{},background?backgroundPath(game):std::string{},
                        strtol(storeGet(g_global,artworkKey(game,"GameId").c_str(),"0"),nullptr,10)});
  }
  if(pending.empty()){ toastStatic("All artwork already downloaded"); return; }
  std::atomic_int ok{0},fail{0};
  std::vector<ArtworkResult> downloaded;
  const std::string detail=std::to_string(pending.size())+" "+uiText("games queued");
  std::atomic_int lastResult{GRIDDB_OK};
  const bool finished=runCancellableNetworkTask("Downloading artwork",detail.c_str(),[&](const std::atomic_bool &taskCancel){
    for(const auto &job:pending){
      if(taskCancel.load(std::memory_order_acquire)) break;
      ArtworkResult saved;saved.gameKey=job.gameKey;saved.id=job.id;
      int result=GRIDDB_OK;
      if(saved.id<=0){
        std::vector<GridDbGameResult> matches;
        result=griddb_search_games(key,job.title,matches,&taskCancel);
        if(result==GRIDDB_OK) saved.id=matches.front().id;
      }
      if(result!=GRIDDB_OK){
        lastResult=result;
        if(result!=GRIDDB_CANCELLED) fail.fetch_add(1,std::memory_order_relaxed);
        if(result==GRIDDB_NO_KEY) break;
        continue;
      }
      bool failed=false;
      if(!job.cover.empty()){
        std::vector<GridDbArtwork> artworks;
        result=griddb_fetch_artworks(key,saved.id,artworks,&taskCancel);
        if(result==GRIDDB_OK) result=griddb_download_image(artworks.front().url,job.cover,&taskCancel);
        saved.cover=result==GRIDDB_OK;
        failed=result!=GRIDDB_OK&&result!=GRIDDB_CANCELLED;
      }
      if(!job.background.empty()&&!taskCancel.load(std::memory_order_acquire)){
        result=griddb_fetch_background(key,saved.id,job.background,&taskCancel);
        saved.background=result==GRIDDB_OK;saved.backgroundMissing=result==GRIDDB_NOT_FOUND;
        failed|=result!=GRIDDB_OK&&result!=GRIDDB_NOT_FOUND&&result!=GRIDDB_CANCELLED;
      }
      if(saved.cover||saved.background) ok.fetch_add(1,std::memory_order_relaxed);
      if(failed) fail.fetch_add(1,std::memory_order_relaxed);
      downloaded.emplace_back(std::move(saved));
      lastResult=result;
      if(result==GRIDDB_NO_KEY) break;
    }
  });
  for(const auto &saved:downloaded) if(Game *game=findGameByKey(saved.gameKey)){
    storeSet(g_global,artworkKey(*game,"GameId").c_str(),std::to_string(saved.id).c_str());
    if(saved.background||saved.backgroundMissing)
      storeSet(g_global,artworkKey(*game,"BackgroundMissing").c_str(),saved.backgroundMissing?"true":"false");
    if(saved.cover) reloadCover(*game);
    if(saved.background) reloadBackground(*game);
  }
  storeSave(g_global,LAUNCHER_INI);
  if(lastResult.load()==GRIDDB_NO_KEY)
    modalMessageStatic("SteamGridDB API key rejected",{"Update the saved API key in Launcher settings."});
  char message[128]; snprintf(message,sizeof(message),tr("Artwork: %d updated, %d failed%s"),ok.load(),fail.load(),finished?"":tr(" (cancelled)"));
  toast(message);
}

static bool writeEnabledPacks(const std::vector<CemuGraphicPack> &enabled) {
  std::string text;
  auto valid = [](const std::string &value) { return value.find_first_of("\t\r\n") == std::string::npos; };
  for (auto &pk : enabled) {
    if (pk.rulesRel.empty() || !valid(pk.rulesRel)) return false;
    text += pk.rulesRel;
    for (auto &pr : pk.presets) {
      if (!valid(pr.category) || pr.category.find('=') != std::string::npos || !valid(pr.preset)) return false;
      text += '\t';
      text += pr.category;
      text += '=';
      text += pr.preset;
    }
    text += '\n';
  }
  return writeAtomicText(ENABLED_PACKS_FILE, text);
}

struct PackSel { bool enabled = false; std::vector<std::pair<std::string, std::string>> choices; };

static std::vector<std::string> gfxSplitPath(const std::string &s) {
  std::vector<std::string> t; size_t a = 0;
  while (a <= s.size()) { size_t sl = s.find('/', a); std::string x = s.substr(a, sl == std::string::npos ? s.size() - a : sl - a);
    if (!x.empty()) t.push_back(x);
    if (sl == std::string::npos) break;
    a = sl + 1;
  }
  return t;
}
static std::string gfxGameOf(const GfxPack &p) { auto t = gfxSplitPath(p.path); return t.empty() ? p.name : t[0]; }
static void gfxCatLeaf(const GfxPack &p, std::string &cat, std::string &leaf) {
  auto t = gfxSplitPath(p.path);
  if (t.size() >= 3) { cat = t[1]; leaf.clear(); for (size_t i = 2; i < t.size(); i++) { if (!leaf.empty()) leaf += " / "; leaf += t[i]; } }
  else if (t.size() == 2) { cat = "General"; leaf = t[1]; }
  else { cat = "General"; leaf = t.empty() ? p.name : t[0]; }
  if (leaf.empty()) leaf = p.name;
}
struct GRow { bool header; std::string label; int packIndex; };
static std::vector<GRow> gfxBuildGameRows(const std::vector<GfxPack> &packs, const std::string &gameName, bool perGame) {
  struct It { std::string cat, leaf; int idx; };
  std::vector<It> items;
  for (int i = 0; i < (int)packs.size(); i++) {
    if (!perGame && gfxGameOf(packs[i]) != gameName) continue;
    std::string cat, leaf; gfxCatLeaf(packs[i], cat, leaf);
    items.push_back({cat, leaf, i});
  }
  std::sort(items.begin(), items.end(), [](const It &a, const It &b) { return a.cat != b.cat ? a.cat < b.cat : a.leaf < b.leaf; });
  std::vector<GRow> rows; std::string curCat = "\x01";
  for (auto &it : items) { if (it.cat != curCat) { curCat = it.cat; rows.push_back({true, curCat, -1}); } rows.push_back({false, it.leaf, it.idx}); }
  return rows;
}
static void drawWrapped(TTF_Font *f, int x, int y, int maxW, int lh, int maxLines, const char *s, SDL_Color c) {
  if (!s || !*s) return;
  std::string text = s, line; int drawn = 0;
  auto emit = [&](const std::string &ln) { if (drawn < maxLines) { drawText(f, x, y + drawn * lh, ln.c_str(), c); drawn++; } };
  size_t i = 0;
  while (i < text.size() && drawn < maxLines) {
    size_t ws = i; while (ws < text.size() && text[ws] != ' ' && text[ws] != '\n') ws++;
    std::string word = text.substr(i, ws - i);
    std::string candidate = line.empty() ? word : line + " " + word;
    if (textW(f, candidate.c_str()) > maxW && !line.empty()) { emit(line); line = word; }
    else line = candidate;
    if (ws < text.size() && text[ws] == '\n') { emit(line); line.clear(); }
    i = ws + 1;
  }
  if (drawn < maxLines && !line.empty()) emit(line);
}
struct GfxCat { std::string category; std::vector<int> presetIdx; };
static std::vector<GfxCat> gfxGroupPresets(const GfxPack &p) {
  std::vector<GfxCat> cats;
  for (int i = 0; i < (int)p.presets.size(); i++) { const std::string &c = p.presets[i].category; int ci = -1;
    for (int k = 0; k < (int)cats.size(); k++) if (cats[k].category == c) { ci = k; break; }
    if (ci < 0) { cats.push_back({c, {}}); ci = (int)cats.size() - 1; } cats[ci].presetIdx.push_back(i); }
  return cats;
}
static std::string gfxCatDefault(const GfxPack &p, const GfxCat &c) {
  for (int i : c.presetIdx) if (p.presets[i].isDefault) return p.presets[i].name;
  return c.presetIdx.empty() ? "" : p.presets[c.presetIdx[0]].name;
}

static int downloadLatestGraphicPacks() {
  toastStatic("Downloading latest graphic packs...");
  appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
  const int result=gfxpacks_downloadLatest(GRAPHICPACKS_DIR);
  appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
  toast(result==GFX_OK?"Packs updated":result==GFX_UPTODATE?"Already up to date":
        result==GFX_NET_FAIL?"Download failed (network)":"Extract failed");
  return result;
}

static void openPackPanel(const GfxPack &p, PackSel &s) {
  auto cats = gfxGroupPresets(p);
  auto choiceRef = [&](const std::string &cat) -> std::string & {
    for (auto &ch : s.choices) if (ch.first == cat) return ch.second;
    s.choices.push_back({cat, ""}); return s.choices.back().second;
  };
  for (auto &c : cats) { auto &ref = choiceRef(c.category); if (ref.empty()) ref = gfxCatDefault(p, c); }
  int row = 0, top = 0, nrows = 1 + (int)cats.size();
  bool hasDesc = !p.description.empty();
  int footerH = hasDesc ? 174 : 54;
  int vis = (SH - LIST_Y0 - 40 - footerH) / ROW_H; if (vis < 1) vis = 1;
  beginScreenFx();
  for (;;) {
    if (!beginUiFrame()) return;
    SDL_Event e;
    navRepeat();
    while (pollUiEvent(e)) {
      pumpStick(e);
      { int tx=0,ty=0; TouchKind touch=touchFeed(e,&tx,&ty);
        if(touchScrollList(touch,row,top,nrows,vis)) continue;
        if(touch==TOUCH_TAP){
          for(int visibleRow=0;visibleRow<vis&&top+visibleRow<nrows;visibleRow++){
            int y=LIST_Y0+visibleRow*ROW_H;
            if(ty>=y&&ty<y+ROW_H){ row=top+visibleRow; SDL_Event press{}; press.type=SDL_CONTROLLERBUTTONDOWN; press.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&press); break; }
          }
          continue;
        }
      }
      if (e.type != SDL_CONTROLLERBUTTONDOWN) continue;
      switch (e.cbutton.button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:   row = (row + nrows - 1) % nrows; break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: row = (row + 1) % nrows; break;
        case BTN_CONFIRM:
          if (row == 0) s.enabled = !s.enabled;
          else if (s.enabled) { const GfxCat &c = cats[row - 1]; auto &cur = choiceRef(c.category);
            std::vector<const char *> labels; int ci = 0;
            for (int k = 0; k < (int)c.presetIdx.size(); k++) { const std::string &nm = p.presets[c.presetIdx[k]].name; labels.push_back(nm.c_str()); if (nm == cur) ci = k; }
            int pick = dropdown(c.category.empty() ? "Options" : c.category.c_str(), labels.data(), (int)labels.size(), ci);
            if (pick >= 0 && pick < (int)c.presetIdx.size()) cur = p.presets[c.presetIdx[pick]].name;
            beginScreenFx();
          }
          break;
        case BTN_X: {
          if(row==0){
            const std::string description=p.description.empty()?
              "Enables or disables this graphics pack and all of its selected presets for the current game.":p.description;
            showHelpCard(p.name.c_str(),"Enabled","Graphics pack",description,
                         s.enabled?"On":"Off","Graphics pack option");
          } else {
            const GfxCat &category=cats[row-1];
            const char *title=category.category.empty()?"Options":category.category.c_str();
            const std::string description="Selects the "+std::string(title)+
              " preset used by this graphics pack. The pack must be enabled for this choice to take effect.";
            showHelpCard(p.name.c_str(),title,"Graphics pack preset",description,
                         choiceRef(category.category).c_str(),"Graphics pack option");
          }
          beginScreenFx();
          break;
        }
        case BTN_CANCEL: return;
      }
      if (row < top) top = row;
      if (row >= top + vis) top = row - vis + 1;
      if (top < 0) top = 0;
    }
    clearUiBackground();
    drawHeader(p.name.c_str(), p.path.c_str());
    int colX, colW, labelX, valX; listCol(&colX, &colW, &labelX, &valX);
    glassPanel(colX-12,LIST_Y0-10,colW+24,std::min(vis,std::max(1,nrows-top))*ROW_H+18);
    float ty = (float)(LIST_Y0 + (row - top) * ROW_H + 1);
    g_hy = (!g_uiAnimations || g_hy < 0) ? ty : g_hy + (ty - g_hy) * 0.30f;
    drawRowHighlight(colX, (int)g_hy, colW, ROW_H - 2);
    SDL_Color grey = {92, 98, 110, 255};
    for (int r = 0; r < vis && top + r < nrows; r++) {
      int i = top + r, slotY = LIST_Y0 + r * ROW_H; bool cur = (i == row);
      if (i == 0) {
        SDL_Color oc = s.enabled ? (SDL_Color){120, 220, 120, 255} : COL_DIM;
        drawSettingsRowText(LauncherLocalization::Translate("Enabled").data(),
                            LauncherLocalization::Translate(s.enabled ? "On" : "Off").data(),
                            slotY, colW, labelX, valX, cur, cur ? COL_VAL : COL_TXT, oc);
      } else { const GfxCat &c = cats[i - 1]; bool en = s.enabled;
        drawSettingsRowText(c.category.empty()?LauncherLocalization::Translate("Options").data():c.category.c_str(),
                            choiceRef(c.category).c_str(), slotY, colW, labelX, valX, cur,
                            en ? (cur ? COL_VAL : COL_TXT) : grey,
                            en ? (cur ? COL_VAL : COL_DIM) : grey, true);
      }
    }
    if (nrows > vis) { int trH = vis * ROW_H, trX = colX + colW + 16, trY = LIST_Y0 - 2; fillRect(trX, trY, 4, trH, (SDL_Color){40, 44, 54, 255});
      int thH = trH * vis / nrows, dn = (nrows - vis > 0 ? nrows - vis : 1); fillRect(trX, trY + (trH - thH) * top / dn, 4, thH, COL_SEL); }
    if (hasDesc) {
      int fy = SH - footerH + 6; fillRect(colX, fy - 12, colW, 2, (SDL_Color){40, 44, 54, 255});
      drawWrapped(g_font_sm, colX + 6, fy, colW - 12, fontHeight(g_font_sm) + 4, 3, p.description.c_str(), COL_DIM);
    }
    drawLocalizedFooter(row==0||s.enabled?"A  Change       X  Info       B  Back"
                                         :"X  Info       B  Back");
    drawFadeIn(); SDL_RenderPresent(g_ren); waitForNextUiFrame();
  }
}

static void gfxPackScreen(uint64_t filterTitleId) {
  bool perGame = (filterTitleId != 0);
  auto load = [&](std::vector<GfxPack> &packs) {
    packs = gfxpacks_enumerate(GRAPHICPACKS_DIR);
    if (perGame) { std::vector<GfxPack> f;
      for (auto &p : packs) { bool m = p.universal; for (auto id : p.titleIds) if (id == filterTitleId) m = true; if (m) f.push_back(p); }
      packs.swap(f); }
  };
  std::vector<GfxPack> packs; load(packs);
  std::vector<CemuGraphicPack> enabledBefore;
  const bool enabledLoaded = readEnabledPacks(enabledBefore);
  std::map<std::string, PackSel> selByRel;
  for (auto &pk : enabledBefore) { PackSel s; s.enabled = true; for (auto &pr : pk.presets) s.choices.push_back({pr.category, pr.preset}); selByRel[pk.rulesRel] = s; }
  auto commit = [&]() -> bool {
    if (!enabledLoaded) return false;
    std::vector<CemuGraphicPack> out = perGame ? enabledBefore : std::vector<CemuGraphicPack>{};
    for (auto &p : packs) { auto it = selByRel.find(p.rulesRel);
      if (perGame) out.erase(std::remove_if(out.begin(), out.end(), [&](const CemuGraphicPack &pk) { return pk.rulesRel == p.rulesRel; }), out.end());
      if (it != selByRel.end() && it->second.enabled) { CemuGraphicPack g; g.rulesRel = p.rulesRel;
        for (auto &ch : it->second.choices) g.presets.push_back({ch.first, ch.second});
        out.push_back(std::move(g));
      }
    }
    return writeEnabledPacks(out);
  };
  auto buildGames = [&](std::vector<std::string> &games) { games.clear();
    for (auto &p : packs) { std::string g = gfxGameOf(p); bool f = false; for (auto &x : games) if (x == g) { f = true; break; } if (!f) games.push_back(g); }
    std::sort(games.begin(), games.end()); };
  auto packValue = [&](const GfxPack &p, const PackSel &s) -> std::string {
    if (!s.enabled) return "off";
    if (p.presets.empty()) return "On";
    auto cats = gfxGroupPresets(p);
    if (cats.size() > 2) return "On  >";
    std::string v; for (auto &c : cats) { std::string cur; for (auto &ch : s.choices) if (ch.first == c.category) { cur = ch.second; break; }
      if (cur.empty()) cur = gfxCatDefault(p, c);
      if (!v.empty()) v += ", ";
      v += cur;
    }
    return v.empty() ? "On" : v;
  };

  int mode = perGame ? 1 : 0;
  std::vector<std::string> games; buildGames(games);
  std::vector<GRow> rows; if (perGame) rows = gfxBuildGameRows(packs, "", true);
  int sel = 0, top = 0, gameSel = 0;
  auto packVis = [&]() { int v = (SH - LIST_Y0 - 188) / ROW_H; return v < 1 ? 1 : v; };
  auto rowGap = [&](int index) { return mode==0&&top==0&&index>0?16:0; };
  const int emptyPanelH = 96;
  auto emptyPanelY = [&]() { return LIST_Y0 + ROW_H * 3 - (emptyPanelH - fontHeight(g_font_sm)) / 2; };
  auto emptyButtonRect = [&]() {
    const int w = std::min(420, SW - 120), h = 56;
    return SDL_Rect{(SW - w) / 2, emptyPanelY() + emptyPanelH + 24, w, h};
  };
  auto nextSelectable = [&](int from, int dir) { int n = (int)rows.size(); if (n == 0) return 0; int i = from;
    for (int k = 0; k < n; k++) { if (i >= 0 && i < n && !rows[i].header) return i; i = (i + dir + n) % n; } return from; };
  if (perGame) sel = nextSelectable(0, +1);
  beginScreenFx();

  for (;;) {
    if (!beginUiFrame()) return;
    int nrows = (mode == 0) ? (1 + (int)games.size()) : (int)rows.size();
    int vis = (mode == 1) ? packVis() : listVis();
    SDL_Event e;
    navRepeat();
    while (pollUiEvent(e)) {
      pumpStick(e);
      { int tx=0,ty=0; TouchKind touch=touchFeed(e,&tx,&ty);
        if(touchScrollList(touch,sel,top,nrows,vis)){
          if(mode==1&&sel>=0&&sel<nrows&&rows[sel].header) sel=nextSelectable(sel,touch==TOUCH_SCROLL_UP?+1:-1);
          if(sel<top) top=sel;
          if(sel>=top+vis) top=sel-vis+1;
          continue;
        }
        if(touch==TOUCH_TAP){
          if(ty<topBarH()||ty>=SH-40){ SDL_Event back{}; back.type=SDL_CONTROLLERBUTTONDOWN; back.cbutton.button=BTN_CANCEL; SDL_PushEvent(&back); continue; }
          if(perGame&&rows.empty()){
            const SDL_Rect button=emptyButtonRect();
            if(tx>=button.x&&tx<button.x+button.w&&ty>=button.y&&ty<button.y+button.h){
              SDL_Event press{}; press.type=SDL_CONTROLLERBUTTONDOWN; press.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&press);
            }
            continue;
          }
          for(int visibleRow=0;visibleRow<vis&&top+visibleRow<nrows;visibleRow++){
            int y=LIST_Y0+visibleRow*ROW_H+rowGap(top+visibleRow);
            if(ty>=y&&ty<y+ROW_H){ int hit=top+visibleRow; if(mode==0||!rows[hit].header){ sel=hit; SDL_Event press{}; press.type=SDL_CONTROLLERBUTTONDOWN; press.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&press); } break; }
          }
          continue;
        }
      }
      if (e.type != SDL_CONTROLLERBUTTONDOWN) continue;
      int b = e.cbutton.button;
      if (b == SDL_CONTROLLER_BUTTON_DPAD_UP || b == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
        int dir = (b == SDL_CONTROLLER_BUTTON_DPAD_DOWN) ? +1 : -1;
        if (nrows > 0) { sel = (sel + dir + nrows) % nrows;
          if (mode == 1) { int g = 0; while (sel >= 0 && sel < nrows && rows[sel].header && g++ < nrows) sel = (sel + dir + nrows) % nrows; } }
      } else if (b == BTN_CANCEL) {
        if (mode == 1 && !perGame) { mode = 0; sel = gameSel; top = 0; beginScreenFx(); }
        else { if (!commit()) { toastStatic("Could not save graphics packs"); } return; }
      } else if (b == BTN_CONFIRM) {
        if (perGame && rows.empty()) {
          downloadLatestGraphicPacks();
          load(packs);
          rows = gfxBuildGameRows(packs, "", true);
          sel = nextSelectable(0, +1); top = 0;
          beginScreenFx();
        } else if (mode == 0) {
          if (sel == 0) {
            downloadLatestGraphicPacks();
            load(packs); buildGames(games); sel = top = 0;
            beginScreenFx();
          } else if (sel - 1 < (int)games.size()) {
            gameSel = sel;
            rows = gfxBuildGameRows(packs, games[sel - 1], false); mode = 1; sel = nextSelectable(0, +1); top = 0; beginScreenFx();
          }
        } else if (sel >= 0 && sel < (int)rows.size() && !rows[sel].header) {
          const GfxPack &p = packs[rows[sel].packIndex]; PackSel &s = selByRel[p.rulesRel];
          if (p.presets.empty()) s.enabled = !s.enabled;
          else { openPackPanel(p, s); beginScreenFx(); }
        }
      } else if (b == BTN_X) {
        if(mode==0){
          if(sel==0)
            showHelpCard("Graphics Packs","Download latest packs","Graphics pack update",
                         "Downloads the latest community graphics-pack bundle. Existing enabled packs and selected presets are preserved when their names still match.",
                         nullptr,"Graphics pack action");
          else if(sel>0&&sel-1<(int)games.size())
            showHelpCard("Graphics Packs",games[sel-1].c_str(),"Game graphics packs",
                         "Opens every installed graphics pack that matches this game so fixes, enhancements, and presets can be configured.",
                         nullptr,"Graphics pack category");
        } else if(sel>=0&&sel<(int)rows.size()&&!rows[sel].header){
          const GfxPack &pack=packs[rows[sel].packIndex];
          const PackSel &selection=selByRel[pack.rulesRel];
          showHelpCard("Graphics Packs",pack.name.c_str(),"Game fix or enhancement",
                       pack.description.empty()?"Configures this graphics pack and its available presets for the selected game.":pack.description,
                       packValue(pack,selection).c_str(),"Graphics pack");
        }
        beginScreenFx();
      }
      if (sel < top) top = sel;
      if (sel >= top + vis) top = sel - vis + 1;
      if (top < 0) top = 0;
      while (mode == 1 && top > 0 && rows[top - 1].header) top--;
    }
    nrows = (mode == 0) ? (1 + (int)games.size()) : (int)rows.size();
    vis = (mode == 1) ? packVis() : listVis();
    clearUiBackground();
    std::string gname; if (mode == 1 && !perGame) { for (auto &gr : rows) if (!gr.header) { gname = gfxGameOf(packs[gr.packIndex]); break; } }
    drawHeaderStatic("Graphics Packs", gname.empty() ? nullptr : gname.c_str());
    int colX, colW, labelX, valX; listCol(&colX, &colW, &labelX, &valX);
    if (nrows > 0) {
      const int shown = std::min(vis, std::max(1, nrows - top));
      const int panelTop = LIST_Y0 - 10 + rowGap(top);
      const int panelBottom = LIST_Y0 + (shown - 1) * ROW_H + rowGap(top + shown - 1) + ROW_H + 8;
      glassPanel(colX - 12, panelTop, colW + 24, panelBottom - panelTop);
    }
    if (nrows > 0) {
      float ty = (float)(LIST_Y0 + (sel - top) * ROW_H + rowGap(sel) + 1);
      g_hy = (!g_uiAnimations || g_hy < 0) ? ty : g_hy + (ty - g_hy) * 0.30f;
      drawRowHighlight(colX, (int)g_hy, colW, ROW_H - 2);
    }
    for (int r = 0; r < vis && top + r < nrows; r++) {
      int i = top + r, slotY = LIST_Y0 + r * ROW_H + rowGap(i); bool cur = (i == sel);
      if (mode == 0) {
        if (i == 0)
          drawSettingsRowText(LauncherLocalization::Translate("Download latest packs").data(), "",
                              slotY, colW, labelX, valX, cur, cur ? COL_VAL : COL_HI, cur ? COL_VAL : COL_DIM);
        else
          drawSettingsRowText(games[i - 1].c_str(), ">", slotY, colW, labelX, valX, cur,
                              cur ? COL_VAL : COL_TXT, cur ? COL_VAL : COL_DIM);
      } else {
        const GRow &gr = rows[i];
        if (gr.header) drawSectionHeading(gr.label.c_str(), colX + 8, slotY + (ROW_H - fontHeight(g_font_sm)) / 2, colW - 16);
        else { const GfxPack &p = packs[gr.packIndex]; PackSel &s = selByRel[p.rulesRel]; bool en = s.enabled;
          SDL_Color oc = en ? (SDL_Color){120, 220, 120, 255} : COL_DIM;
          drawSettingsRowText(gr.label.c_str(), packValue(p, s).c_str(), slotY, colW, labelX + 28, valX, cur,
                              cur ? COL_VAL : COL_TXT, cur ? COL_VAL : oc, true); }
      }
    }
    if (mode == 1 && sel >= 0 && sel < (int)rows.size() && !rows[sel].header) {
      const GfxPack &p = packs[rows[sel].packIndex];
      if (!p.description.empty()) { const int fy = SH - 176, lineH = fontHeight(g_font_sm) + 4;
        const int lines = std::max(1, std::min(4, (SH - settingsFooterReserve() - fy) / std::max(1, lineH)));
        fillRect(colX, fy - 10, colW, 2, (SDL_Color){40, 44, 54, 255});
        drawWrapped(g_font_sm, colX + 6, fy, colW - 12, lineH, lines, p.description.c_str(), COL_DIM); }
    }
    if ((mode == 0 && games.empty()) || (mode == 1 && rows.empty())) {
      const int emptyH = emptyPanelH, emptyY = emptyPanelY();
      glassPanel(colX - 12, emptyY, colW + 24, emptyH);
      if (perGame)
        drawStaticTextC(g_font_sm, SW / 2, LIST_Y0 + ROW_H * 3, "No packs available for this game", COL_DIM);
      else
        drawStaticTextC(g_font_sm, SW / 2, LIST_Y0 + ROW_H * 3, "No graphics packs installed", COL_DIM);
      if (perGame) {
        const SDL_Rect button = emptyButtonRect();
        glassPanel(button.x, button.y, button.w, button.h);
        drawRowHighlight(button.x + 6, button.y + 4, button.w - 12, button.h - 8);
        drawStaticTextC(g_font, SW / 2, button.y + (button.h - fontHeight(g_font)) / 2,
                        "Download latest packs", COL_VAL);
      }
    }
    if (nrows > vis) { int trH = vis * ROW_H, trX = colX + colW + 16, trY = LIST_Y0 - 2; fillRect(trX, trY, 4, trH, (SDL_Color){40, 44, 54, 255});
      int thH = trH * vis / nrows, dn = (nrows - vis > 0 ? nrows - vis : 1); fillRect(trX, trY + (trH - thH) * top / dn, 4, thH, COL_SEL); }
    const bool packRow=sel>=0&&sel<(int)rows.size()&&!rows[sel].header;
    drawLocalizedFooter(mode==0?"A  Open       X  Info       B  Back":
                        packRow?"A  Change       X  Info       B  Back":
                        (perGame&&rows.empty())?"A  Download latest packs       B  Back":"B  Back");
    drawFadeIn(); SDL_RenderPresent(g_ren); waitForNextUiFrame();
  }
}

static SDL_Texture *loadScaledTexture(const std::string &path,int width,int height,bool preserveAspect) {
  if(width<1||height<1) return nullptr;
  const float scale=g_uiScale>0.0f?g_uiScale:1.0f;
  int pixelWidth=std::max(1,(int)std::lround(width*scale));
  int pixelHeight=std::max(1,(int)std::lround(height*scale));
  SDL_Surface *source=IMG_Load(path.c_str());
  if(!source) return nullptr;
  if(preserveAspect){
    const SDL_Rect fitted=fitArtworkRect(source->w,source->h,{0,0,pixelWidth,pixelHeight});
    pixelWidth=fitted.w;pixelHeight=fitted.h;
  }
  SDL_Surface *scaled=SDL_CreateRGBSurfaceWithFormat(0,pixelWidth,pixelHeight,32,SDL_PIXELFORMAT_RGBA32);
  if(!scaled){ SDL_FreeSurface(source); return nullptr; }
  SDL_BlendMode blend=SDL_BLENDMODE_NONE;
  SDL_GetSurfaceBlendMode(source,&blend);
  SDL_SetSurfaceBlendMode(source,SDL_BLENDMODE_NONE);
  bool ok=SDL_BlitScaled(source,nullptr,scaled,nullptr)==0;
  SDL_SetSurfaceBlendMode(source,blend);
  SDL_FreeSurface(source);
  if(!ok){ SDL_FreeSurface(scaled); return nullptr; }
  SDL_Texture *texture=SDL_CreateTextureFromSurface(g_ren,scaled);
  SDL_FreeSurface(scaled);
  if(texture) SDL_SetTextureBlendMode(texture,SDL_BLENDMODE_BLEND);
  return texture;
}

static bool pickIcon(Game &g, char *outPath, size_t outSize) {
  std::string base = std::string(DATA_DIR) + "/forwarders", tmp = base + "/iconpick";
  mkdir(base.c_str(),0777); mkdir(tmp.c_str(),0777);
  if(DIR*d=opendir(tmp.c_str())){ struct dirent*e; while((e=readdir(d))) if(e->d_name[0]!='.') remove((tmp+"/"+std::string(e->d_name)).c_str()); closedir(d); }
  std::vector<std::string> paths; struct stat st;
  if(!g.iconPath.empty() && stat(g.iconPath.c_str(),&st)==0) paths.push_back(g.iconPath);
  { std::string cp=existingCoverPath(g); if(stat(cp.c_str(),&st)==0) paths.push_back(cp); }
  std::string key=steamGridDbKey();
  if(!key.empty()){
    clearUiBackground();
    drawHeaderStatic("Choose an icon", g.title.c_str());
    { const int waitW=std::min(940,SW-64),waitH=140;
      glassPanel((SW-waitW)/2,(SH-waitH)/2,waitW,waitH); }
    drawStaticTextC(g_font, SW/2, SH/2, "Fetching icons from SteamGridDB...", COL_TXT);
    SDL_RenderPresent(g_ren);
    int nf=0;
    if(!runCancellableNetworkTask("Choose an icon","Fetching icons from SteamGridDB...",
       [&](const std::atomic_bool &cancel){ nf=griddb_fetch_icons(key,g.title,tmp,14,&cancel); })) return false;
    for(int i=0;i<nf;i++){ char p[300]; snprintf(p,sizeof(p),"%s/gicon_%d.png",tmp.c_str(),i); paths.push_back(p); }
  }
  if(paths.empty()){ toastStatic("No icon found - add a SteamGridDB key or download a cover first"); return false; }
  const int n=(int)paths.size();
  const int cols=std::max(1,std::min(n,5)),rows=(n+cols-1)/cols;
  const int gap=24,top=topBarH()+32,bottom=SH-settingsFooterReserve()-12;
  const int cell=std::min({200,(SW-96-(cols-1)*gap)/cols,(bottom-top-(rows-1)*gap)/rows});
  const int x0=(SW-(cols*cell+(cols-1)*gap))/2;
  const int y0=top+std::max(0,(bottom-top-rows*cell-(rows-1)*gap)/2);
  std::vector<SDL_Texture*> tex(n,nullptr);
  for(int i=0;i<n;i++) tex[i]=loadScaledTexture(paths[i],cell,cell);
  int sel=0, chosen=-1; bool done=false; beginScreenFx();
  while(!done){
    if (!beginUiFrame()) { done=true; break; }
    SDL_Event e; navRepeat();
    while(pollUiEvent(e)){ pumpStick(e);
      { int tx=0,ty=0; TouchKind touch=touchFeed(e,&tx,&ty);
        if(touch==TOUCH_SCROLL_UP){ sel=std::min(n-1,sel+cols); continue; }
        if(touch==TOUCH_SCROLL_DOWN){ sel=std::max(0,sel-cols); continue; }
        if(touch==TOUCH_TAP){
          for(int i=0;i<n;i++){ int row=i/cols,column=i%cols,x=x0+column*(cell+gap),y=y0+row*(cell+gap);
            if(tx>=x&&tx<x+cell&&ty>=y&&ty<y+cell){ sel=i; chosen=i; done=true; break; } }
          if(done) continue;
        }
      }
      if(e.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      switch(e.cbutton.button){
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: sel=(sel+1)%n; break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  sel=(sel+n-1)%n; break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  sel=(sel+cols)%n; break;
        case SDL_CONTROLLER_BUTTON_DPAD_UP:    sel=(sel-cols+n)%n; break;
        case BTN_CONFIRM: chosen=sel; done=true; break;
        case BTN_CANCEL:  done=true; break;
      }
    }
    clearUiBackground();
    drawHeaderStatic("Choose an icon", g.title.c_str());
    for(int i=0;i<n;i++){ int r=i/cols,c=i%cols, x=x0+c*(cell+gap), y=y0+r*(cell+gap);
      if(i==sel) roundedPanel(x-8,y-8,cell+16,cell+16,COL_PANEL,COL_SEL);
      else glassPanel(x-8,y-8,cell+16,cell+16);
      fillRect(x,y,cell,cell,COL_CARD);
      if(tex[i]){ SDL_Rect d{x,y,cell,cell}; SDL_RenderCopy(g_ren,tex[i],nullptr,&d); }
      else drawStaticTextC(g_font_sm,x+cell/2,y+cell/2,"?",COL_DIM);
    }
    FootItem footer[]={{"A",LauncherLocalization::Translate("Use icon").data()},
                       {"B",LauncherLocalization::Translate("Back").data()}};
    drawFooterHints(footer,2,SH-26);
    drawFadeIn(); SDL_RenderPresent(g_ren); waitForNextUiFrame();
  }
  for(auto t:tex) if(t) SDL_DestroyTexture(t);
  if(chosen>=0 && chosen<n){ snprintf(outPath,outSize,"%s",paths[chosen].c_str()); return true; }
  return false;
}

static void forwarderWizard(Game &g) {
  const GameDetailLayout layout=gameDetailLayout();
  const int isz=260;
  const int ix=layout.preview.x+(layout.preview.w-isz)/2;
  const int iy=layout.preview.y+(layout.preview.h-isz)/2;
  const SDL_Rect panel={layout.content.x,layout.content.y+std::max(0,(layout.content.h-330)/2),
                        layout.content.w,330};
  const int rx=panel.x+24, rw=panel.w-48;
  const int nameY=panel.y+32, authY=panel.y+130, createY=panel.y+232;
  const int fieldH=82, createH=64;
  char name[256]; snprintf(name,sizeof(name),"%s",g.title.c_str());
  char author[128]; snprintf(author,sizeof(author),"%s","naga");
  char icon[300]={0};
  { struct stat st; std::string cp=existingCoverPath(g);
    if(stat(cp.c_str(),&st)==0) snprintf(icon,sizeof(icon),"%s",cp.c_str());
    else if(!g.iconPath.empty() && stat(g.iconPath.c_str(),&st)==0) snprintf(icon,sizeof(icon),"%s",g.iconPath.c_str()); }
  SDL_Texture *iconTex = icon[0] ? loadScaledTexture(icon,isz,isz) : nullptr;
  int sel=0; bool done=false; beginScreenFx();

  auto edit=[&](const char *hdr, char *buf, size_t sz){
    char b[256];
    if(promptText(hdr, buf, b, sizeof(b)) && b[0] && sz){
      size_t len=std::min(strlen(b),sz-1);
      memcpy(buf,b,len);
      buf[len]=0;
    }
  };
  auto build=[&](){
    if(!icon[0]){ toastStatic("Pick an icon first"); return; }
    clearUiBackground();
    drawHeaderStatic("Creating HOME shortcut", g.title.c_str());
    { const int waitW=std::min(940,SW-64),waitH=140;
      glassPanel((SW-waitW)/2,(SH-waitH)/2,waitW,waitH); }
    drawStaticTextC(g_font, SW/2, SH/2, "Building + installing forwarder...", COL_TXT);
    SDL_RenderPresent(g_ren);
    appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
    char err[256]={0}; bool ok=forwarder_create(g.key,g.legacyKey,name,author,icon,err,sizeof(err));
    appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
    if(ok){ toastStatic("HOME shortcut installed"); done=true; }
    else modalMessageStatic("Shortcut failed", { std::string(err[0]?err:"Unknown error") });
    beginScreenFx();
  };
  auto activate=[&](){
    if(sel==0){ char p[300]; if(pickIcon(g,p,sizeof(p))){ snprintf(icon,sizeof(icon),"%s",p); if(iconTex)SDL_DestroyTexture(iconTex); iconTex=loadScaledTexture(icon,isz,isz); } beginScreenFx(); }
    else if(sel==1) edit("Shortcut name", name, sizeof(name));
    else if(sel==2) edit("Author", author, sizeof(author));
    else build();
  };

  while(!done){
    if (!beginUiFrame()) { done=true; break; }
    SDL_Event e; navRepeat();
    while(pollUiEvent(e)){
      pumpStick(e);
      { int tx=0,ty=0; TouchKind tk=touchFeed(e,&tx,&ty);
        if(tk==TOUCH_TAP){
          const bool inColumn=tx>=rx-10&&tx<rx+rw+10;
          if(tx>=ix&&tx<ix+isz&&ty>=iy&&ty<iy+isz){ sel=0; activate(); }
          else if(inColumn&&ty>=nameY-6&&ty<nameY+fieldH){ sel=1; activate(); }
          else if(inColumn&&ty>=authY-6&&ty<authY+fieldH){ sel=2; activate(); }
          else if(inColumn&&ty>=createY-6&&ty<createY+createH){ sel=3; activate(); }
          else if(ty>=SH-40) done=true;
          continue;
        }
      }
      if(e.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      switch(e.cbutton.button){
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  sel=0; break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: if(sel==0) sel=1; break;
        case SDL_CONTROLLER_BUTTON_DPAD_UP:    sel=(sel==0)?3:(sel==1?3:sel-1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  sel=(sel==0)?1:(sel==3?1:sel+1); break;
        case BTN_CONFIRM: activate(); break;
        case BTN_CANCEL:  done=true; break;
      }
    }
    clearUiBackground();
    drawHeaderStatic("Create HOME shortcut", g.title.c_str());
    { SDL_Rect iconRect={ix,iy,isz,isz}; drawArtworkPreview(iconTex,iconRect,sel==0); }
    drawTextC(g_font_sm, ix+isz/2, iy+isz+20,
              LauncherLocalization::Translate("Icon").data(), sel==0?COL_VAL:COL_DIM);
    glassPanel(panel.x,panel.y,panel.w,panel.h);
    auto field=[&](int idx,int y,const char*label,const char*val){ bool cur=sel==idx;
      roundedRect(rx-10,y-6,rw+20,fieldH,4,COL_CARD);
      if(cur){ drawRowHighlight(rx-10,y-6,rw+20,fieldH); }
      drawText(g_font_sm, rx, y+2, LauncherLocalization::Translate(label).data(), COL_HI);
      drawScrollTextL(g_font, rx, y+32, rw-8, val, cur?COL_VAL:COL_TXT); };
    field(1,nameY,"Name",name);
    field(2,authY,"Author",author);
    { bool cur=sel==3;
      drawButtonPanel(rx-10,createY-6,rw+20,createH,cur);
      drawTextC(g_font, rx+rw/2, createY-6+(createH-fontHeight(g_font))/2,
                LauncherLocalization::Translate("Create shortcut").data(),
                cur?COL_VAL:COL_TXT); }
    FootItem footer[]={{"A",LauncherLocalization::Translate(sel==3?"Create shortcut":"Edit").data()},
                       {"B",LauncherLocalization::Translate("Back").data()}};
    drawFooterHints(footer,2,SH-26);
    drawFadeIn(); SDL_RenderPresent(g_ren); waitForNextUiFrame();
  }
  if(iconTex) SDL_DestroyTexture(iconTex);
}

static void showTitleIdUnavailable(const char *title,const Game &game) {
  std::vector<std::string> lines;
  if(!game.titleIdError.empty()) lines.push_back(game.titleIdError);
  else lines.push_back("The title ID could not be read from this game's metadata.");
  std::string extension=game.file.substr(game.file.find_last_of('.')==std::string::npos?game.file.size():game.file.find_last_of('.'));
  std::transform(extension.begin(),extension.end(),extension.begin(),[](unsigned char c){ return (char)std::tolower(c); });
  if(extension==".wud"||extension==".wux"||extension==".iso")
    lines.push_back("Add the disc key to Cemu/keys.txt or place a .key beside the game.");
  lines.push_back("");
  lines.push_back("Rescan the game folders after correcting the file or key.");
  modalMessage(title,lines);
}

static void clearShaderCaches(const Game &g) {
  if (!g.titleId) {
    showTitleIdUnavailable("Shader caches unavailable",g);
    return;
  }

  static const char *formats[] = {
    "/cache/shaderCache/driver/vk/%016llx.bin",
    "/cache/shaderCache/precompiled/%016llx_spirv.bin",
    "/cache/shaderCache/precompiled/%016llx_gl.bin",
    "/cache/shaderCache/precompiled/%016llx_air.bin",
    "/cache/shaderCache/transferable/%016llx_shaders.bin",
    "/cache/shaderCache/transferable/%016llx_mtlshaders.bin",
    "/cache/shaderCache/transferable/%016llx_vkpipeline.bin",
    "/cache/shaderCache/transferable/%016llx_mtlpipeline.bin",
  };
  std::vector<std::string> paths;
  for (const char *format : formats) {
    char suffix[256];
    int length = snprintf(suffix, sizeof(suffix), format, (unsigned long long)g.titleId);
    if (length > 0 && (size_t)length < sizeof(suffix)) {
      std::string path = std::string(DATA_DIR) + suffix;
      if (regularFileExists(path)) paths.emplace_back(std::move(path));
    }
  }
  if (paths.empty()) {
    toastStatic("No shader caches found");
    return;
  }
  if (!confirmBoxStatic("Clear shader caches?", {
        g.title, "", "Cemu will rebuild them the next time the game runs."
      }))
    return;

  int failed = 0;
  for (const auto &path : paths)
    if (remove(path.c_str()) != 0 && errno != ENOENT)
      failed++;
  if (failed) {
    modalMessageStatic("Cache cleanup failed", {
      std::to_string(failed) + " cache file(s) could not be removed."
    });
  } else {
    toastStatic("Shader caches cleared");
  }
}



static bool gameFolderDeletable(const std::string &path,std::string &reason){
  if(filesystemRoot(path)){ reason="That path is a drive root."; return false; }
  if(!directoryIsTitle(path)){
    reason="No code folder or title.tmd here, so this is not a game folder.";
    return false;
  }
  const std::string normalized=normalizeLocationPath(path);
  for(const std::string &source:loadGameSources())
    if(normalizeLocationPath(source)==normalized){
      reason="This is a configured game folder. Remove it from Game folders first.";
      return false;
    }
  return true;
}

static void measureTree(const std::string &path,size_t &files,uint64_t &bytes){
  DIR *dir=opendir(path.c_str());
  if(!dir) return;
  struct dirent *entry;
  while((entry=readdir(dir))){
    if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,"..")) continue;
    const std::string child=join(path,entry->d_name);
    struct stat info{};
    if(lstat(child.c_str(),&info)!=0) continue;
    if(S_ISDIR(info.st_mode)) measureTree(child,files,bytes);
    else { files++; bytes+=(uint64_t)info.st_size; }
  }
  closedir(dir);
}

static const char *GAME_MENU_ITEMS[]={"Launch","Game settings","Graphics packs","Rename game",
  "Cover settings","Create HOME shortcut","Favorite","Manage installed content","Clear shader caches",
  "Clear game settings","Delete game"};
static const int GAME_MENU_COUNT=11;
static const int GAME_MENU_MANAGE_START=7;
static const int GAME_MENU_SECTION_GAP=56;
struct GameMenuLayout {
  GameDetailLayout detail;
  int start,rowHeight;
  int rowY(int index)const{
    return start+index*rowHeight+(index>=GAME_MENU_MANAGE_START?GAME_MENU_SECTION_GAP:0);
  }
};
static GameMenuLayout gameMenuLayout(){
  const GameDetailLayout detail=gameDetailLayout();
  const int available=detail.content.h-44-GAME_MENU_SECTION_GAP;
  int rowHeight=settingsRowH();
  if(available/GAME_MENU_COUNT<rowHeight)
    rowHeight=std::max(32,available/GAME_MENU_COUNT);
  const int height=GAME_MENU_COUNT*rowHeight+GAME_MENU_SECTION_GAP+44+8;
  return {detail,detail.content.y+44+std::max(0,(detail.content.h-height)/2),rowHeight};
}
static void drawGameMenu(Game &game,int selection,bool graphicPacksDownloaded){
  const GameMenuLayout layout=gameMenuLayout();
  const SDL_Rect &menu=layout.detail.content;
  clearUiBackground();
  char titleIdText[48];
  if(game.titleId) snprintf(titleIdText,sizeof(titleIdText),"Title ID  %016llX",(unsigned long long)game.titleId);
  else snprintf(titleIdText,sizeof(titleIdText),"%s",
                LauncherLocalization::Translate("Title ID unavailable").data());
  drawPageHeader(game.title.c_str(),LauncherLocalization::Translate("Game menu").data(),titleIdText);
  drawGamePreview(game,layout.detail.preview);
  drawSectionHeading("General",menu.x,layout.start-44,menu.w);
  glassPanel(menu.x-8,layout.start-8,menu.w+16,GAME_MENU_MANAGE_START*layout.rowHeight+16);
  drawSectionHeading("Manage game",menu.x,layout.rowY(GAME_MENU_MANAGE_START)-44,menu.w);
  glassPanel(menu.x-8,layout.rowY(GAME_MENU_MANAGE_START)-8,menu.w+16,
             (GAME_MENU_COUNT-GAME_MENU_MANAGE_START)*layout.rowHeight+16);
  const float target=(float)(layout.rowY(selection)+2);
  g_hy=(!g_uiAnimations||g_hy<0)?target:g_hy+(target-g_hy)*0.30f;
  drawRowHighlight(menu.x,(int)g_hy,menu.w,layout.rowHeight-4);
  for(int index=0;index<GAME_MENU_COUNT;index++){
    const bool current=index==selection;
    const bool submenu=index==1||index==2||index==4||index==5||index==7;
    const char *label=(index==2&&!graphicPacksDownloaded)?"Download graphics packs":GAME_MENU_ITEMS[index];
    SDL_Color color=index==GAME_MENU_COUNT-1?(SDL_Color){238,135,135,255}:
                    (index==2&&!graphicPacksDownloaded?COL_HI:COL_TXT);
    if(current) color=COL_VAL;
    drawSettingsRowText(LauncherLocalization::Translate(label).data(),index==6?tr(g_favorites.count(game.key)?"Yes":"No"):(submenu?">":""),
                        layout.rowY(index),menu.w,menu.x+24,menu.x+menu.w-24,current,color,
                        current?COL_VAL:COL_DIM,false,layout.rowHeight);
  }
  FootItem footer[]={{"A",LauncherLocalization::Translate(selection==0?"Launch":"Select").data()},
                     {"B",LauncherLocalization::Translate("Back").data()}};
  drawFooterHints(footer,2,SH-26);
}

static int perGameMenu(Game &g) {
  const int n=GAME_MENU_COUNT;
  int sel=0, touchTop=0;
  std::string gp = std::string(GAMECFG_DIR) + "/" + g.key + ".ini";
  const std::string downloadedPacksVersion=std::string(GRAPHICPACKS_DIR)+"/downloadedGraphicPacks/version.txt";
  bool graphicPacksDownloaded=regularFileExists(downloadedPacksVersion);
  storeLoad(g_game, gp.c_str());
  beginScreenFx();
  for(;;){
    if (!beginUiFrame()) return 0;
    const GameMenuLayout layout=gameMenuLayout();
    SDL_Event e;
    navRepeat();
    while(pollUiEvent(e)){
      pumpStick(e);
      { int tx=0,ty=0; TouchKind tk=touchFeed(e,&tx,&ty);
        if(touchScrollList(tk,sel,touchTop,n,n)) continue;
        if(tk==TOUCH_TAP){
          if(ty<topBarH()||ty>=SH-40){ return 0; }
          const SDL_Rect &menu=layout.detail.content;
          if(tx<menu.x||tx>=menu.x+menu.w) continue;
          for(int i=0;i<n;i++){ const int rowTop=layout.rowY(i);
            if(ty>=rowTop && ty<rowTop+layout.rowHeight){ sel=i;
            SDL_Event a; memset(&a,0,sizeof(a)); a.type=SDL_CONTROLLERBUTTONDOWN; a.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&a); break; } }
          continue;
        }
      }
      if(e.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      switch(e.cbutton.button){
        case SDL_CONTROLLER_BUTTON_DPAD_UP: sel=(sel+n-1)%n; break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: sel=(sel+1)%n; break;
        case BTN_CANCEL: return 0;
        case BTN_CONFIRM:
          if(sel==0) return 1;
          else if(sel==1){
            g_active=&g_game;
            runSettingsRoot(g.title.c_str());
            g_active=&g_global;
            mkdir(GAMECFG_DIR,0777);
            storeSave(g_game, gp.c_str());
            g.hasCfg = !g_game.kv.empty();
            beginScreenFx();
          }
          else if(sel==2){
            if(!graphicPacksDownloaded){
              const int result=downloadLatestGraphicPacks();
              if(result==GFX_OK||result==GFX_UPTODATE)
                graphicPacksDownloaded=regularFileExists(downloadedPacksVersion);
            }
            else if (g.titleId) gfxPackScreen(g.titleId);
            else showTitleIdUnavailable("Graphics packs unavailable",g);
            beginScreenFx();
          }
          else if(sel==3){
            char buf[128];
            if(promptTextStatic("Rename game", g.title.c_str(), buf, sizeof(buf))){
              g.title = buf;
              storeSet(g_titles, g.key.c_str(), buf);
              storeSave(g_titles, TITLES_INI);
            }
          }
          else if(sel==4){ coverSettings(g); beginScreenFx(); }
          else if(sel==5){ forwarderWizard(g); beginScreenFx(); }
          else if(sel==7){
            if (!g.titleId) showTitleIdUnavailable("Installed content unavailable",g);
            else if (installedContentScreen(g.titleId) && g.path.empty()) return 2;
            beginScreenFx();
          }
          else if(sel==8){ clearShaderCaches(g); beginScreenFx(); }
          else if(sel==9){ g_game.kv.clear(); remove(gp.c_str()); g.hasCfg=false; toastStatic("Game settings cleared"); beginScreenFx(); }
          else if(sel==6){
            if(!g_favorites.erase(g.key)) g_favorites.insert(g.key);
            saveLibraryOrganization(); beginScreenFx();
          }
          else if(sel==10){
            if(g.path.empty()){
              if (installedContentScreen(g.titleId)) return 2;
              beginScreenFx();
            }
            else {
              struct stat gameStat{};
              std::string reason;
              if (stat(g.path.c_str(), &gameStat) != 0) {
                modalMessageStatic("Delete failed", { "The selected game no longer exists.", "No metadata was removed." });
                beginScreenFx();
              } else if (!S_ISDIR(gameStat.st_mode) && !S_ISREG(gameStat.st_mode)) {
                modalMessageStatic("Delete failed", { "The selected path is not a game file or folder." });
                beginScreenFx();
              } else if (S_ISDIR(gameStat.st_mode) && !gameFolderDeletable(g.path, reason)) {
                modalMessageStatic("Delete failed", { reason, g.path });
                beginScreenFx();
              } else {
                const bool folder = S_ISDIR(gameStat.st_mode);
                size_t files = 1;
                uint64_t bytes = (uint64_t)gameStat.st_size;
                if (folder) { files = 0; bytes = 0; measureTree(g.path, files, bytes); }
                char counted[96];
                snprintf(counted, sizeof(counted), "%llu file(s), %s",
                         (unsigned long long)files, installedSizeText(bytes).c_str());
                if (confirmBoxStatic("Delete game?", { g.title, g.path, std::string(counted), "",
                                                       "This permanently deletes it from its location.",
                                                       "This cannot be undone." })) {
                  const bool removed = folder ? removeTreeInternal(g.path) : remove(g.path.c_str()) == 0;
                  if (!removed) {
                    modalMessageStatic("Delete failed", { "The game could not be removed.", "No metadata was removed." });
                    beginScreenFx();
                  } else {
                    fsdevCommitDevice("sdmc");
                    remove(coverPath(g).c_str());
                    remove(backgroundPath(g).c_str());
                    g_favorites.erase(g.key);
                    storeRemovePrefix(g_global,("Artwork/"+g.key+"/").c_str());
                    saveLibraryOrganization();
                    remove(gp.c_str());
                    storeRemove(g_titles, g.key.c_str());
                    storeRemove(g_recent, g.key.c_str());
                    storeSave(g_titles, TITLES_INI);
                    storeSave(g_recent, RECENT_INI);
                    toastStatic("Game deleted");
                    return 2;
                  }
                }
              }
            }
          }
          break;
      }
    }
    drawGameMenu(g,sel,graphicPacksDownloaded);
    drawFadeIn();
    SDL_RenderPresent(g_ren);
    waitForNextUiFrame();
  }
}

static void drawSetupProgress(int pct, const char *msg) {
  clearUiBackground();
  const int bw = SW * 2 / 3, bx = (SW - bw) / 2, by = SH / 2 + 40, bh = 26;
  glassPanel(bx - 40, SH / 2 - 198, bw + 80, 340);
  if (g_logo) { int s = 140; SDL_Rect ld = {(SW - s) / 2, SH / 2 - 170, s, s}; SDL_RenderCopy(g_ren, g_logo, nullptr, &ld); }
  drawTextC(g_font, SW / 2, SH / 2 - 10, LauncherLocalization::Translate(msg).data(), COL_TXT);
  drawProgressBar(bx, by, bw, bh, pct / 100.0);
  char t[16]; snprintf(t, sizeof(t), "%d%%", pct);
  drawTextC(g_font_sm, SW / 2, by + bh + 14, t, COL_DIM);
  SDL_RenderPresent(g_ren);
}

static bool validNro(const std::string &path, long long expectedSize) {
  struct stat fileStat{};
  if (stat(path.c_str(), &fileStat) != 0 || fileStat.st_size <= 0 ||
      (expectedSize >= 0 && fileStat.st_size != expectedSize)) return false;
  FILE *file = fopen(path.c_str(), "rb");
  if (!file) return false;
  NroStart start{};
  NroHeader header{};
  bool ok = fread(&start, 1, sizeof(start), file) == sizeof(start) &&
            fread(&header, 1, sizeof(header), file) == sizeof(header);
  fclose(file);
  if (!ok || header.magic != NROHEADER_MAGIC ||
      header.size < sizeof(start) + sizeof(header) || header.size > (u64)fileStat.st_size)
    return false;
  for (const auto &segment : header.segments)
    if (segment.file_off > header.size || segment.size > header.size - segment.file_off)
      return false;
  return true;
}

static bool readEmbeddedEmulatorHash(std::array<u8, SHA256_HASH_SIZE> &hash,
                                     std::string &text) {
  FILE *file=fopen(EMU_HASH_SRC,"rb");
  if(!file) return false;
  char buffer[128];
  const size_t size=fread(buffer,1,sizeof(buffer),file);
  const bool ok=!ferror(file) && feof(file);
  fclose(file);
  if(!ok) return false;
  std::string value=trim(std::string(buffer,size));
  if(value.size()!=SHA256_HASH_SIZE*2) return false;
  auto hex=[](char c)->int {
    if(c>='0'&&c<='9') return c-'0';
    if(c>='a'&&c<='f') return c-'a'+10;
    if(c>='A'&&c<='F') return c-'A'+10;
    return -1;
  };
  static const char digits[]="0123456789abcdef";
  text.clear(); text.reserve(SHA256_HASH_SIZE*2);
  for(size_t i=0;i<SHA256_HASH_SIZE;i++){
    int high=hex(value[i*2]),low=hex(value[i*2+1]);
    if(high<0||low<0) return false;
    hash[i]=(u8)((high<<4)|low);
    text+=digits[high]; text+=digits[low];
  }
  return true;
}

static void cleanupLegacyEmuHosts() {
  static const char *directories[] = {DATA_DIR, EMU_HOST_DIR};
  static const char *filenames[] = {"cemu_vk.nro", "cemu_gl.nro", "cemu_zink.nro"};
  static const char *suffixes[] = {"", ".tmp", ".old"};
  bool changed=false;
  for(const char *directory:directories)
    for(const char *filename:filenames)
      for(const char *suffix:suffixes){
        const std::string path=std::string(directory)+"/"+filename+suffix;
        if(remove(path.c_str())==0) changed=true;
      }
  const std::string oldMarker=std::string(DATA_DIR)+"/.emu_build";
  for(const char *suffix:suffixes){
    const std::string path=oldMarker+suffix;
    if(remove(path.c_str())==0) changed=true;
  }
  if(changed) fsdevCommitDevice("sdmc");
}

static bool ensureEmu() {
  std::string marker = std::string(DATA_DIR) + "/.emu_core_build";
  const std::string tmp = std::string(EMU_NRO_DST) + ".tmp";
  const std::string old = std::string(EMU_NRO_DST) + ".old";
  bool destinationExists=false,oldExists=false,tmpExists=false;
  if(!queryRegularFile(EMU_NRO_DST,destinationExists) || !queryRegularFile(old,oldExists) ||
     !queryRegularFile(tmp,tmpExists)) return false;
  if (!destinationExists && oldExists) {
    if (validNro(old,-1)) {
      if(rename(old.c_str(),EMU_NRO_DST)!=0) return false;
      destinationExists=true;
    } else if(remove(old.c_str())!=0) {
      return false;
    }
    oldExists=false;
    fsdevCommitDevice("sdmc");
  } else if (destinationExists && oldExists) {
    if (!validNro(EMU_NRO_DST, -1) && validNro(old, -1)) {
      if (remove(EMU_NRO_DST) != 0 || rename(old.c_str(), EMU_NRO_DST) != 0) return false;
    } else {
      if(remove(old.c_str())!=0) return false;
    }
    fsdevCommitDevice("sdmc");
  }
  if(tmpExists && remove(tmp.c_str())!=0) return false;

  std::array<u8,SHA256_HASH_SIZE> expectedHash{};
  std::string expectedHashText;
  if(!readEmbeddedEmulatorHash(expectedHash,expectedHashText) || !recoverAtomicFile(marker)) return false;

  struct stat sourceStat{};
  if (stat(EMU_NRO_SRC, &sourceStat) != 0 || sourceStat.st_size <= 0 || !S_ISREG(sourceStat.st_mode)) return false;
  char cur[80] = {0};
  if (FILE *mf = fopen(marker.c_str(), "r")) { if (!fgets(cur, sizeof(cur), mf)) cur[0] = 0; fclose(mf); }
  struct stat destinationStat{};
  if (trim(cur) == expectedHashText && stat(EMU_NRO_DST, &destinationStat) == 0 &&
      destinationStat.st_size == sourceStat.st_size && validNro(EMU_NRO_DST, sourceStat.st_size))
    return true;

  FILE *in = fopen(EMU_NRO_SRC, "rb"), *out = fopen(tmp.c_str(), "wb");
  if (!in || !out) {
    if (in) fclose(in);
    if (out) fclose(out);
    remove(tmp.c_str());
    return false;
  }
  static char buf[1 << 20]; size_t n; bool ok = true; long long written = 0; int lastPct = -1;
  Sha256Context hashContext;
  sha256ContextCreate(&hashContext);
  appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
  while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
    if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
    sha256ContextUpdate(&hashContext,buf,n);
    written += (long long)n;
    int pct = (int)((unsigned __int128)written * 100 / sourceStat.st_size);
    if (pct != lastPct) {
      if(g_sdlReady){
        if(!beginUiFrame()){ ok=false; break; }
        SDL_Event event; while(pollUiEvent(event)) {}
        if(g_exitRequested){ ok=false; break; }
        drawSetupProgress(pct, "Preparing emulator...");
      }
      lastPct = pct;
    }
  }
  if (ferror(in)) ok = false;
  if (fclose(in) != 0) ok = false;
  if (fflush(out) != 0 || fsync(fileno(out)) != 0) ok = false;
  if (fclose(out) != 0) ok = false;
  appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
  std::array<u8,SHA256_HASH_SIZE> actualHash{};
  sha256ContextGetHash(&hashContext,actualHash.data());
  if (!ok || stat(tmp.c_str(), &destinationStat) != 0 ||
      destinationStat.st_size != sourceStat.st_size || actualHash != expectedHash ||
      !validNro(tmp, sourceStat.st_size)) {
    remove(tmp.c_str());
    return false;
  }
  if (!replaceAtomic(EMU_NRO_DST, tmp)) {
    remove(tmp.c_str());
    return false;
  }
  return writeAtomicText(marker, expectedHashText + "\n");
}

static std::string libraryTabLabel(int tab){
  static const char *labels[]={"Recent","All games","Favorites"};
  return tab>=0&&tab<libraryTabCount()?uiText(labels[tab]):std::string{};
}

enum class LibraryIcon { Recent, Games, Favorite, Settings };

static void drawLibraryIcon(LibraryIcon icon,int cx,int cy,int size,SDL_Color color){
  if(icon==LibraryIcon::Settings){
    const int thickness=std::max(2,size/12),spacing=std::max(4,size/3);
    const int knobOffsets[]={size/5,-size/4,size/10};
    for(int row=0;row<3;row++){
      const int y=cy+(row-1)*spacing;
      roundedRect(cx-size/2,y-thickness/2,size,thickness,thickness/2,color);
      roundedRect(cx+knobOffsets[row]-thickness,y-spacing/3,thickness*2,
                  std::max(thickness*2,spacing*2/3),thickness,color);
    }
  } else if(icon==LibraryIcon::Games){
    const int cell=std::max(3,size/4),gap=std::max(2,size/7);
    for(int row=0;row<2;row++) for(int column=0;column<3;column++)
      roundedRect(cx-(3*cell+2*gap)/2+column*(cell+gap),cy-(2*cell+gap)/2+row*(cell+gap),cell,cell,1,color);
  } else {
    const int points=icon==LibraryIcon::Favorite?10:48;
    const auto point=[&](int index,float radius){
      const float angle=index*6.2831853f/points-1.5707963f;
      if(icon==LibraryIcon::Favorite&&index%2) radius*=0.45f;
      return SDL_FPoint{cx+std::cos(angle)*radius,cy+std::sin(angle)*radius};
    };
    std::array<SDL_Vertex,288> vertices;
    int count=0;
    for(int i=0;i<points;i++){
      const SDL_FPoint outer=point(i,size*0.5f),next=point(i+1,size*0.5f);
      if(icon==LibraryIcon::Favorite){
        vertices[count++]={{(float)cx,(float)cy},color,{0,0}};
        vertices[count++]={outer,color,{0,0}}; vertices[count++]={next,color,{0,0}};
      } else {
        const SDL_FPoint inner=point(i,size*0.5f-2),innerNext=point(i+1,size*0.5f-2);
        vertices[count++]={outer,color,{0,0}}; vertices[count++]={next,color,{0,0}};
        vertices[count++]={inner,color,{0,0}}; vertices[count++]={inner,color,{0,0}};
        vertices[count++]={next,color,{0,0}}; vertices[count++]={innerNext,color,{0,0}};
      }
    }
    SDL_RenderGeometry(g_ren,nullptr,vertices.data(),count,nullptr,0);
    if(icon==LibraryIcon::Recent){
      fillRect(cx-1,cy-size/3,2,size/3+1,color);
      fillRect(cx,cy-1,size/4,2,color);
    }
  }
}

struct LibraryHeaderLayout {
  std::array<SDL_Rect,3> tabs;
  SDL_Rect settings;
};

static LibraryHeaderLayout libraryHeaderLayout(){
  LibraryHeaderLayout layout{};
  int x=40;
  for(int tab=0;tab<libraryTabCount();tab++){
    const std::string label=libraryTabLabel(tab);
    const int width=28+(int)std::lround((12+textW(g_font,label.c_str()))*g_libraryTabOpen[tab]);
    layout.tabs[tab]={x-12,22,width+24,48};
    x+=width+32;
  }
  layout.settings={SW-82,22,54,48};
  return layout;
}

static int libraryHeaderHit(int x,int y){
  const LibraryHeaderLayout layout=libraryHeaderLayout();
  SDL_Point point{x,y};
  if(SDL_PointInRect(&point,&layout.settings)) return libraryTabCount();
  for(size_t index=0;index<layout.tabs.size();index++)
    if(SDL_PointInRect(&point,&layout.tabs[index])) return (int)index;
  return -1;
}

struct GLay { int cols, rows, cw, chh, gapx, gapy, x0, y0, titleH; };
static GLay gridLayout(){
  GLay g;
  g.gapx=32; g.gapy=20; g.titleH=g_showGameTitles?fontHeight(g_font_sm):0;
  // Reserve space for wrapped footer hints.
  const auto hints=libraryFooter();
  int topBar=112,footer=std::max(88,measureFooter(hints.data(),(int)hints.size()).height+32);
  const bool recent=g_libraryView==LibraryView::Recent;
  g.rows=recent?1:g_gridRows;
  int availH = SH - topBar - footer;
  int caption=g.titleH?g.titleH+8:0;
  int maxCoverH=(availH-(g.rows-1)*g.gapy-g.rows*caption)/g.rows;
  if(maxCoverH<72) maxCoverH=72;
  int margin = 56;
  int autoWidth=maxCoverH*2/3;
  g.cols=recent?5:g_gridColumns;
  int maxCoverW=(SW-2*margin-(g.cols-1)*g.gapx)/g.cols;
  g.cw=std::max(48,std::min(autoWidth,maxCoverW));
  g.chh=std::min(maxCoverH,g.cw*3/2);
  g.cw=g.chh*2/3;
  int gridW = g.cols*g.cw + (g.cols-1)*g.gapx;
  g.x0 = (SW - gridW)/2;
  int gridH=g.rows*(g.chh+caption)+(g.rows-1)*g.gapy;
  g.y0=topBar+std::max(0,(availH-gridH)/2);
  return g;
}
static void drawLibraryHeader(){
  const Uint32 now=SDL_GetTicks();
  const float dt=std::min(0.05f,(now-g_libraryAnimationTick)/1000.0f);
  g_libraryAnimationTick=now;
  for(int tab=0;tab<libraryTabCount();tab++){
    const float target=tab==libraryTabIndex()?1.0f:0.0f;
    if(!g_uiAnimations) g_libraryTabOpen[tab]=target;
    else g_libraryTabOpen[tab]+=(target-g_libraryTabOpen[tab])*(1-std::exp(-18*dt));
    if(std::fabs(target-g_libraryTabOpen[tab])<0.004f) g_libraryTabOpen[tab]=target;
    else g_redrawUntil=now+100;
  }
  drawGradient({0,0,SW,140},{0,0,0,110},{0,0,0,0});
  const LibraryHeaderLayout layout=libraryHeaderLayout();
  for(int tab=0;tab<libraryTabCount();tab++){
    const SDL_Rect &rect=layout.tabs[tab];
    const bool focused=g_libraryHeaderFocused&&g_libraryHeaderSelection==tab;
    const SDL_Color color=tab==libraryTabIndex()||focused?COL_TXT:COL_DIM;
    if(focused) roundedPanel(rect.x,rect.y,rect.w,rect.h,{255,255,255,8},COL_SEL,14,2);
    drawLibraryIcon(tab==0?LibraryIcon::Recent:tab==1?LibraryIcon::Games:LibraryIcon::Favorite,rect.x+26,46,28,color);
    if(rect.w>52){
      const SDL_Rect clip{rect.x+52,rect.y,rect.w-52,rect.h};
      SDL_RenderSetClipRect(g_ren,&clip);
      drawText(g_font,clip.x,46-fontHeight(g_font)/2,libraryTabLabel(tab).c_str(),color);
      SDL_RenderSetClipRect(g_ren,nullptr);
    }
  }
  const bool focused=g_libraryHeaderFocused&&g_libraryHeaderSelection==libraryTabCount();
  if(focused) roundedPanel(layout.settings.x,layout.settings.y,layout.settings.w,layout.settings.h,{255,255,255,8},COL_SEL,14,2);
  drawLibraryIcon(LibraryIcon::Settings,layout.settings.x+27,46,28,focused?COL_TXT:COL_DIM);
}

struct RecentLayout { int x,y,focusedWidth,width,titleY; };

static RecentLayout recentLayout(){ return {40,126,240,200,440}; }

static SDL_Rect recentCoverRect(int index,const RecentLayout &layout){
  const float slot=index-g_recentPosition;
  const float focus=std::max(0.0f,1.0f-std::fabs(slot));
  const int step=layout.width+18,expanded=layout.focusedWidth+16-step;
  const int x=layout.x+(int)std::lround(slot*step+std::clamp(slot,0.0f,1.0f)*expanded);
  const int width=layout.width+(int)std::lround((layout.focusedWidth-layout.width)*focus);
  return {x,layout.y,width,width*3/2};
}

static int gridHitTest(int px,int py,int top,int selection){
  const int n=(int)g_visibleGames.size();
  if(g_libraryView==LibraryView::Recent){
    const RecentLayout recent=recentLayout();
    SDL_Point point{px,py};
    if(selection>=0&&selection<n){
      const SDL_Rect rect=recentCoverRect(selection,recent);
      if(SDL_PointInRect(&point,&rect)) return selection;
    }
    for(int index=std::max(0,(int)std::floor(g_recentPosition)-1);index<std::min(n,(int)std::ceil(g_recentPosition)+6);index++){
      const SDL_Rect rect=recentCoverRect(index,recent);
      if(SDL_PointInRect(&point,&rect)) return index;
    }
    return -1;
  }
  GLay L=gridLayout();
  int rowStride=L.chh+(L.titleH?L.titleH+8:0)+L.gapy;
  for(int r=0;r<L.rows;r++) for(int c=0;c<L.cols;c++){
    int idx=(top+r)*L.cols+c; if(idx>=n) continue;
    int x=L.x0+c*(L.cw+L.gapx), y=L.y0+r*rowStride;
    if(px>=x-4 && px<x+L.cw+4 && py>=y-4 && py<y+L.chh+(L.titleH?L.titleH+8:0)) return idx;
  }
  return -1;
}


static void drawTitleCell(int cx,int cellW,int y,const std::string&title,bool sel,SDL_Color col){
  TTF_Font*f=g_font_sm;
  int tw=textW(f,title.c_str());
  if(tw<=cellW){ drawTextC(f,cx,y,title.c_str(),col); return; }
  int x0=cx-cellW/2;
  if(!sel){
    const std::string &shortened=ellipsizedText(f,title,cellW);
    drawTextC(f,cx,y,shortened.c_str(),col);
    return;
  }
  drawScrollTextL(f,x0,y,cellW,title.c_str(),col);
}

static void drawScrollTextR(TTF_Font*f,int xRight,int y,int maxW,const char*s,SDL_Color c){
  if(maxW<=0 || !s || !*s) return;
  int tw=textW(f,s);
  if(tw<=maxW){ drawTextR(f,xRight,y,s,c); return; }
  int x0=xRight-maxW;
  SDL_Rect clip={x0,y-2,maxW,(f?fontHeight(f):26)+6};
  SDL_RenderSetClipRect(g_ren,&clip);
  drawText(f,x0-textScrollOffset(x0,y,tw-maxW,s),y,s,c);
  SDL_RenderSetClipRect(g_ren,nullptr);
}
static void drawScrollTextL(TTF_Font*f,int x,int y,int maxW,const char*s,SDL_Color c){
  if(maxW<=0 || !s || !*s) return;
  int tw=textW(f,s);
  if(tw<=maxW){ drawText(f,x,y,s,c); return; }
  SDL_Rect clip={x,y-2,maxW,(f?fontHeight(f):26)+6};
  SDL_RenderSetClipRect(g_ren,&clip);
  drawText(f,x-textScrollOffset(x,y,tw-maxW,s),y,s,c);
  SDL_RenderSetClipRect(g_ren,nullptr);
}



static void drawLibraryCover(Game &game,const SDL_Rect &rect,bool selected){
  const int x=rect.x,y=rect.y,w=rect.w,h=rect.h;
  ensureCover(game,true);
  fillRect(x+4,y+6,w,h,(SDL_Color){0,0,0,55});
  fillRect(x+2,y+3,w,h,(SDL_Color){0,0,0,70});
  if(game.cover){
    const Uint32 elapsed=SDL_GetTicks()-game.coverAt;
    const Uint8 opacity=!g_uiAnimations?255:elapsed<180?(Uint8)(255*elapsed/180):255;
    SDL_SetTextureAlphaMod(game.cover,opacity);
    const Uint8 tint=selected?255:225;
    SDL_SetTextureColorMod(game.cover,tint,tint,tint);
    SDL_RenderCopy(g_ren,game.cover,nullptr,&rect);
  } else {
    fillRect(x,y,w,h,COL_CARD);
    drawTextC(g_font_sm,x+w/2,y+h/2-8,fittedText(g_font_sm,tr("NO COVER"),w-12).c_str(),COL_DIM);
  }
  border(x,y,w,h,1,(SDL_Color){12,13,18,255});
  fillRect(x,y,w,1,(SDL_Color){255,255,255,26});
  if(selected) border(x-3,y-3,w+6,h+6,2,COL_SEL);
  if(g_showRegionFlags&&game.region>0&&g_flag[game.region]){
    const int fw=std::clamp(w*26/100,16,30),fh=fw*2/3;
    SDL_Rect flag{x+6,y+6,fw,fh}; SDL_RenderCopy(g_ren,g_flag[game.region],nullptr,&flag);
    border(flag.x,flag.y,fw,fh,1,(SDL_Color){10,12,18,255});
  }
  if(g_showCustomSettingsBadges&&game.hasCfg){
    const int size=26,icon=18,bx=x+w-size-6,by=y+6;
    roundedPanel(bx,by,size,size,{8,12,24,232},{235,239,247,80},size/2);
    drawLibraryIcon(LibraryIcon::Settings,bx+size/2,by+size/2,icon,COL_TXT);
  }
  if(g_favorites.count(game.key)){
    roundedRect(x+w-31,y+h-31,26,26,13,(SDL_Color){8,12,24,225});
    drawLibraryIcon(LibraryIcon::Favorite,x+w-18,y+h-18,18,COL_VAL);
  }
}

static void drawWrappedCentered(TTF_Font *font,int centerX,int y,int maxWidth,int lineHeight,
                                int maxLines,const char *text,SDL_Color color) {
  if(!text||!*text) return;
  std::string input=text,line; int drawn=0;
  auto emit=[&](const std::string &value){
    if(drawn<maxLines){ drawTextC(font,centerX,y+drawn*lineHeight,value.c_str(),color); drawn++; }
  };
  size_t index=0;
  while(index<input.size()&&drawn<maxLines){
    size_t end=index; while(end<input.size()&&input[end]!=' '&&input[end]!='\n') end++;
    std::string word=input.substr(index,end-index);
    std::string candidate=line.empty()?word:line+" "+word;
    if(textW(font,candidate.c_str())>maxWidth&&!line.empty()){ emit(line); line=word; }
    else line=std::move(candidate);
    if(end<input.size()&&input[end]=='\n'){ emit(line); line.clear(); }
    index=end+1;
  }
  if(!line.empty()&&drawn<maxLines) emit(line);
}

static SDL_Rect libraryEmptyActionRect(){ return {SW/2-148,SH/2+82,296,48}; }

static void drawLibraryEmpty(){
  if(!g_libraryEmptyShown&&(g_libraryScan||g_libraryStoragePending)) return;
  g_libraryEmptyShown=true;
  const char *title=g_libraryView==LibraryView::Recent?"Nothing played yet":
      g_libraryView==LibraryView::Favorites?"No favorites yet":"No games found";
  const char *description=g_libraryView==LibraryView::Recent?"Games you launch appear here, most recent first.":
      g_libraryView==LibraryView::Favorites?"Mark games as favorites from their game menu.":
      "Add game folders in Settings to populate your library.";
  drawTextC(g_font_big,SW/2,SH/2-80,tr(title),COL_TXT);
  drawWrappedCentered(g_font_sm,SW/2,SH/2-22,SW-240,30,2,tr(description),COL_DIM);
  const SDL_Rect action=libraryEmptyActionRect();
  roundedPanel(action.x,action.y,action.w,action.h,COL_FOCUS,g_libraryHeaderFocused?COL_DIM:COL_SEL,12);
  drawTextC(g_font_sm,SW/2,action.y+(action.h-fontHeight(g_font_sm))/2,
            tr(g_libraryView==LibraryView::All?"Settings":"All games"),COL_TXT);
}

static void renderGrid(int selection,int top){
  g_cover_budget=COVER_REQUEST_BUDGET;
  Game *selected=visibleGame(selection);
  g_backgroundTarget=selected?selected->key:std::string{};
  clearUiBackground();
  if(g_launcherTheme==LauncherTheme::Classic){
    for(int direction:{-1,1}) if(Game *neighbor=visibleGame(selection+direction))
      queueCoverDecode(*neighbor,false,true);
  }
  if(selected) ensureCover(*selected,true);
  const GLay layout=gridLayout();
  const int count=(int)g_visibleGames.size(),per=layout.cols*layout.rows;
  if(count) g_libraryEmptyShown=false;
  const int page=count?selection/per:0;
  drawLibraryHeader();
  if(count&&g_libraryView==LibraryView::Recent){
    const RecentLayout recent=recentLayout();
    const float target=(float)selection;
    if(g_recentPosition<0||!g_uiAnimations) g_recentPosition=target;
    else g_recentPosition+=(target-g_recentPosition)*0.24f;
    if(std::fabs(target-g_recentPosition)<0.005f) g_recentPosition=target;
    else g_redrawUntil=SDL_GetTicks()+100;
    const SDL_Rect clip{0,recent.y-8,SW,recent.focusedWidth*3/2+16};
    SDL_RenderSetClipRect(g_ren,&clip);
    for(int index=std::max(0,(int)std::floor(g_recentPosition)-1);index<std::min(count,(int)std::ceil(g_recentPosition)+6);index++)
      if(index!=selection) drawLibraryCover(*visibleGame(index),recentCoverRect(index,recent),false);
    const SDL_Rect focused=recentCoverRect(selection,recent);
    drawLibraryCover(*selected,focused,!g_libraryHeaderFocused);
    SDL_RenderSetClipRect(g_ren,nullptr);
    const int titleX=focused.x+focused.w+16;
    drawScrollTextL(g_font,titleX,recent.titleY,SW-40-titleX,selected->title.c_str(),COL_TXT);
  } else if(count){
    const int rowStride=layout.chh+(layout.titleH?layout.titleH+8:0)+layout.gapy;
    for(int row=0;row<layout.rows;row++) for(int column=0;column<layout.cols;column++){
      const int index=(top+row)*layout.cols+column;
      if(index>=count) continue;
      Game &game=*visibleGame(index);
      const SDL_Rect rect{layout.x0+column*(layout.cw+layout.gapx),layout.y0+row*rowStride,layout.cw,layout.chh};
      const bool current=index==selection&&!g_libraryHeaderFocused;
      drawLibraryCover(game,rect,current);
      if(g_showGameTitles) drawTitleCell(rect.x+rect.w/2,rect.w,rect.y+rect.h+6,game.title,current,current?COL_VAL:COL_DIM);
    }
    const int prefetchStart=(page+1)*per;
    for(int index=prefetchStart;index<std::min(count,prefetchStart+per);index++) ensureCover(*visibleGame(index));
  } else drawLibraryEmpty();
  drawUpdateNotification();
  if(g_libraryHeaderFocused){
    FootItem hints[]={{"A",g_libraryHeaderSelection==libraryTabCount()?"Settings":"Open"},{"B","Back"}};
    drawFooterHints(hints,2,SH-50);
  } else {
    if(count){
      const auto hints=libraryFooter();
      drawFooterHints(hints.data(),(int)hints.size(),SH-50);
    } else {
      FootItem hints[]={{"A",g_libraryView==LibraryView::All?"Settings":"All games"},{"B","Quit"}};
      drawFooterHints(hints,2,SH-50);
    }
  }
  drawFadeIn(); SDL_RenderPresent(g_ren);
}



static int gridNav(int sel,int dx,int dy,int cols,int rows,int n){
  if(n<=0) return 0;
  int per=cols*rows, page=sel/per, pos=sel%per, cr=pos/cols, cc=pos%cols;
  auto clamp=[&](int i){ return i>=n? n-1 : (i<0?0:i); };
  if(dx>0){
    if(cc<cols-1 && page*per+cr*cols+cc+1 < n) return page*per+cr*cols+cc+1;
    if((page+1)*per < n) return clamp((page+1)*per + cr*cols);
    return sel;
  }
  if(dx<0){
    if(cc>0) return sel-1;
    if(page>0) return clamp((page-1)*per + cr*cols + (cols-1));
    return sel;
  }
  if(dy>0){
    if(cr<rows-1 && page*per+(cr+1)*cols+cc < n) return page*per+(cr+1)*cols+cc;
    return sel;
  }
  if(dy<0){
    if(cr>0) return sel-cols;
    return sel;
  }
  return sel;
}

static int gridPage(int sel,int dir,int cols,int rows,int n){
  if(n<=0) return 0;
  int per=cols*rows, pos=sel%per, maxpage=(n-1)/per;
  int np=sel/per + dir; if(np<0) np=0; if(np>maxpage) np=maxpage;
  int i=np*per+pos; return i>=n? n-1 : i;
}

static bool ensureDirectory(const char *path) {
  if (mkdir(path, 0777) == 0) return true;
  if (errno != EEXIST) return false;
  struct stat st{};
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void cleanupLauncher() {
  LauncherUpdate_Shutdown();
  stopGameScan();
  stopCoverDecodeWorker();
  for (auto &game : g_games) {
    releaseGameArtwork(game);
  }
  clearTextCaches();
  for (int i = 1; i < 4; i++) {
    if (g_flag[i]) SDL_DestroyTexture(g_flag[i]);
    g_flag[i] = nullptr;
  }
  destroyGlyphs();
  if (g_logo) SDL_DestroyTexture(g_logo);
  g_logo = nullptr;
  if (g_glowTexture) SDL_DestroyTexture(g_glowTexture);
  g_glowTexture = nullptr;
  if (g_roundTexture) SDL_DestroyTexture(g_roundTexture);
  g_roundTexture = nullptr;

  if (g_font) TTF_CloseFont(g_font);
  if (g_font_sm) TTF_CloseFont(g_font_sm);
  if (g_font_big) TTF_CloseFont(g_font_big);
  if (g_font_caption) TTF_CloseFont(g_font_caption);
  if (g_font_hint) TTF_CloseFont(g_font_hint);
  g_font = g_font_sm = g_font_big = g_font_caption = g_font_hint = nullptr;
  g_requestedFontType = PlSharedFontType_Total;
  if (g_plReady) plExit();
  g_plReady = false;

  uiAudioShutdown();
  SwitchStorage::Shutdown();
  closeController();
  if (g_ren) SDL_DestroyRenderer(g_ren);
  if (g_win) SDL_DestroyWindow(g_win);
  g_ren = nullptr;
  g_win = nullptr;
  if (g_imgReady) IMG_Quit();
  if (g_ttfReady) TTF_Quit();
  if (g_sdlReady) SDL_Quit();
  g_imgReady = g_ttfReady = g_sdlReady = false;

  if (g_griddbReady) griddb_global_exit();
  g_griddbReady = false;
  if (g_storageSocketReady) socketExit();
  g_storageSocketReady = false;
  if (g_romfsReady) romfsExit();
  g_romfsReady = false;
}

static int startupFailure(const char *message) {
  if (g_sdlReady)
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Cemu Launcher", message, g_win);
  cleanupLauncher();
  return 1;
}

static bool isAppletMode() {
  const AppletType type=appletGetAppletType();
  return type!=AppletType_Application&&type!=AppletType_SystemApplication;
}

static void showLaunchError(SwitchLaunch::Error error) {
  using SwitchLaunch::Error;
  if(error==Error::None) return;
  if(error==Error::AddressSpace36 || error==Error::AddressSpaceUnsupported){
    modalMessageStatic("39-bit forwarder required",{
      std::string(LauncherLocalization::Translate(error==Error::AddressSpace36 ?
        "Cemu NX is running in 36-bit mode." : "This address-space mode is not supported.")),
      "Recreate the forwarder with the built-in applet installer,",
      "or use Sphaira and select a 39-bit address space.",
      "Then launch Cemu from the new HOME Menu shortcut."});
  } else if(error==Error::AddressSpaceUnknown){
    modalMessageStatic("Cannot start game",{
      "Cemu NX could not verify the process address space.",
      "Launch through a 39-bit HOME Menu forwarder."});
  } else if(error==Error::VulkanRequired){
    modalMessageStatic("Vulkan required",{
      "Host memory requires the Vulkan renderer.",
      "Select Vulkan in the launcher graphics settings."});
  } else if(error==Error::HostMemoryUnavailable){
    modalMessageStatic("Host memory unavailable",{
      "Cemu NX could not initialize host memory.",
      "Close Cemu and launch it again from the HOME Menu.",
      "Details: switch/cemu/last-core-error.log"});
  } else {
    modalMessageStatic("Cannot start game",{
      "Cemu NX could not start the game.",
      "Details: switch/cemu/last-core-error.log"});
  }
}

static void runAppletInstaller() {
  LauncherLocalization::Initialize("system");
  const int panelWidth=std::min(SW-96,960),panelHeight=std::min(SH-96,500);
  const int panelX=(SW-panelWidth)/2,panelY=(SH-panelHeight)/2;
  const int buttonWidth=std::min(520,panelWidth-96),buttonHeight=76;
  const int buttonX=(SW-buttonWidth)/2,buttonY=panelY+panelHeight-buttonHeight-48;
  beginScreenFx();
  for(;;){
    if(!beginUiFrame()) return;
    SDL_Event event{}; navRepeat();
    while(pollUiEvent(event)){
      int tx=0,ty=0; const TouchKind touch=touchFeed(event,&tx,&ty);
      const bool pressed=event.type==SDL_CONTROLLERBUTTONDOWN&&event.cbutton.button==BTN_CONFIRM;
      const bool touched=touch==TOUCH_TAP&&tx>=buttonX&&tx<buttonX+buttonWidth&&ty>=buttonY&&ty<buttonY+buttonHeight;
      if(pressed||touched){
        clearUiBackground();
        drawHeader(LauncherLocalization::Translate("Applet mode installer").data(),nullptr);
        { const int waitW=std::min(940,SW-64),waitH=140;
          glassPanel((SW-waitW)/2,(SH-waitH)/2,waitW,waitH); }
        drawTextC(g_font,SW/2,SH/2,LauncherLocalization::Translate("Installing...").data(),COL_VAL);
        SDL_RenderPresent(g_ren);
        appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
        char error[256]{}; const bool installed=forwarder_create_launcher(error,sizeof(error));
        appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
        if(installed) modalMessageStatic("Cemu",{std::string(LauncherLocalization::Translate("HOME Menu shortcut installed."))});
        else modalMessageStatic("Shortcut failed",{std::string(error[0]?error:"Unknown error")});
        beginScreenFx();
      }
      if(event.type==SDL_CONTROLLERBUTTONDOWN&&event.cbutton.button==BTN_CANCEL) return;
    }
    clearUiBackground();
    glassPanel(panelX,panelY,panelWidth,panelHeight);
    drawText(g_font_big,panelX+28,panelY+22,
             fittedText(g_font_big,std::string(LauncherLocalization::Translate("Applet mode installer")),
                        panelWidth-56).c_str(),COL_TXT);
    fillRect(panelX+28,panelY+72,panelWidth-56,1,(SDL_Color){255,255,255,24});
    const std::vector<std::string> lines=wrapDialogLines({
      std::string(LauncherLocalization::Translate("Cemu is running in applet mode.")),
      std::string(LauncherLocalization::Translate("Install a HOME Menu shortcut to run Cemu with full memory and normal performance."))
    },panelWidth-96);
    int lineY=panelY+132;
    for(const std::string &line:lines){
      if(lineY+fontHeight(g_font)>=buttonY-28) break;
      drawTextC(g_font,SW/2,lineY,line.c_str(),COL_TXT); lineY+=fontHeight(g_font)+12;
    }
    drawButtonPanel(buttonX,buttonY,buttonWidth,buttonHeight,true);
    drawTextC(g_font,SW/2,buttonY+(buttonHeight-fontHeight(g_font))/2,
              LauncherLocalization::Translate("Install").data(),COL_VAL);
    drawLocalizedFooter("A  Install       B  Back",panelY+panelHeight-18);
    drawFadeIn(); SDL_RenderPresent(g_ren); waitForNextUiFrame();
  }
}

static void initializeLauncherDefaults() {
  g_active=&g_global;
  saveGameSources({DEF_GAMEDIR});
  for(const Opt &option:S_launcher)
    storeSet(g_global,option.key,option.def);
  storeSet(g_global,"Wrapper/SteamGridDBKey","");
  storeSet(g_global,"Wrapper/Language","system");
  storeSet(g_global,"Wrapper/Renderer","vk");
  storeSet(g_global,"console_language","-1");
  storeSet(g_global,"Wrapper/InstalledReleaseTag",LauncherUpdate_BuiltReleaseTag());
  commitAll();
}

static bool writeLaunchHandoff(const std::vector<CemuKV> &settings,const std::string &renderer,
                              const std::string &gamePath,uint64_t titleId,bool lsfgPrepared) {
  std::string handoff;
  if(!appendHandoffValue(handoff,"timer_shift",cemuKVGet(settings,"TimerShiftFactor","3")) ||
     !appendHandoffValue(handoff,"triple_buffer",cemuKVGet(settings,"TripleBuffer","1")) ||
     !appendHandoffValue(handoff,"cpu_mode",cemuKVGet(settings,"cpuMode","3")) ||
     !appendHandoffValue(handoff,"renderer",renderer) ||
     !appendHandoffValue(handoff,"gamepad_layout",cemuKVGet(settings,"GamePadLayout","off")) ||
     !appendHandoffValue(handoff,"lsfg_enabled",lsfgPrepared?"true":"false") ||
     !appendHandoffValue(handoff,"lsfg_flow_scale",cemuKVGet(settings,"Wrapper/LSFGFlowScale","0.25")) ||
     !appendHandoffValue(handoff,"lsfg_performance",cemuKVGet(settings,"Wrapper/LSFGPerformance","true")) ||
     !appendHandoffValue(handoff,"usb_skylanders",cemuKVGet(settings,"UsbSkylanders","false")) ||
     !appendHandoffValue(handoff,"usb_infinity",cemuKVGet(settings,"UsbInfinity","false")) ||
     !appendHandoffValue(handoff,"usb_dimensions",cemuKVGet(settings,"UsbDimensions","false")))
    return false;
  if(!gamePath.empty()) {
    if(!appendHandoffValue(handoff,"game",gamePath)) return false;
  } else if(titleId) {
    char gameId[32];
    snprintf(gameId,sizeof(gameId),"id:%016llx",(unsigned long long)titleId);
    if(!appendHandoffValue(handoff,"game",gameId)) return false;
  }
  std::string usbId,physicalId,relative;
  if(!gamePath.empty() && bindMountedUsbPath(gamePath,SwitchStorage::ListUsbLocations(),usbId,physicalId,relative) &&
     (!appendHandoffValue(handoff,"game_usb_id",usbId) || !appendHandoffValue(handoff,"game_usb_relative",relative)))
    return false;
  return writeAtomicText(LAUNCH_HANDOFF,handoff);
}

int main(int argc, char **argv){
  if(argc>=1 && argv[0] && argv[0][0]){
    g_forwarderSelfPath=argv[0];
    setLauncherPathFromArg(argv[0]);
  }
  std::string positionalForwarderPath;
  if(argc>=2&&argv[1]&&argv[1][0]&&argv[1][0]!='-'){
    const std::string candidate=normalizeLocationPath(argv[1]);
    if(candidate.find(":/")!=std::string::npos) positionalForwarderPath=candidate;
  }
  std::string updateRecoveryError;
  const bool updateRecoveryOk=LauncherUpdate_RecoverInstallation(g_launcherNroPath,updateRecoveryError);
  if (R_FAILED(romfsInit())) return 1;
  g_romfsReady = true;
  detectSystemLanguage();
  storeLoad(g_global,LAUNCHER_INI);

  const auto pendingLaunchError=SwitchLaunch::TakeError();
  const auto addressSpaceError=SwitchLaunch::AddressSpaceError(SwitchAddressSpaceBits());
  const bool allowShortcutAutolaunch=SwitchLaunch::AllowShortcutAutolaunch(
    pendingLaunchError,addressSpaceError,isAppletMode());

  if(!positionalForwarderPath.empty() && allowShortcutAutolaunch){
    const char *directories[]={
      "sdmc:/switch",DATA_DIR,EMU_HOST_DIR,COVERS_DIR,GAMECFG_DIR,DEF_GAMEDIR,
      GAMEPROFILES_DIR,GRAPHICPACKS_DIR,LSFG_DIR,"sdmc:/switch/cemu/cache",
      "sdmc:/switch/cemu/install","sdmc:/switch/cemu/mlc01"
    };
    for(const char *directory:directories) if(!ensureDirectory(directory)){
      cleanupLauncher();
      return 1;
    }
    cleanupLegacyEmuHosts();
    if(!updateRecoveryOk){
      cleanupLauncher();
      return 1;
    }

    struct stat configStat{};
    const bool firstRunHeadless=stat(LAUNCHER_INI,&configStat)!=0;
    storeLoad(g_titles,TITLES_INI);
    storeLoad(g_recent,RECENT_INI);
    storeLoad(g_containerTitles,CONTAINER_TITLES_INI);
    storeLoad(g_gameIdentities,GAME_IDENTITIES_INI);
    loadLibraryOrganization();

    if(firstRunHeadless){
      initializeLauncherDefaults();
      if(!storeSave(g_global,LAUNCHER_INI)){
        cleanupLauncher();
        return 1;
      }
    }

    const bool directUsesUsb=isUsbStoragePath(positionalForwarderPath);
    if(directUsesUsb&&!SwitchStorage::InitializeUsb()){
      cleanupLauncher();
      return 1;
    }
    std::vector<SwitchStorage::SmbShare> directShares=loadSmbSharesFromStore();
    const SwitchStorage::SmbShare *directSmbShare=nullptr;
    for(const auto &share:directShares){
      if(pathAtOrBelow(positionalForwarderPath,SwitchStorage::SmbRootPath(share.id))){
        directSmbShare=&share;
        break;
      }
    }
    if(directSmbShare){
      if(R_FAILED(socketInitializeDefault())){
        cleanupLauncher();
        return 1;
      }
      g_storageSocketReady=true;
      std::string error;
      if(!SwitchStorage::MountSmb(*directSmbShare,&error)){
        cleanupLauncher();
        return 1;
      }
    }

    std::vector<std::string> gamePathsHeadless=loadGameSources();
    refreshConfiguredUsbSources(gamePathsHeadless);
    gamePathsHeadless.erase(std::remove_if(gamePathsHeadless.begin(),gamePathsHeadless.end(),
      isUnresolvedUsbSource),gamePathsHeadless.end());

    auto resolveDirectGame=[&](Game &output)->bool{
      const std::string &directPath=positionalForwarderPath;
      struct stat info{};
      const bool exists=SwitchStorage::GetCachedSmbStat(directPath,&info)||stat(directPath.c_str(),&info)==0;
      if(!exists) return false;
      const bool isDirectory=S_ISDIR(info.st_mode);
      const bool isGameDirectory=isDirectory&&directoryIsTitle(directPath);
      const size_t slash=directPath.find_last_of("/\\");
      const std::string file=slash==std::string::npos?directPath:directPath.substr(slash+1);
      if(!isGameDirectory&&(isDirectory||!hasGameExt(file.c_str()))) return false;

      Game game;
      game.file=file;
      game.path=directPath;
      game.storageId=usbStableIdForPath(directPath);
      game.legacyKey=makeGameKey(game.file,directPath);
      game.key=game.legacyKey;
      game.added=(long long)info.st_mtime;
      game.modified=game.added;
      game.fileSize=(long long)info.st_size;

      if(!isDirectory){
        const char *cached=storeGet(g_containerTitles,game.legacyKey.c_str(),"");
        long long size=0,mtime=0;unsigned long long titleId=0,fingerprint=0;int consumed=0;
        const int parsed=sscanf(cached,"%lld,%lld,%llx,%llx%n",&size,&mtime,&titleId,&fingerprint,&consumed);
        if(parsed>=3&&cached[consumed]==0&&size==game.fileSize&&mtime==game.modified&&(titleId>>48)==0x0005){
          game.titleId=(uint64_t)titleId;
          if(parsed==4) game.fingerprint=(uint64_t)fingerprint;
        }
      }
      const auto titleCache=cemu_loadTitleCache(std::string(DATA_DIR)+"/title_list_cache.xml");
      if(!game.titleId) game.titleId=cemu_resolveBaseTitleId(directPath,titleCache,&game.titleIdError);
      if(!isDirectory&&!game.fingerprint) game.fingerprint=fingerprintGameFile(directPath,info);
      if(isDirectory&&!game.fingerprint) game.fingerprint=fingerprintGameDirectory(directPath,game.titleId);

      Store refreshedIdentities=g_gameIdentities;
      std::unordered_set<std::string> usedIdentities,reservedIds,reservedCanonicalPaths;
      for(const KV &entry:refreshedIdentities.kv){
        GameIdentityRecord record;
        if(!parseIdentityRecord(entry,record)||record.retired) continue;
        reservedIds.insert(record.key);
        if(!record.canonicalPath.empty()&&identityPathExists(record))
          reservedCanonicalPaths.insert(record.canonicalPath);
      }
      const std::string canonical=canonicalGamePath(directPath);
      game.key=choosePersistentGameKey(refreshedIdentities,refreshedIdentities,usedIdentities,
                                       reservedIds,reservedCanonicalPaths,game.titleId,
                                       game.fingerprint,identityFormat(directPath,isDirectory),
                                       canonical,directPath);
      g_gameIdentities=std::move(refreshedIdentities);
      if(!storeSave(g_gameIdentities,GAME_IDENTITIES_INI)) return false;
      migrateGameIdentity(game);

      const char *custom=storeGet(g_titles,game.key.c_str(),"");
      if(!custom[0]) custom=storeGet(g_titles,game.legacyKey.c_str(),"");
      game.title=custom[0]?custom:cleanTitle(game.file);
      if(!custom[0]&&nameIsBareTitleId(game.file)){
        std::string cached=cemu_titleNameFromCache(titleCache,game.titleId);
        if(!cached.empty()) game.title=std::move(cached);
      }
      game.region=detectRegion(game.file);
      const char *played=storeGet(g_recent,game.key.c_str(),"");
      if(!played[0]) played=storeGet(g_recent,game.legacyKey.c_str(),"0");
      game.played=atoll(played);
      game.hasCfg=gameFileExists(GAMECFG_DIR,game,".ini")||
                  regularFileExists(std::string(GAMECFG_DIR)+"/"+game.legacyKey+".ini");

      if(!isDirectory&&game.titleId){
        char cached[128];
        snprintf(cached,sizeof(cached),"%lld,%lld,%016llx,%016llx",game.fileSize,game.modified,
                 (unsigned long long)game.titleId,(unsigned long long)game.fingerprint);
        storeSet(g_containerTitles,game.key.c_str(),cached);
        storeSet(g_containerTitles,game.legacyKey.c_str(),cached);
        if(!storeSave(g_containerTitles,CONTAINER_TITLES_INI)) return false;
      }
      output=std::move(game);
      return true;
    };

    Game directGame;
    bool directMatched=false;
    const int attempts=directUsesUsb?100:1;
    for(int attempt=0;attempt<attempts;attempt++){
      if(resolveDirectGame(directGame)){directMatched=true;break;}
      if(directUsesUsb) svcSleepThread(100000000LL);
    }
    if(!directMatched){
      cleanupLauncher();
      return 1;
    }

    recordPlayed(directGame);
    g_active=&g_global;
    commitAll();
    if(!storeSave(g_global,LAUNCHER_INI)||!storeSave(g_recent,RECENT_INI)||!envHasNextLoad()){
      cleanupLauncher();
      return 1;
    }

    std::vector<CemuKV> effective=buildEffectiveSettings(directGame.key);
    const char *configuredRenderer=cemuKVGet(effective,"Wrapper/Renderer","vk");
    const std::string renderer=!strcmp(configuredRenderer,"gl")?"gl":
                               !strcmp(configuredRenderer,"zink")?"zink":"vk";
    const bool haveEmu=ensureEmu();
    appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
    std::vector<CemuGraphicPack> enabledPacks;
    bool configOk=haveEmu&&readEnabledPacks(enabledPacks);
    if(configOk) configOk=cemu_writeSettingsXml(SETTINGS_XML,effective,gamePathsHeadless,enabledPacks);
    if(configOk) configOk=writeInputIni(effective);
    if(configOk&&directGame.titleId)
      configOk=cemu_writeGameProfile(GAMEPROFILES_DIR,directGame.titleId,
                                     directGame.title.c_str(),effective);
    if(configOk){
      const bool lsfgPrepared=renderer=="vk" &&
        !strcmp(cemuKVGet(effective,"Wrapper/LSFGEnabled","false"),"true") &&
        regularFileExists(LSFG_DLL_FILE);
      configOk=writeLaunchHandoff(effective,renderer,directGame.path,directGame.titleId,lsfgPrepared);
    }
    appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
    if(!configOk) remove(LAUNCH_HANDOFF);
    cleanupLauncher();
    if(configOk) envSetNextLoad(EMU_NRO_DST,EMU_NRO_DST);
    return configOk?0:1;
  }

  LauncherLocalization::Initialize(isAppletMode()?"system":storeGet(g_global,"Wrapper/Language","system"));
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"1");
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY,"linear");
  if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMECONTROLLER)!=0) return startupFailure("SDL initialization failed.");
  g_sdlReady = true;
  LauncherUpdate_SetWakeCallback([](void*){ wakeUiFromWorker(0x55504454); },nullptr);
  uiAudioInit();
  if(TTF_Init()!=0) return startupFailure("Font initialization failed.");
  g_ttfReady = true;
  const int imageFlags = IMG_INIT_PNG | IMG_INIT_JPG;
  if((IMG_Init(imageFlags) & imageFlags) != imageFlags) return startupFailure("Image initialization failed.");
  g_imgReady = true;
  int windowWidth=1280,windowHeight=720;
  if(appletGetOperationMode()==AppletOperationMode_Console){ windowWidth=1920; windowHeight=1080; }
  g_win=SDL_CreateWindow("Cemu",0,0,windowWidth,windowHeight,SDL_WINDOW_FULLSCREEN);
  if(!g_win) return startupFailure("Could not create the launcher window.");
  g_ren=SDL_CreateRenderer(g_win,-1,SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
  if(!g_ren) return startupFailure("Could not create the launcher renderer.");
  SDL_RendererInfo rendererInfo{};
  g_presentVsync=SDL_GetRendererInfo(g_ren,&rendererInfo)==0&&
                 (rendererInfo.flags&SDL_RENDERER_PRESENTVSYNC)!=0;
  SDL_SetRenderDrawBlendMode(g_ren,SDL_BLENDMODE_BLEND);
  if(SDL_GetRendererOutputSize(g_ren,&g_outputW,&g_outputH)!=0)
    return startupFailure("Could not query the display size.");
  // Establishes SW/SH (1280x720) and the render scale before anything measures text.
  configureLauncherScale();
  { SDL_Surface *ls=IMG_Load("romfs:/logo.png"); if(ls){ g_logo=SDL_CreateTextureFromSurface(g_ren,ls); SDL_FreeSurface(ls); } }
  makeFlags();

  for(int i=0;i<SDL_NumJoysticks();i++) if(SDL_IsGameController(i)){ openController(i); break; }

  if(R_FAILED(plInitialize(PlServiceType_User))) return startupFailure("System font service initialization failed.");
  g_plReady = true;
  if(!loadLauncherFonts()){
    LauncherLocalization::Initialize("en");
    if(!loadLauncherFonts()) return startupFailure("Could not load the system font.");
  }
  if(isAppletMode()){
    (void)ensureDirectory("sdmc:/switch");
    (void)ensureDirectory(DATA_DIR);
    runAppletInstaller();
    cleanupLauncher();
    return 0;
  }
  showLaunchError(addressSpaceError!=SwitchLaunch::Error::None?addressSpaceError:pendingLaunchError);
  g_griddbReady = griddb_global_init();
  if(!g_griddbReady && R_SUCCEEDED(socketInitializeDefault())) g_storageSocketReady=true;

  const char *directories[] = {
    "sdmc:/switch", DATA_DIR, EMU_HOST_DIR, COVERS_DIR, GAMECFG_DIR, DEF_GAMEDIR,
    GAMEPROFILES_DIR, GRAPHICPACKS_DIR, LSFG_DIR, "sdmc:/switch/cemu/cache",
    "sdmc:/switch/cemu/install", "sdmc:/switch/cemu/mlc01"
  };
  for (const char *directory : directories)
    if (!ensureDirectory(directory)) return startupFailure("Could not create the Cemu data directories.");
  cleanupLegacyEmuHosts();

  if(!updateRecoveryOk)
    modalMessageStatic("Update recovery failed",{updateRecoveryError,"The installed launcher was left unchanged."});

  struct stat bst;
  bool firstRun = (stat(LAUNCHER_INI, &bst) != 0);
  const bool themeMigrated=normalizeLauncherTheme(g_global);
  storeLoad(g_titles, TITLES_INI);
  storeLoad(g_recent, RECENT_INI);
  storeLoad(g_containerTitles, CONTAINER_TITLES_INI);
  storeLoad(g_gameIdentities, GAME_IDENTITIES_INI);
  loadLibraryOrganization();

  if (firstRun) {
    initializeLauncherDefaults();
    storeSave(g_global,LAUNCHER_INI);
  } else {
    bool changed=themeMigrated;
    int columns=atoi(storeGet(g_global,"Wrapper/GridColumns","5"));
    if(columns<3||columns>8){ storeSet(g_global,"Wrapper/GridColumns","5"); changed=true; }
    if(changed) storeSave(g_global,LAUNCHER_INI);
  }
  const std::string fallbackLanguage(LauncherLocalization::Preference());
  LauncherLocalization::Initialize(storeGet(g_global,"Wrapper/Language","system"));
  if(!loadLauncherFonts()){
    LauncherLocalization::Initialize(fallbackLanguage);
    if(!loadLauncherFonts()) return startupFailure("Could not load the system font.");
    storeSet(g_global,"Wrapper/Language",fallbackLanguage.c_str());
    storeSave(g_global,LAUNCHER_INI);
  }
  applyLauncherAppearance();
  uiAudioSetEnabled(strcmp(storeGet(g_global,"Wrapper/UiSounds","true"),"false")!=0);
  startCoverDecodeWorker();
  std::vector<std::string> gamePaths=loadGameSources();
  bool hasUsbSource=hasConfiguredUsbSource(gamePaths)||hasConfiguredUsbBinding();
  std::atomic<bool> storageInitDone{false},storageInitCancel{false};
  std::vector<GameSourceRecord> storageRebound;
  bool storageReboundPersist=false;
  std::thread storageInitWorker;
  SwitchStorage::UsbSnapshot usbSnapshot,storageResolvedSnapshot;
  bool storageIntegrated=false,storageRefreshPending=false,storageMountPending=true;
  bool storageFirstPass=true;
  uint64_t sourceConfigVersion=0,storageRequestVersion=0,storageAppliedVersion=0;
  uint64_t usbGeneration=0;
  SwitchStorage::SetUsbStatusCallback(usbStatusWake,nullptr);
  auto startStorageRefresh=[&](){
    auto records=loadSavedGameSourceRecords();
    auto shares=storageMountPending?loadSmbSharesFromStore():std::vector<SwitchStorage::SmbShare>{};
    const bool initializeUsb=hasUsbSource;
    storageRequestVersion=sourceConfigVersion;
    storageRebound.clear();storageReboundPersist=false;
    storageInitDone=false;storageIntegrated=false;
    storageRefreshPending=false;storageMountPending=false;
    g_libraryStoragePending=true;
    storageInitWorker=std::thread([&,records=std::move(records),shares=std::move(shares),initializeUsb]() mutable {
      if(initializeUsb&&!storageInitCancel.load())
        SwitchStorage::InitializeUsb();
      for(const auto &share:shares){
        if(storageInitCancel.load()) break;
        if(share.autoMount){ std::string error; SwitchStorage::MountSmb(share,&error,&storageInitCancel); }
      }
      storageResolvedSnapshot=SwitchStorage::GetUsbSnapshot();
      if(!storageInitCancel.load())
        storageRebound=resolveGameSourceRecords(std::move(records),storageResolvedSnapshot.locations,
                                               true,&storageReboundPersist);
      storageInitDone=true;
      wakeUiFromWorker(0x53544f52);
    });
  };
  startStorageRefresh();
  startGameScan(localGameSources(gamePaths),true);

  if (!cemu_hasConfiguredDiscKey("sdmc:/switch/cemu/keys.txt"))
    modalMessageStatic("Disc key required", {
      "Cemu/keys.txt does not contain a Wii U disc key.",
      "WUX games cannot be decrypted until a valid key is added.",
      "",
      "Add the key to sdmc:/switch/cemu/keys.txt." });

  int sel=0, top=0, rows=1;
  bool running=true, launch=false,userExit=false;
  std::string launchKey;
  std::string launchPath;
  uint64_t launchTitleId=0;

  auto selectGame = [&](Game &game) {
    if(addressSpaceError!=SwitchLaunch::Error::None){
      showLaunchError(addressSpaceError);
      return;
    }
    recordPlayed(game);
    launchKey = game.key;
    launchPath = game.path;
    launchTitleId = game.titleId;
    launch = true;
    running = false;
  };
  auto requestExit=[&](){
    if(!confirmBox(LauncherLocalization::Translate("Exit Cemu?").data(),{
         std::string(LauncherLocalization::Translate("Active scans and network operations will be cancelled safely.")),
         std::string(LauncherLocalization::Translate("Return to the HOME Menu?"))})){
      beginScreenFx(); return false;
    }
    userExit=true; running=false; return true;
  };

  bool forwarderRequested=false,forwarderMatched=false;
  std::string forwarderKey;
  for(int ai=allowShortcutAutolaunch?1:argc; ai+1<argc; ai++) if(strcmp(argv[ai],"-g")==0){
    forwarderRequested=true;
    forwarderKey=argv[ai+1];
    if (Game *game = findGameByKey(forwarderKey)){ selectGame(*game); forwarderMatched=true; }
    break;
  }
  if(!forwarderRequested&&g_griddbReady&&
     strcmp(storeGet(g_global,"Wrapper/CheckUpdatesAtBoot","true"),"false")!=0)
    LauncherUpdate_StartCheck(installedReleaseTag());
  bool forwarderPending=forwarderRequested&&!forwarderMatched;
  const Uint32 forwarderDeadline=forwarderPending?SDL_GetTicks()+10000:0;
  std::vector<std::string> pendingMountedSources;
  bool keepLibrarySelection=false;
  auto changeLibraryTab=[&](int tab){
    selectLibraryTab(tab,sel);
    keepLibrarySelection=true; top=0;
  };
  auto rescanLibrary=[&](){
    ++sourceConfigVersion;storageRefreshPending=true;
    startGameScan(localGameSources(gamePaths),true);
    sel=top=0;keepLibrarySelection=false;
  };
  auto openLibrarySettings=[&](){
    const std::vector<std::string> oldPaths=gamePaths;
    const std::string selected=visibleGame(sel)?visibleGame(sel)->key:std::string{};
    g_active=&g_global; runSettingsRoot(nullptr);
    storeSave(g_global,LAUNCHER_INI);
    gamePaths=loadGameSources();
    if(gamePaths!=oldPaths||g_rescanAfterSettings){
      hasUsbSource=hasConfiguredUsbSource(gamePaths)||hasConfiguredUsbBinding();
      storageMountPending=true;
      rescanLibrary();g_rescanAfterSettings=false;
    } else sel=visibleIndexForKey(selected);
    g_recentPosition=-1; beginScreenFx();
  };
  auto activateLibraryEmpty=[&](){
    if(g_libraryView==LibraryView::All) openLibrarySettings();
    else changeLibraryTab(1);
    g_libraryHeaderFocused=false; g_recentPosition=-1; beginScreenFx();
  };
  auto activateLibraryHeader=[&](int item){
    if(item<libraryTabCount()){
      changeLibraryTab(item); g_libraryHeaderFocused=false;
    } else openLibrarySettings();
  };
  auto pageLibrary=[&](int direction){
    const GLay layout=gridLayout();
    const int count=(int)g_visibleGames.size();
    if(g_libraryView==LibraryView::Recent&&count) sel=std::clamp(sel+direction*layout.cols,0,count-1);
    else sel=gridPage(sel,direction,layout.cols,layout.rows,count);
    keepLibrarySelection=true; g_libraryHeaderFocused=false;
  };

  while(running && beginUiFrame()){
    if(g_libraryScan){
      std::string selectedKey;
      if(keepLibrarySelection&&sel>=0&&sel<(int)g_visibleGames.size())
        selectedKey=g_visibleGames[sel]->key;
      pumpGameScan();
      sel=0;
      if(!selectedKey.empty())
        for(int index=0;index<(int)g_visibleGames.size();index++)
          if(g_visibleGames[index]->key==selectedKey){ sel=index; break; }
    }
    if(!g_libraryScan&&!pendingMountedSources.empty()){
      startGameScan(std::move(pendingMountedSources),false);
      pendingMountedSources.clear();
    }
    if(sel>=(int)g_visibleGames.size()) sel=std::max(0,(int)g_visibleGames.size()-1);
    if(storageInitDone.load()&&!storageIntegrated){
      if(storageInitWorker.joinable()) storageInitWorker.join();
      storageIntegrated=true;
      if(storageRequestVersion!=sourceConfigVersion||
         storageResolvedSnapshot.generation!=SwitchStorage::UsbStatusGeneration()){
        storageRefreshPending=true;
      } else {
        const std::string selected=visibleGame(sel)?visibleGame(sel)->key:std::string{};
        const auto &nextSnapshot=storageResolvedSnapshot;
        std::unordered_map<std::string,std::string> previousPaths,nextPaths;
        for(const auto &location:usbSnapshot.locations) previousPaths[location.id]=pathIdentity(location.path);
        for(const auto &location:nextSnapshot.locations) nextPaths[location.id]=pathIdentity(location.path);
        std::unordered_set<std::string> invalidated,needsScan;
        for(const auto &entry:previousPaths){
          const auto current=nextPaths.find(entry.first);
          if(current==nextPaths.end()||current->second!=entry.second) invalidated.insert(entry.first);
          if(current!=nextPaths.end()&&current->second!=entry.second) needsScan.insert(entry.first);
        }
        for(const auto &entry:nextPaths) if(!previousPaths.count(entry.first)) needsScan.insert(entry.first);
        if(!storageFirstPass&&nextSnapshot.generation!=usbSnapshot.generation&&
           invalidated.empty()&&needsScan.empty()){
          for(const std::string &source:gamePaths){
            const std::string id=usbStableIdForPath(source,nextSnapshot.locations);
            if(!id.empty()){ invalidated.insert(id); needsScan.insert(id); }
          }
        }
        removeGamesFromUsbDevices(invalidated);
        const std::vector<std::string> previousSources=gamePaths;
        gamePaths.clear();
        for(const auto &record:storageRebound)
          if(!record.runtimePath.empty()) gamePaths.push_back(record.runtimePath);
        if(storageReboundPersist){
          persistGameSourceRecords(storageRebound);
          storeSave(g_global,LAUNCHER_INI);
        }
        cacheGameSourceRecords(storageRebound,nextSnapshot.generation);
        const bool rescanMounted=storageFirstPass||storageAppliedVersion!=sourceConfigVersion;
        std::unordered_set<std::string> queuedSources;
        for(const std::string &source:pendingMountedSources) queuedSources.insert(pathIdentity(source));
        for(const std::string &source:gamePaths){
          if(isUnresolvedUsbSource(source)||
             (!isUsbStoragePath(source)&&source.rfind("cemusmb_",0)!=0)) continue;
          const std::string identity=pathIdentity(source);
          const bool appeared=std::none_of(previousSources.begin(),previousSources.end(),
            [&](const std::string &previous){ return pathIdentity(previous)==identity; });
          const std::string id=usbStableIdForPath(source,nextSnapshot.locations);
          if(!rescanMounted&&!appeared&&(id.empty()||!needsScan.count(id))) continue;
          if(queuedSources.insert(identity).second) pendingMountedSources.push_back(source);
        }
        usbSnapshot=std::move(storageResolvedSnapshot);
        usbGeneration=usbSnapshot.generation;
        storageFirstPass=false;storageAppliedVersion=sourceConfigVersion;
        storageRefreshPending=false;
        sel=visibleIndexForKey(selected);top=0;
        if(forwarderPending) if(Game *game=findGameByKey(forwarderKey)){
          selectGame(*game);
          forwarderPending=false;
        }
      }
      storageRebound.clear();
    }
    if(hasUsbSource){
      const uint64_t generation=SwitchStorage::UsbStatusGeneration();
      if(generation!=usbGeneration){
        usbGeneration=generation;
        storageRefreshPending=true;
      }
    }
    if(storageIntegrated&&storageRefreshPending) startStorageRefresh();
    if(forwarderPending&&SDL_TICKS_PASSED(SDL_GetTicks(),forwarderDeadline)){
      forwarderPending=false;
      modalMessageStatic("Game not found",{"The shortcut's game is not in the current library.","","Reconnect its storage or update the game folders."});
      running=false;
    }
    if(!running) break;
    if(forwarderPending){
      SDL_Event event;
      while(pollUiEvent(event)){
        pumpStick(event);
        if(event.type==SDL_CONTROLLERBUTTONDOWN&&event.cbutton.button==BTN_CANCEL){
          running=false;
          break;
        }
      }
      if(!running) break;
      if(Game *game=findGameByKey(forwarderKey)){
        selectGame(*game);
        forwarderPending=false;
        break;
      }
      renderUsbForwarderWait();
    waitForNextUiFrame();
      continue;
    }
    SDL_Event event; navRepeat();
    while(running&&pollUiEvent(event)){
      pumpStick(event);
      const GLay layout=gridLayout();
      const int cols=layout.cols;
      rows=layout.rows;
      const int count=(int)g_visibleGames.size();
      sel=count?std::clamp(sel,0,count-1):0;
      top=count?(sel/(cols*rows))*rows:0;
      if(event.type==SDL_CONTROLLERAXISMOTION&&
         (event.caxis.axis==SDL_CONTROLLER_AXIS_TRIGGERLEFT||event.caxis.axis==SDL_CONTROLLER_AXIS_TRIGGERRIGHT)){
        const size_t trigger=event.caxis.axis==SDL_CONTROLLER_AXIS_TRIGGERLEFT?0:1;
        const bool pressed=event.caxis.value>18000;
        if(pressed&&!g_libraryTriggers[trigger]) pageLibrary(trigger?1:-1);
        g_libraryTriggers[trigger]=pressed;
        continue;
      }
      int tx=0,ty=0;
      const TouchKind touch=touchFeed(event,&tx,&ty);
      if(touch==TOUCH_SWIPE_L||touch==TOUCH_SWIPE_R){
        const int direction=touch==TOUCH_SWIPE_L?1:-1;
        if(g_touch.y0<88) changeLibraryTab((libraryTabIndex()+direction+libraryTabCount())%libraryTabCount());
        else pageLibrary(direction);
        continue;
      }
      if(touch==TOUCH_SCROLL_UP||touch==TOUCH_SCROLL_DOWN){
        const int step=(touch==TOUCH_SCROLL_UP?1:-1)*g_touchScrollSteps;
        if(count) sel=std::clamp(sel+step*(g_libraryView==LibraryView::Recent?1:cols),0,count-1);
        g_libraryHeaderFocused=false; keepLibrarySelection=true;
        continue;
      }
      if(touch==TOUCH_TAP){
        keepLibrarySelection=true;
        const int header=libraryHeaderHit(tx,ty);
        if(header>=0){ activateLibraryHeader(header); continue; }
        {
          SDL_Point point{tx,ty};
          const SDL_Rect emptyAction=libraryEmptyActionRect();
          if(!count&&SDL_PointInRect(&point,&emptyAction)){ activateLibraryEmpty(); continue; }
          const int hit=gridHitTest(tx,ty,top,sel);
          if(hit>=0){
            if(hit==sel&&!g_libraryHeaderFocused) selectGame(*visibleGame(sel));
            else sel=hit;
            g_libraryHeaderFocused=false;
          }
        }
        continue;
      }
      if(event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      keepLibrarySelection=true;
      switch(event.cbutton.button){
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: {
          const int direction=event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_LEFT?-1:1;
          if(g_libraryHeaderFocused)
            g_libraryHeaderSelection=(g_libraryHeaderSelection+direction+libraryTabCount()+1)%(libraryTabCount()+1);
          else if(g_libraryView==LibraryView::Recent&&count) sel=std::clamp(sel+direction,0,count-1);
          else sel=gridNav(sel,direction,0,cols,rows,count);
          break;
        }
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
          if(!g_libraryHeaderFocused&&(!count||g_libraryView==LibraryView::Recent||sel%(cols*rows)<cols)){
            g_libraryHeaderFocused=true; g_libraryHeaderSelection=libraryTabIndex();
          } else if(!g_libraryHeaderFocused) sel=gridNav(sel,0,-1,cols,rows,count);
          break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
          if(g_libraryHeaderFocused) g_libraryHeaderFocused=false;
          else sel=gridNav(sel,0,1,cols,rows,count);
          break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
          changeLibraryTab((libraryTabIndex()+libraryTabCount()-1)%libraryTabCount());
          break;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
          changeLibraryTab((libraryTabIndex()+1)%libraryTabCount());
          break;
        case BTN_CONFIRM:
          if(g_libraryHeaderFocused) activateLibraryHeader(g_libraryHeaderSelection);
          else if(count) selectGame(*visibleGame(sel));
          else activateLibraryEmpty();
          break;
        case BTN_Y:
          if(!g_libraryHeaderFocused) if(Game *game=visibleGame(sel)){
            const std::string selectedKey=game->key;
            const int result=perGameMenu(*game);
            if(result==1) selectGame(*game);
            else if(result==2) rescanLibrary();
            else { applySort(); sel=visibleIndexForKey(selectedKey); }
            g_recentPosition=-1;
            beginScreenFx();
          }
          break;
        case BTN_CANCEL:
          if(g_libraryHeaderFocused) g_libraryHeaderFocused=false;
          else requestExit();
          break;
      }
    }
    const GLay layout=gridLayout();
    const int count=(int)g_visibleGames.size();
    sel=count?std::clamp(sel,0,count-1):0;
    top=count?(sel/(layout.cols*layout.rows))*layout.rows:0;
    pollUpdateNotification();
    g_libraryStoragePending=!storageIntegrated||storageRefreshPending||!pendingMountedSources.empty();
    renderGrid(sel,top);
    waitForNextUiFrame(g_libraryScan!=nullptr||g_libraryStoragePending);
  }
  if(userExit){
    auto renderClosing=[&](){
      clearUiBackground();
      drawHeaderStatic("Closing Cemu...",nullptr);
      { const int waitW=std::min(940,SW-64),waitH=140;
        glassPanel((SW-waitW)/2,(SH-waitH)/2,waitW,waitH); }
      drawTextC(g_font,SW/2,SH/2-fontHeight(g_font)/2,
                LauncherLocalization::Translate("Finishing background operations safely.").data(),COL_DIM);
      SDL_RenderPresent(g_ren);
    };
    renderClosing();
    SwitchStorage::SetUsbStatusCallback(nullptr,nullptr);
    storageInitCancel=true;
    if(g_libraryScan) g_libraryScan->cancel=true;
    Uint32 nextFrame=SDL_GetTicks()+100;
    while((g_libraryScan&&!g_libraryScan->done.load())||!storageInitDone.load()){
      SDL_PumpEvents();
      if(SDL_TICKS_PASSED(SDL_GetTicks(),nextFrame)){ renderClosing(); nextFrame=SDL_GetTicks()+100; }
      if(!appletMainLoop()) break;
      svcSleepThread(16000000);
    }
  }
  storageInitCancel=true;
  if(storageInitWorker.joinable()) storageInitWorker.join();
  stopGameScan();

  g_active=&g_global;
  if(launch) commitAll();
  storeSave(g_global, LAUNCHER_INI);
  storeSave(g_recent, RECENT_INI);

  bool willChain = false;
  if(launch && envHasNextLoad()){
    std::vector<CemuKV> eff = buildEffectiveSettings(launchKey);
    const char *configuredRenderer=cemuKVGet(eff,"Wrapper/Renderer","vk");
    const std::string renderer=!strcmp(configuredRenderer,"gl") ? "gl" :
                               !strcmp(configuredRenderer,"zink") ? "zink" : "vk";
    const bool haveEmu=ensureEmu();
    appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);

    std::vector<CemuGraphicPack> enabledPacks;
    bool configOk = haveEmu && readEnabledPacks(enabledPacks);
    std::vector<std::string> mountedGamePaths;
    for(const std::string &path:gamePaths)
      if(!isUnresolvedUsbSource(path)) mountedGamePaths.push_back(path);
    if (configOk) configOk = cemu_writeSettingsXml(SETTINGS_XML, eff, mountedGamePaths, enabledPacks);
    if (configOk) configOk = writeInputIni(eff);
    if(configOk && launchTitleId){
      const char *gname = "";
      for(auto &g:g_games) if(g.key==launchKey){ gname=g.title.c_str(); break; }
      configOk = cemu_writeGameProfile(GAMEPROFILES_DIR, launchTitleId, gname, eff);
    }
    if (configOk) {
      bool lsfgPrepared = renderer=="vk" &&
                          !strcmp(cemuKVGet(eff,"Wrapper/LSFGEnabled","false"),"true");
      if (lsfgPrepared && !regularFileExists(LSFG_DLL_FILE)) {
        lsfgPrepared = false;
        modalMessageStatic("LSFG disabled for this launch", {
          "Lossless.dll was not found at",
          "sdmc:/switch/cemu/lsfg/Lossless.dll"
        });
      }
      configOk=writeLaunchHandoff(eff,renderer,launchPath,launchTitleId,lsfgPrepared);
    }
    appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
    if(configOk) willChain=true;
    else {
      remove(LAUNCH_HANDOFF);
      if (!haveEmu) {
        modalMessageStatic("Emulator setup failed", {
          "The embedded Cemu core could not be prepared.",
          "The SD card may be full or write-protected.",
          "Free some space and try launching the game again."
        });
      } else {
        modalMessageStatic("Launch configuration failed", {
          "Cemu configuration could not be updated safely.",
          "The previous settings were preserved.",
          "Check free SD space and file permissions."
        });
      }
    }
  }

  cleanupLauncher();

  if(willChain) envSetNextLoad(EMU_NRO_DST,EMU_NRO_DST);
  return 0;
}
