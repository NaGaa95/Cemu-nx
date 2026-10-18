#include "shader_packs.h"

#include <switch.h>
#include <curl/curl.h>
#include <rapidjson/reader.h>
#include <rapidjson/memorystream.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <set>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace ShaderPacks {
namespace {

constexpr char repository[] = "https://raw.githubusercontent.com/chriztr/cemu_shader_and_pipeline_caches/main/";
constexpr char fullArchive[] = "https://github.com/chriztr/cemu_shader_and_pipeline_caches/releases/latest/download/shaders.zip";
constexpr uint64_t maxArchive = 1024ULL * 1024 * 1024;
constexpr uint64_t maxCache = 256ULL * 1024 * 1024;
constexpr uint64_t maxExtracted = 2ULL * 1024 * 1024 * 1024;
using File = std::unique_ptr<FILE, decltype(&std::fclose)>;

uint16_t Read16(const uint8_t *p) { return p[0] | (uint16_t(p[1]) << 8); }
uint32_t Read32(const uint8_t *p) { return Read16(p) | (uint32_t(Read16(p + 2)) << 16); }
uint64_t Read64(const uint8_t *p) { return Read32(p) | (uint64_t(Read32(p + 4)) << 32); }

bool TitleId(std::string_view value, uint64_t &id) {
  if (value.size() != 16) return false;
  id = 0;
  for (char c : value) {
    unsigned digit = c >= '0' && c <= '9' ? c - '0' :
                     c >= 'a' && c <= 'f' ? c - 'a' + 10 : 16;
    if (digit == 16) return false;
    id = id * 16 + digit;
  }
  return id != 0;
}

bool CacheName(std::string_view name, uint64_t &id, bool &pipeline) {
  if (name.size() < 16 || !TitleId(name.substr(0, 16), id)) return false;
  pipeline = name.substr(16) == "_vkpipeline.bin";
  return pipeline || name.substr(16) == "_shaders.bin";
}

bool ReadAt(FILE *file, uint64_t offset, void *buffer, size_t size) {
  return offset <= uint64_t(std::numeric_limits<off_t>::max()) &&
         fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0 &&
         fread(buffer, 1, size, file) == size;
}

bool ValidCache(const std::string &path, uint64_t title, bool pipeline) {
  struct stat st{};
  if (lstat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 128) return false;
  File file(fopen(path.c_str(), "rb"), &std::fclose);
  std::array<uint8_t, 28> header{};
  if (!file || !ReadAt(file.get(), 0, header.data(), header.size())) return false;
  const uint32_t magic = Read32(header.data());
  const uint32_t version = Read32(header.data() + 4);
  const uint32_t expected = uint32_t(title >> 32) + uint32_t(title) * 3 + 1 +
                            (pipeline ? 0 : 0xe97af1adu);
  const uint64_t data = Read64(header.data() + 8);
  const uint64_t table = Read64(header.data() + 16);
  const uint32_t tableSize = Read32(header.data() + 24);
  const uint64_t size = uint64_t(st.st_size);
  return (magic == 0x8371b695 || magic == 0x8371b696) &&
         (version == expected || (!pipeline && version == 2)) &&
         data >= header.size() && data <= size && table <= size - data &&
         tableSize >= 32 && tableSize % 32 == 0 && tableSize <= size - data - table;
}

bool MakeDirectories(const std::string &path) {
  if (path.empty()) { errno = EINVAL; return false; }
  size_t start = 1;
  if (const size_t mount = path.find(":/"); mount != std::string::npos)
    start = mount + 2;
  for (size_t end = start; end <= path.size(); ++end) {
    if (end != path.size() && path[end] != '/') continue;
    const auto part = path.substr(0, end);
    struct stat st{};
    if (stat(part.c_str(), &st) == 0) {
      if (S_ISDIR(st.st_mode)) continue;
      errno = ENOTDIR;
      return false;
    }
    if (errno != ENOENT) return false;
    if (mkdir(part.c_str(), 0777) == 0) continue;
    if (errno != EEXIST || stat(part.c_str(), &st) != 0) return false;
    if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return false; }
  }
  return true;
}

