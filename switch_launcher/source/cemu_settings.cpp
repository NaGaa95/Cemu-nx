#include "cemu_settings.h"
#include "tinyxml2.h"

#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
#include <switch/runtime/devices/fs_dev.h>
}

const char *cemuKVGet(const std::vector<CemuKV> &kv, const char *key, const char *def) {
  for (auto &e : kv)
    if (e.k == key)
      return e.v.c_str();
  return def;
}

namespace {

bool queryPath(const std::string &path, bool &exists) {
  struct stat st{};
  if (stat(path.c_str(), &st) == 0) {
    exists = true;
    return true;
  }
  exists = false;
  return errno == ENOENT;
}

bool recoverAtomicFile(const std::string &path) {
  const std::string tmp = path + ".tmp";
  const std::string old = path + ".old";
  bool currentExists = false, oldExists = false, tmpExists = false;
  if (!queryPath(path, currentExists) || !queryPath(old, oldExists) ||
      !queryPath(tmp, tmpExists))
    return false;
  if (!currentExists && oldExists) {
    if (rename(old.c_str(), path.c_str()) != 0)
      return false;
    fsdevCommitDevice("sdmc");
  }
  if (tmpExists && remove(tmp.c_str()) != 0)
    return false;
  return true;
}

bool replaceAtomic(const std::string &path, const std::string &tmp) {
  const std::string old = path + ".old";
  bool hadCurrent = false, oldExists = false;
  if (!queryPath(path, hadCurrent) || !queryPath(old, oldExists))
    return false;
  if (oldExists && remove(old.c_str()) != 0)
    return false;
  if (hadCurrent && rename(path.c_str(), old.c_str()) != 0)
    return false;
  if (rename(tmp.c_str(), path.c_str()) != 0) {
    if (hadCurrent) {
      rename(old.c_str(), path.c_str());
      fsdevCommitDevice("sdmc");
    }
    return false;
  }
  fsdevCommitDevice("sdmc");
  if (hadCurrent && remove(old.c_str()) == 0)
    fsdevCommitDevice("sdmc");
  return true;
}

bool flushAndClose(FILE *file) {
  bool ok = fflush(file) == 0;
  if (ok && fsync(fileno(file)) != 0)
    ok = false;
  if (fclose(file) != 0)
    ok = false;
  return ok;
}

tinyxml2::XMLElement *upsertElement(tinyxml2::XMLDocument &doc,
                                    tinyxml2::XMLElement *parent,
                                    const char *name) {
  auto *element = parent->FirstChildElement(name);
  if (!element) {
    element = doc.NewElement(name);
    parent->InsertEndChild(element);
    return element;
  }
  for (auto *duplicate = element->NextSiblingElement(name); duplicate;) {
    auto *next = duplicate->NextSiblingElement(name);
    parent->DeleteChild(duplicate);
    duplicate = next;
  }
  return element;
}

void removeChildElements(tinyxml2::XMLElement *parent, const char *name) {
  for (auto *element = parent->FirstChildElement(name); element;) {
    auto *next = element->NextSiblingElement(name);
    parent->DeleteChild(element);
    element = next;
  }
}

struct IniLine {
  std::string text;
  std::string ending;
};

std::string trimHorizontal(const std::string &value) {
  const size_t first = value.find_first_not_of(" \t");
  if (first == std::string::npos)
    return {};
  const size_t last = value.find_last_not_of(" \t");
  return value.substr(first, last - first + 1);
}

bool equalsIgnoreCase(const std::string &left, const char *right) {
  const size_t length = strlen(right);
  if (left.size() != length)
    return false;
  for (size_t i = 0; i < length; ++i) {
    if (std::tolower(static_cast<unsigned char>(left[i])) !=
        std::tolower(static_cast<unsigned char>(right[i])))
      return false;
  }
  return true;
}

bool parseSection(const std::string &line, std::string &section) {
  const std::string value = trimHorizontal(line);
  if (value.size() < 3 || value.front() != '[')
    return false;
  const size_t close = value.find(']');
  if (close == std::string::npos)
    return false;
  const std::string suffix = trimHorizontal(value.substr(close + 1));
  if (!suffix.empty() && suffix.front() != ';' && suffix.front() != '#')
    return false;
  section = trimHorizontal(value.substr(1, close - 1));
  return !section.empty();
}

bool parseAssignment(const std::string &line, std::string &key, size_t &valueStart) {
  const std::string stripped = trimHorizontal(line);
  if (stripped.empty() || stripped.front() == ';' || stripped.front() == '#')
    return false;
  const size_t equal = line.find('=');
  if (equal == std::string::npos)
    return false;
  key = trimHorizontal(line.substr(0, equal));
  if (key.empty())
    return false;
  valueStart = equal + 1;
  while (valueStart < line.size() && (line[valueStart] == ' ' || line[valueStart] == '\t'))
    ++valueStart;
  return true;
}

void replaceAssignmentValue(IniLine &line, size_t valueStart, const std::string &value) {
  size_t valueEnd = line.text.size();
  bool quoted = false;
  for (size_t i = valueStart; i < line.text.size(); ++i) {
    if (line.text[i] == '"')
      quoted = !quoted;
    else if (!quoted && (line.text[i] == ';' || line.text[i] == '#')) {
      valueEnd = i;
      break;
    }
  }
  while (valueEnd > valueStart && (line.text[valueEnd - 1] == ' ' || line.text[valueEnd - 1] == '\t'))
    --valueEnd;
  line.text = line.text.substr(0, valueStart) + value + line.text.substr(valueEnd);
}

std::vector<IniLine> splitIniLines(const std::string &text) {
  std::vector<IniLine> lines;
  size_t offset = 0;
  while (offset < text.size()) {
    const size_t newline = text.find('\n', offset);
    if (newline == std::string::npos) {
      lines.push_back({text.substr(offset), {}});
      break;
    }
    if (newline > offset && text[newline - 1] == '\r')
      lines.push_back({text.substr(offset, newline - offset - 1), "\r\n"});
    else
      lines.push_back({text.substr(offset, newline - offset), "\n"});
    offset = newline + 1;
  }
  return lines;
}

std::string preferredLineEnding(const std::vector<IniLine> &lines) {
  for (const auto &line : lines)
    if (!line.ending.empty())
      return line.ending;
  return "\n";
}

void appendIniLine(std::vector<IniLine> &lines, const std::string &text,
                   const std::string &ending) {
  if (!lines.empty() && lines.back().ending.empty())
    lines.back().ending = ending;
  lines.push_back({text, ending});
}

void insertIniLine(std::vector<IniLine> &lines, size_t position,
                   const std::string &text, const std::string &ending) {
  if (position >= lines.size()) {
    appendIniLine(lines, text, ending);
    return;
  }
  lines.insert(lines.begin() + position, {text, ending});
}

void setIniValue(std::vector<IniLine> &lines, const char *sectionName,
                 const char *keyName, const std::string &value) {
  bool active = false;
  bool foundSection = false;
  bool foundKey = false;
  size_t insertAt = lines.size();
  for (size_t i = 0; i < lines.size(); ++i) {
    std::string section;
    if (parseSection(lines[i].text, section)) {
      active = equalsIgnoreCase(section, sectionName);
      if (active) {
        foundSection = true;
        insertAt = i + 1;
      }
      continue;
    }
    if (!active)
      continue;
    insertAt = i + 1;
    std::string key;
    size_t valueStart = 0;
    if (parseAssignment(lines[i].text, key, valueStart) && equalsIgnoreCase(key, keyName)) {
      replaceAssignmentValue(lines[i], valueStart, value);
      foundKey = true;
    }
  }
  if (foundKey)
    return;

  const std::string ending = preferredLineEnding(lines);
  const std::string assignment = std::string(keyName) + " = " + value;
  if (foundSection) {
    insertIniLine(lines, insertAt, assignment, ending);
    return;
  }
  if (!lines.empty() && !trimHorizontal(lines.back().text).empty())
    appendIniLine(lines, {}, ending);
  appendIniLine(lines, std::string("[") + sectionName + "]", ending);
  appendIniLine(lines, assignment, ending);
}

bool readTextFile(const std::string &path, std::string &text, size_t limit) {
  FILE *file = fopen(path.c_str(), "rb");
  if (!file)
    return false;
  char buffer[4096];
  bool ok = true;
  size_t count = 0;
  while ((count = fread(buffer, 1, sizeof(buffer), file)) > 0) {
    if (text.size() > limit || count > limit - text.size()) {
      ok = false;
      break;
    }
    text.append(buffer, count);
  }
  if (ferror(file) || fclose(file) != 0)
    ok = false;
  return ok;
}

std::string serializeIni(const std::vector<IniLine> &lines) {
  std::string text;
  for (const auto &line : lines) {
    text += line.text;
    text += line.ending;
  }
  return text;
}

} // namespace