struct Staging {
  std::string directory;
  std::vector<std::string> paths;
  ~Staging() {
    for (const auto &path : paths) remove(path.c_str());
    if (!directory.empty()) rmdir(directory.c_str());
  }
  bool Create(const std::string &cacheDir) {
    if (!MakeDirectories(cacheDir)) return false;
    const auto tick = static_cast<unsigned long long>(armGetSystemTick());
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
      char name[64];
      snprintf(name, sizeof(name), "/.shader-download-%016llx-%u", tick, attempt);
      const std::string path = cacheDir + name;
      if (mkdir(path.c_str(), 0777) == 0) {
        directory = path;
        return true;
      }
      if (errno != EEXIST) return false;
    }
    return false;
  }
  std::string Path(const std::string &name) {
    paths.push_back(directory + "/" + name);
    return paths.back();
  }
  void Keep(const std::string &path) {
    paths.erase(std::remove(paths.begin(), paths.end(), path), paths.end());
  }
};

struct Transfer {
  FILE *file = nullptr;
  std::string *body = nullptr;
  uint64_t written = 0;
  uint64_t limit = 0;
  const std::atomic_bool *cancel = nullptr;
  Progress *progress = nullptr;
};

size_t WriteDownload(void *data, size_t size, size_t count, void *context) {
  auto &transfer = *static_cast<Transfer *>(context);
  if (transfer.cancel->load() || (count && size > SIZE_MAX / count)) return 0;
  const size_t bytes = size * count;
  if (bytes > transfer.limit - transfer.written) return 0;
  if (transfer.file) {
    if (fwrite(data, 1, bytes, transfer.file) != bytes) return 0;
  } else transfer.body->append(static_cast<const char *>(data), bytes);
  transfer.written += bytes;
  return bytes;
}

int DownloadProgress(void *context, curl_off_t total, curl_off_t current, curl_off_t, curl_off_t) {
  auto &transfer = *static_cast<Transfer *>(context);
  if (transfer.progress) {
    transfer.progress->total = total > 0 ? uint64_t(total) : 0;
    transfer.progress->current = current > 0 ? uint64_t(current) : 0;
  }
  return transfer.cancel->load() ? 1 : 0;
}

bool Download(const std::string &url, Transfer &transfer, std::string &error) {
  if (transfer.cancel->load()) return false;
  CURL *curl = curl_easy_init();
  if (!curl) { error = "Could not start the download."; return false; }
  char message[CURL_ERROR_SIZE]{};
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
#if LIBCURL_VERSION_NUM >= 0x075500
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
#endif
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "Cemu-Launcher/" CEMU_SWITCH_VERSION);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 1800L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
  curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(transfer.limit));
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteDownload);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &transfer);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, DownloadProgress);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &transfer);
  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, message);
  const CURLcode result = curl_easy_perform(curl);
  long response = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response);
  curl_easy_cleanup(curl);
  if (transfer.cancel->load()) return false;
  if (result != CURLE_OK || response < 200 || response >= 300) {
    error = message[0] ? message : "Could not download the shader pack.";
    return false;
  }
  return true;
}

bool DownloadFile(const std::string &url, const std::string &path, uint64_t limit,
                  Progress &progress, const std::atomic_bool &cancel, std::string &error) {
  File file(fopen(path.c_str(), "wb"), &std::fclose);
  if (!file) { error = "Could not create the download file."; return false; }
  progress.phase = Phase::Downloading;
  progress.current = progress.total = 0;
  Transfer transfer{file.get(), nullptr, 0, limit, &cancel, &progress};
  bool ok = Download(url, transfer, error);
  if (ok) ok = fflush(file.get()) == 0 && fsync(fileno(file.get())) == 0;
  if (fclose(file.release()) != 0) ok = false;
  if (!ok && error.empty() && !cancel.load()) error = "Could not write the download to storage.";
  return ok;
}

struct Entry {
  std::string name;
  std::string staged;
  std::string backup;
  std::string target;
  uint64_t title = 0;
  bool pipeline = false;
  bool hadOld = false;
  bool backedUp = false;
  bool installed = false;
};

bool ExtractEntry(FILE *archive, uint64_t offset, uint32_t compressed, uint32_t size,
                  uint32_t expectedCrc, uint16_t method, const std::string &path,
                  const std::atomic_bool &cancel) {
  if (fseeko(archive, static_cast<off_t>(offset), SEEK_SET) != 0) return false;
  File file(fopen(path.c_str(), "wb"), &std::fclose);
  if (!file) return false;
  std::vector<uint8_t> input(64 * 1024), output(64 * 1024);
  z_stream stream{};
  if (method == 8 && inflateInit2(&stream, -MAX_WBITS) != Z_OK) return false;
  uint64_t written = 0;
  uLong crc = crc32(0, Z_NULL, 0);
  int status = Z_OK;
  bool ok = true;
  uint32_t remaining = compressed;
  while (remaining && ok && !cancel.load()) {
    const size_t bytes = std::min<size_t>(remaining, input.size());
    if (fread(input.data(), 1, bytes, archive) != bytes) { ok = false; break; }
    remaining -= static_cast<uint32_t>(bytes);
    stream.next_in = input.data();
    stream.avail_in = static_cast<uInt>(bytes);
    do {
      size_t have = bytes;
      const uint8_t *data = input.data();
      if (method == 8) {
        stream.next_out = output.data();
        stream.avail_out = static_cast<uInt>(output.size());
        status = inflate(&stream, Z_NO_FLUSH);
        have = output.size() - stream.avail_out;
        data = output.data();
        if (status == Z_BUF_ERROR && stream.avail_in == 0 && remaining) break;
        if (status != Z_OK && status != Z_STREAM_END) { ok = false; break; }
      }
      if (have > size - written || fwrite(data, 1, have, file.get()) != have) { ok = false; break; }
      crc = crc32(crc, data, static_cast<uInt>(have));
      written += have;
      if (method == 0 || status == Z_STREAM_END) break;
    } while (ok && (stream.avail_in || stream.avail_out == 0) && !cancel.load());
    if (method == 8 && status == Z_STREAM_END) break;
  }
  ok = ok && !cancel.load() && remaining == 0 && written == size && crc == expectedCrc &&
       (method == 0 ? compressed == size : status == Z_STREAM_END && stream.avail_in == 0);
  if (method == 8) inflateEnd(&stream);
  if (ok) ok = fflush(file.get()) == 0 && fsync(fileno(file.get())) == 0;
  if (fclose(file.release()) != 0) ok = false;
  return ok;
}

bool ExtractArchive(const std::string &path, Staging &staging, std::vector<Entry> &entries,
                    Progress &progress, const std::atomic_bool &cancel, std::string &error) {
  File file(fopen(path.c_str(), "rb"), &std::fclose);
  struct stat st{};
  if (!file || fstat(fileno(file.get()), &st) != 0 || st.st_size < 22) return false;
  const uint64_t length = uint64_t(st.st_size);
  std::vector<uint8_t> tail(static_cast<size_t>(std::min<uint64_t>(length, 65557)));
  if (!ReadAt(file.get(), length - tail.size(), tail.data(), tail.size())) return false;
  size_t end = tail.size();
  for (size_t i = tail.size() - 22 + 1; i-- > 0;) {
    if (Read32(tail.data() + i) == 0x06054b50 && Read16(tail.data() + i + 20) == tail.size() - i - 22) { end = i; break; }
  }
  if (end == tail.size()) return false;
  const auto *eocd = tail.data() + end;
  const uint16_t count = Read16(eocd + 10);
  const uint64_t central = Read32(eocd + 16);
  const uint64_t centralSize = Read32(eocd + 12);
  const uint64_t eocdOffset = length - tail.size() + end;
  if (Read16(eocd + 4) || Read16(eocd + 6) || Read16(eocd + 8) != count ||
      !count || count > 20000 || central > eocdOffset || centralSize > eocdOffset - central) return false;
  std::set<std::string> names;
  uint64_t cursor = central, extracted = 0;
  progress.phase = Phase::Extracting;
  progress.current = 0;
  progress.total = count;
  for (unsigned i = 0; i < count && !cancel.load(); ++i) {
    std::array<uint8_t, 46> header{};
    if (cursor > central + centralSize || header.size() > central + centralSize - cursor ||
        !ReadAt(file.get(), cursor, header.data(), header.size()) || Read32(header.data()) != 0x02014b50) return false;
    const uint16_t flags = Read16(header.data() + 8), method = Read16(header.data() + 10);
    const uint32_t compressed = Read32(header.data() + 20), size = Read32(header.data() + 24);
    const uint16_t nameSize = Read16(header.data() + 28);
    const uint64_t recordSize = 46u + nameSize + Read16(header.data() + 30) + Read16(header.data() + 32);
    if (recordSize > central + centralSize - cursor || nameSize > 128 || !nameSize ||
        (flags & 1) || (method != 0 && method != 8) || Read16(header.data() + 34)) return false;
    std::string name(nameSize, '\0');
    if (!ReadAt(file.get(), cursor + 46, name.data(), name.size())) return false;
    cursor += recordSize;
    progress.current = i + 1;
    if (name == "shaderCache/" || name == "shaderCache/transferable/") continue;
    constexpr std::string_view prefix = "shaderCache/transferable/";
    if (name.compare(0, prefix.size(), prefix) != 0) return false;
    Entry entry;
    entry.name = name.substr(prefix.size());
    if (!CacheName(entry.name, entry.title, entry.pipeline) || !names.insert(entry.name).second ||
        size > maxCache || size > maxExtracted - extracted) return false;
    const uint64_t local = Read32(header.data() + 42);
    std::array<uint8_t, 30> localHeader{};
    if (local > central || localHeader.size() > central - local ||
        !ReadAt(file.get(), local, localHeader.data(), localHeader.size()) || Read32(localHeader.data()) != 0x04034b50 ||
        (Read16(localHeader.data() + 6) & 1) || Read16(localHeader.data() + 8) != method) return false;
    const uint64_t offset = local + 30 + Read16(localHeader.data() + 26) + Read16(localHeader.data() + 28);
    if (offset > central || compressed > central - offset) return false;
    entry.staged = staging.Path(entry.name);
    if (!ExtractEntry(file.get(), offset, compressed, size, Read32(header.data() + 16), method, entry.staged, cancel) ||
        !ValidCache(entry.staged, entry.title, entry.pipeline)) {
      error = "The shader archive contains a damaged cache.";
      return false;
    }
    extracted += size;
    entries.push_back(std::move(entry));
  }
  for (const auto &entry : entries) {
    const auto counterpart = entry.name.substr(0, 16) + (entry.pipeline ? "_shaders.bin" : "_vkpipeline.bin");
    if (!names.count(counterpart)) { error = "The shader archive contains an incomplete game pack."; return false; }
  }
  return !cancel.load() && cursor == central + centralSize && !entries.empty();
}