bool cemu_readAccountService(const char *path, uint32_t persistentId,
                             int &service) {
  service = 0;
  if (!path || !*path)
    return false;
  bool exists = false;
  if (!queryPath(path, exists))
    return false;
  if (!exists)
    return true;
  tinyxml2::XMLDocument doc;
  if (doc.LoadFile(path) != tinyxml2::XML_SUCCESS)
    return false;
  auto *content = doc.FirstChildElement("content");
  auto *accountService = content ? content->FirstChildElement("AccountService") : nullptr;
  if (!accountService)
    return true;
  for (auto *entry = accountService->FirstChildElement("SelectedService"); entry;
       entry = entry->NextSiblingElement("SelectedService")) {
    if (entry->UnsignedAttribute("PersistentId", 0) != persistentId)
      continue;
    const int selected = entry->IntAttribute("Service", 0);
    service = selected >= 0 && selected <= 3 ? selected : 0;
    break;
  }
  return true;
}

bool cemu_readAccountSelection(const char *path, uint32_t &persistentId,
                               int &service) {
  persistentId = 0x80000001;
  service = 0;
  if (!path || !*path)
    return false;
  bool exists = false;
  if (!queryPath(path, exists))
    return false;
  if (!exists)
    return true;
  tinyxml2::XMLDocument doc;
  if (doc.LoadFile(path) != tinyxml2::XML_SUCCESS)
    return false;
  auto *content = doc.FirstChildElement("content");
  auto *account = content ? content->FirstChildElement("Account") : nullptr;
  auto *id = account ? account->FirstChildElement("PersistentId") : nullptr;
  if (id)
    persistentId = id->UnsignedText(persistentId);
  return cemu_readAccountService(path, persistentId, service);
}

bool cemu_writeAccountSelection(const char *path, uint32_t persistentId,
                                int service) {
  using namespace tinyxml2;
  if (!path || !*path || persistentId < 0x80000001 || service < 0 || service > 3)
    return false;
  const std::string target = path;
  if (!recoverAtomicFile(target))
    return false;

  bool existed = false;
  if (!queryPath(target, existed))
    return false;
  XMLDocument doc;
  if (existed && doc.LoadFile(path) != XML_SUCCESS)
    return false;
  if (!existed)
    doc.InsertEndChild(doc.NewDeclaration());
  XMLElement *content = doc.FirstChildElement("content");
  if (!content) {
    if (doc.RootElement())
      return false;
    content = doc.NewElement("content");
    doc.InsertEndChild(content);
  }

  XMLElement *account = upsertElement(doc, content, "Account");
  upsertElement(doc, account, "PersistentId")->SetText(persistentId);
  XMLElement *services = upsertElement(doc, content, "AccountService");
  XMLElement *selected = nullptr;
  for (auto *entry = services->FirstChildElement("SelectedService"); entry;) {
    auto *next = entry->NextSiblingElement("SelectedService");
    if (entry->UnsignedAttribute("PersistentId", 0) == persistentId) {
      if (!selected)
        selected = entry;
      else
        services->DeleteChild(entry);
    }
    entry = next;
  }
  if (!selected) {
    selected = doc.NewElement("SelectedService");
    services->InsertEndChild(selected);
  }
  selected->SetAttribute("PersistentId", persistentId);
  selected->SetAttribute("Service", service);

  const std::string tmp = target + ".tmp";
  FILE *file = fopen(tmp.c_str(), "wb");
  if (!file)
    return false;
  bool ok = doc.SaveFile(file, false) == XML_SUCCESS;
  if (!flushAndClose(file))
    ok = false;
  if (!ok) {
    remove(tmp.c_str());
    return false;
  }
  if (!replaceAtomic(target, tmp)) {
    remove(tmp.c_str());
    return false;
  }
  return true;
}