bool InstallEntries(const std::string &cacheDir, Staging &staging, std::vector<Entry> &entries,
                    Progress &progress, const std::atomic_bool &cancel, std::string &error) {
  const std::string destination = cacheDir + "/shaderCache/transferable";
  if (!MakeDirectories(destination)) {
    error = "Could not create the shader-cache folder: " + destination + " (" + std::strerror(errno) + ")";
    return false;
  }
  for (auto &entry : entries) {
    entry.target = destination + "/" + entry.name;
    entry.backup = staging.Path(entry.name + ".old");
    struct stat st{};
    entry.hadOld = lstat(entry.target.c_str(), &st) == 0;
    if ((!entry.hadOld && errno != ENOENT) || (entry.hadOld && !S_ISREG(st.st_mode))) {
      error = "Could not access an existing shader cache.";
      return false;
    }
  }
  progress.phase = Phase::Installing;
  progress.current = 0;
  progress.total = entries.size();
  bool ok = true;
  for (auto &entry : entries) {
    if (cancel.load()) { ok = false; break; }
    if (entry.hadOld) {
      if (rename(entry.target.c_str(), entry.backup.c_str()) != 0) { ok = false; break; }
      entry.backedUp = true;
    }
    if (rename(entry.staged.c_str(), entry.target.c_str()) != 0) { ok = false; break; }
    entry.installed = true;
    ++progress.current;
  }
  if (ok && fsdevCommitDevice("sdmc") != 0) ok = false;
  if (!ok) {
    bool restored = true;
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
      if (it->installed) remove(it->target.c_str());
      if (it->backedUp && rename(it->backup.c_str(), it->target.c_str()) != 0) {
        staging.Keep(it->backup);
        restored = false;
      }
    }
    if (!cancel.load()) error = "Could not install the shader pack. Check the free space on your SD card.";
    if (!restored) error = "Could not restore a previous cache. Its backup was kept in the download folder.";
  }
  fsdevCommitDevice("sdmc");
  return ok;
}

struct CatalogReader : rapidjson::BaseReaderHandler<rapidjson::UTF8<>, CatalogReader> {
  std::vector<Pack> packs;
  std::set<std::string> titles;
  Pack current;
  std::string field;
  unsigned depth = 0;
  bool hasName = false, hasRegion = false;