bool cemu_writeSettingsXml(const char *path, const std::vector<CemuKV> &s,
                           const std::vector<std::string> &gamePaths,
                           const std::vector<CemuGraphicPack> &enabledPacks) {
  using namespace tinyxml2;
  if (!path || !*path)
    return false;

  const std::string target = path;
  if (!recoverAtomicFile(target))
    return false;

  XMLDocument doc;
  bool existed = false;
  if (!queryPath(target, existed))
    return false;
  if (existed && doc.LoadFile(path) != XML_SUCCESS)
    return false;
  if (!existed)
    doc.InsertEndChild(doc.NewDeclaration());

  XMLElement *content = doc.FirstChildElement("content");
  if (!content) {
    if (doc.RootElement())
      return false;
    content = doc.NewElement("content");
    doc.InsertEndChild(content);
  }

  auto gi = [&](const char *k, long def) { const char *v = cemuKVGet(s, k, nullptr); return v ? strtol(v, nullptr, 10) : def; };
  auto gb = [&](const char *k, bool def) { const char *v = cemuKVGet(s, k, nullptr); bool b = v ? (!strcmp(v, "true") || !strcmp(v, "1")) : def; return b ? "true" : "false"; };
  auto ei = [&](XMLElement *p, const char *n, long v) { upsertElement(doc, p, n)->SetText((int)v); };
  auto es = [&](XMLElement *p, const char *n, const char *v) { upsertElement(doc, p, n)->SetText(v); };

  ei(content, "console_language", gi("console_language", 1));

  XMLElement *gp = upsertElement(doc, content, "GamePaths");
  removeChildElements(gp, "Entry");
  for (auto &p : gamePaths) { XMLElement *e = doc.NewElement("Entry"); e->SetText(p.c_str()); gp->InsertEndChild(e); }

  XMLElement *gpk = upsertElement(doc, content, "GraphicPack");
  removeChildElements(gpk, "Entry");
  for (auto &pk : enabledPacks) {
    XMLElement *entry = doc.NewElement("Entry");
    entry->SetAttribute("filename", pk.rulesRel.c_str());
    for (auto &pr : pk.presets) {
      XMLElement *preset = doc.NewElement("Preset");
      if (!pr.category.empty()) { XMLElement *cat = doc.NewElement("category"); cat->SetText(pr.category.c_str()); preset->InsertEndChild(cat); }
      XMLElement *nm = doc.NewElement("preset"); nm->SetText(pr.preset.c_str()); preset->InsertEndChild(nm);
      entry->InsertEndChild(preset);
    }
    gpk->InsertEndChild(entry);
  }
  const char *renderer = cemuKVGet(s, "Wrapper/Renderer", "vk");
  const bool nativeVulkan = !strcmp(renderer, "vk");
  const bool nativeDeko = !strcmp(renderer, "deko3d");
  XMLElement *g = upsertElement(doc, content, "Graphic");
  ei(g, "api", nativeDeko ? 3 : nativeVulkan ? 1 : 0);
  ei(g, "VSync", gi("VSync", 0));
  es(g, "vkAccurateBarriers", gb("vkAccurateBarriers", true));
  es(g, "vkHostMemoryImport", nativeVulkan ? "true" : "false");
  ei(g, "UpscaleFilter", gi("UpscaleFilter", 1));
  ei(g, "DownscaleFilter", gi("DownscaleFilter", 0));
  ei(g, "FullscreenScaling", gi("FullscreenScaling", 0));
  es(g, "AsyncCompile", gb("AsyncCompile", true));
  es(g, "H264HardwareDecode", gb("H264HardwareDecode", true));
  XMLElement *ov = upsertElement(doc, g, "Overlay");
  ei(ov, "Position", gi("OverlayPosition", 0));
  es(ov, "FPS", gb("OverlayFPS", false));
  // Process metrics are unavailable through libnx.
  es(ov, "CPUUsage", "false");
  es(ov, "RAMUsage", "false");
  es(ov, "VRAMUsage", gb("OverlayVRAM", false));
  es(ov, "ShaderCompiling", gb("NotifShaderCompile", true));
  XMLElement *a = upsertElement(doc, content, "Audio");
  ei(a, "delay", gi("AudioDelay", 2));
  ei(a, "TVVolume", gi("TVVolume", 50));
  ei(a, "PadChannels", 1);
  ei(a, "PadVolume", gi("PadVolume", 50));
  es(a, "PadDevice", !strcmp(gb("PadAudio", true), "true") ? "default" : "");
  XMLElement *usb = upsertElement(doc, content, "EmulatedUsbDevices");
  es(usb, "EmulateSkylanderPortal", gb("UsbSkylanders", false));
  es(usb, "EmulateInfinityBase", gb("UsbInfinity", false));
  es(usb, "EmulateDimensionsToypad", gb("UsbDimensions", false));

  const std::string tmp = target + ".tmp";
  FILE *file = fopen(tmp.c_str(), "wb");
  if (!file)
    return false;
  bool ok = doc.SaveFile(file, false) == XML_SUCCESS;
  if (!flushAndClose(file))
    ok = false;
  if (!ok) {
    remove(tmp.c_str());
    return false;
  }
  if (!replaceAtomic(target, tmp)) {
    remove(tmp.c_str());
    return false;
  }
  return true;
}

bool cemu_writeGameProfile(const char *dir, uint64_t titleId, const char *gameName,
                           const std::vector<CemuKV> &s) {
  if (titleId == 0)
    return true;
  if (!dir || !*dir)
    return false;
  char fn[512];
  const int length = snprintf(fn, sizeof(fn), "%s/%016llx.ini", dir, (unsigned long long)titleId);
  if (length < 0 || static_cast<size_t>(length) >= sizeof(fn))
    return false;
  const std::string target = fn;
  if (!recoverAtomicFile(target))
    return false;

  const char *cpu = cemuKVGet(s, "cpuMode", "");
  const char *tq = cemuKVGet(s, "threadQuantum", "");
  if (strpbrk(cpu, "\r\n") || strpbrk(tq, "\r\n"))
    return false;

  bool existed = false;
  if (!queryPath(target, existed))
    return false;
  std::string source;
  if (existed && !readTextFile(target, source, 1U << 20))
    return false;
  std::vector<IniLine> lines = splitIniLines(source);
  if (!existed && gameName && *gameName) {
    std::string name = gameName;
    for (char &c : name)
      if (c == '\r' || c == '\n') c = ' ';
    appendIniLine(lines, "# " + name, "\n");
  }

  setIniValue(lines, "General", "startWithPadView", "false");
  if (*cpu)
    setIniValue(lines, "CPU", "cpuMode", cpu);
  if (*tq)
    setIniValue(lines, "CPU", "threadQuantum", tq);
  const char *renderer = cemuKVGet(s, "Wrapper/Renderer", "vk");
  const bool nativeVulkan = !strcmp(renderer, "vk");
  const bool nativeDeko = !strcmp(renderer, "deko3d");
  setIniValue(lines, "Graphics", "graphics_api", nativeDeko ? "3" : nativeVulkan ? "1" : "0");

  const std::string output = serializeIni(lines);
  const std::string tmp = target + ".tmp";
  FILE *f = fopen(tmp.c_str(), "wb");
  if (!f)
    return false;
  bool ok = fwrite(output.data(), 1, output.size(), f) == output.size();
  if (!flushAndClose(f))
    ok = false;
  if (!ok) {
    remove(tmp.c_str());
    return false;
  }
  if (!replaceAtomic(target, tmp)) {
    remove(tmp.c_str());
    return false;
  }
  return true;
}