  bool Default() { return depth >= 2 && (depth != 2 || (field != "name" && field != "region")); }
  bool StartObject() {
    if (depth == 1) { hasName = hasRegion = false; field.clear(); }
    else if (depth && !Default()) return false;
    return ++depth <= 32;
  }
  bool EndObject(rapidjson::SizeType) {
    if (depth == 2) {
      if (!hasName || !hasRegion) return false;
      packs.push_back(std::move(current));
      current = {};
    }
    return depth-- > 0;
  }
  bool StartArray() {
    if (!Default()) return false;
    return ++depth <= 32;
  }
  bool EndArray(rapidjson::SizeType) { return depth-- > 2; }
  bool Key(const char *text, rapidjson::SizeType length, bool) {
    if (depth == 1) {
      uint64_t title = 0;
      current.titleId.assign(text, length);
      return titles.size() < 10000 && TitleId(current.titleId, title) && titles.insert(current.titleId).second;
    }
    if (depth == 2) field.assign(text, length);
    return true;
  }
  bool String(const char *text, rapidjson::SizeType length, bool) {
    if (depth != 2 || (field != "name" && field != "region")) return Default();
    const bool name = field == "name";
    bool &present = name ? hasName : hasRegion;
    if (present || (name && !length) || length > (name ? 256u : 32u) ||
        std::any_of(text, text + length, [](unsigned char c) { return c < 32; })) return false;
    (name ? current.name : current.region).assign(text, length);
    present = true;
    return true;
  }
};

}

bool LoadCatalog(std::vector<Pack> &packs, std::string &error, const std::atomic_bool &cancel) {
  std::string body;
  Transfer transfer{nullptr, &body, 0, 4 * 1024 * 1024, &cancel, nullptr};
  if (!Download(std::string(repository) + "shaders.json", transfer, error)) return false;
  rapidjson::MemoryStream input(body.data(), body.size());
  rapidjson::Reader reader;
  CatalogReader catalog;
  if (!reader.Parse<rapidjson::kParseValidateEncodingFlag | rapidjson::kParseIterativeFlag>(input, catalog)) {
    error = "Could not read the shader-pack catalog.";
    return false;
  }
  auto &found = catalog.packs;
  std::sort(found.begin(), found.end(), [](const Pack &a, const Pack &b) {
    return a.name != b.name ? a.name < b.name : a.region < b.region;
  });
  if (found.empty()) { error = "No shader packs are available."; return false; }
  if (cancel.load()) return false;
  packs = std::move(found);
  return true;
}

bool IsInstalled(const std::string &cacheDir, const Pack &pack) {
  uint64_t title = 0;
  if (!TitleId(pack.titleId, title)) return false;
  const auto path = cacheDir + "/shaderCache/transferable/" + pack.titleId;
  return ValidCache(path + "_shaders.bin", title, false) && ValidCache(path + "_vkpipeline.bin", title, true);
}

bool Install(const std::string &cacheDir, const Pack *pack, Progress &progress,
             const std::atomic_bool &cancel, std::string &error) {
  Staging staging;
  if (!staging.Create(cacheDir)) {
    error = "Could not create the shader download folder: " + cacheDir + " (" + std::strerror(errno) + ")";
    return false;
  }
  std::vector<Entry> entries;
  if (pack) {
    uint64_t title = 0;
    if (!TitleId(pack->titleId, title)) { error = "Invalid shader-pack title."; return false; }
    for (bool pipeline : {false, true}) {
      Entry entry;
      entry.name = pack->titleId + (pipeline ? "_vkpipeline.bin" : "_shaders.bin");
      entry.title = title;
      entry.pipeline = pipeline;
      entry.staged = staging.Path(entry.name);
      const auto url = std::string(repository) + (pipeline ? "pipelines/" : "shaders/") + entry.name;
      if (!DownloadFile(url, entry.staged, maxCache, progress, cancel, error)) return false;
      if (!ValidCache(entry.staged, title, pipeline)) { error = "The downloaded shader cache is invalid."; return false; }
      entries.push_back(std::move(entry));
    }
  } else {
    const auto path = staging.Path("shaders.zip");
    if (!DownloadFile(fullArchive, path, maxArchive, progress, cancel, error)) return false;
    if (!ExtractArchive(path, staging, entries, progress, cancel, error)) {
      if (error.empty() && !cancel.load()) error = "Could not extract the shader pack.";
      return false;
    }
  }
  return InstallEntries(cacheDir, staging, entries, progress, cancel, error);
}

}
