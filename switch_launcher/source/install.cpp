#include "install.h"
#include "tinyxml2.h"

extern "C" {
#include <switch/runtime/devices/fs_dev.h>
}

#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <limits>
#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace {

struct InstallEntry {
  std::string relativePath;
  uint64_t size{};
  bool directory{};
};

bool exists(const std::string &path, bool *directory = nullptr) {
  struct stat st{};
  if (stat(path.c_str(), &st) != 0)
    return false;
  if (directory)
    *directory = S_ISDIR(st.st_mode);
  return true;
}

bool ensureDirectory(const std::string &path) {
  bool directory = false;
  if (exists(path, &directory))
    return directory;
  if (mkdir(path.c_str(), 0777) == 0)
    return true;
  return errno == EEXIST && exists(path, &directory) && directory;
}

bool ensureDirectories(const std::string &path) {
  if (path.empty())
    return false;
  size_t start = 1;
  if (const size_t scheme = path.find(":/"); scheme != std::string::npos)
    start = scheme + 2;
  for (size_t slash = path.find('/', start); slash != std::string::npos;
       slash = path.find('/', slash + 1)) {
    if (!ensureDirectory(path.substr(0, slash)))
      return false;
  }
  return ensureDirectory(path);
}

bool isDotEntry(const char *name) {
  return !std::strcmp(name, ".") || !std::strcmp(name, "..");
}

bool collectTree(const std::string &sourceRoot, const std::string &relative,
                 std::vector<InstallEntry> &entries, uint64_t &totalBytes,
                 unsigned depth, std::string &error) {
  if (depth > 64 || entries.size() >= 250000) {
    error = "The title contains too many nested files";
    return false;
  }

  const std::string source = sourceRoot + "/" + relative;
  struct stat st{};
  if (stat(source.c_str(), &st) != 0) {
    error = "Could not read " + relative;
    return false;
  }
  if (S_ISDIR(st.st_mode)) {
    entries.push_back({relative, 0, true});
    DIR *dir = opendir(source.c_str());
    if (!dir) {
      error = "Could not open " + relative;
      return false;
    }
    bool ok = true;
    while (ok) {
      dirent *entry = readdir(dir);
      if (!entry)
        break;
      if (isDotEntry(entry->d_name))
        continue;
      ok = collectTree(sourceRoot, relative + "/" + entry->d_name,
                       entries, totalBytes, depth + 1, error);
    }
    closedir(dir);
    return ok;
  }
  if (!S_ISREG(st.st_mode) || st.st_size < 0) {
    error = "Unsupported file type in " + relative;
    return false;
  }
  const uint64_t size = static_cast<uint64_t>(st.st_size);
  if (size > std::numeric_limits<uint64_t>::max() - totalBytes) {
    error = "The title is too large";
    return false;
  }
  totalBytes += size;
  entries.push_back({relative, size, false});
  return true;
}

bool safeTransactionPath(const std::string &path) {
  const bool suffix = path.size() >= 12 &&
    (path.compare(path.size() - 12, 12, ".install.tmp") == 0 ||
     path.compare(path.size() - 12, 12, ".install.old") == 0);
  return suffix && path.find("/title/") != std::string::npos;
}

bool removeTree(const std::string &path) {
  struct stat st{};
  if (lstat(path.c_str(), &st) != 0)
    return errno == ENOENT;
  if (!S_ISDIR(st.st_mode))
    return remove(path.c_str()) == 0;

  DIR *dir = opendir(path.c_str());
  if (!dir)
    return false;
  bool ok = true;
  while (ok) {
    dirent *entry = readdir(dir);
    if (!entry)
      break;
    if (isDotEntry(entry->d_name))
      continue;
    const std::string child = path + "/" + entry->d_name;
    struct stat childStat{};
    if (lstat(child.c_str(), &childStat) != 0) {
      ok = false;
    } else if (S_ISDIR(childStat.st_mode)) {
      ok = removeTree(child);
    } else {
      ok = remove(child.c_str()) == 0;
    }
  }
  closedir(dir);
  return ok && rmdir(path.c_str()) == 0;
}

bool removeTransactionTree(const std::string &path) {
  return safeTransactionPath(path) && removeTree(path);
}

bool safeMlcRoot(const std::string &root) {
  if (root.empty() || root.find('\\') != std::string::npos ||
      root.find(":/") == std::string::npos)
    return false;
  size_t start = root.find(":/") + 2;
  while (start <= root.size()) {
    const size_t end = root.find('/', start);
    const std::string part = root.substr(start, end - start);
    if (part == "." || part == "..")
      return false;
    if (end == std::string::npos)
      break;
    start = end + 1;
  }
  return true;
}

bool realDirectory(const std::string &path) {
  struct stat st{};
  return lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode) &&
         !S_ISLNK(st.st_mode);
}

bool copyFile(const std::string &source, const std::string &destination,
              uint64_t expectedSize, uint64_t &copiedBytes, uint64_t totalBytes,
              size_t &completedFiles, size_t totalFiles, int &lastProgress,
              void (*progress)(int)) {
  FILE *input = fopen(source.c_str(), "rb");
  if (!input)
    return false;
  FILE *output = fopen(destination.c_str(), "wb");
  if (!output) {
    fclose(input);
    return false;
  }

  static std::array<unsigned char, 256 * 1024> buffer;
  uint64_t written = 0;
  bool ok = true;
  while (ok) {
    const size_t count = fread(buffer.data(), 1, buffer.size(), input);
    if (count == 0)
      break;
    if (fwrite(buffer.data(), 1, count, output) != count) {
      ok = false;
      break;
    }
    written += count;
    copiedBytes += count;
    if (progress && totalBytes > 0) {
      const int percent = static_cast<int>(
        static_cast<unsigned __int128>(copiedBytes) * 100 / totalBytes);
      if (percent != lastProgress) {
        progress(percent);
        lastProgress = percent;
      }
    }
  }
  if (ferror(input) || written != expectedSize)
    ok = false;
  if (fclose(input) != 0)
    ok = false;
  if (fflush(output) != 0 || fsync(fileno(output)) != 0)
    ok = false;
  if (fclose(output) != 0)
    ok = false;
  if (!ok) {
    remove(destination.c_str());
    return false;
  }

  completedFiles++;
  if (progress && totalBytes == 0 && totalFiles > 0) {
    const int percent = static_cast<int>(completedFiles * 100 / totalFiles);
    if (percent != lastProgress) {
      progress(percent);
      lastProgress = percent;
    }
  }
  return true;
}

uint64_t readAppTitleId(const std::string &appXmlPath) {
  tinyxml2::XMLDocument doc;
  if (doc.LoadFile(appXmlPath.c_str()) != tinyxml2::XML_SUCCESS)
    return 0;
  auto *app = doc.FirstChildElement("app");
  auto *tid = app ? app->FirstChildElement("title_id") : nullptr;
  const char *text = tid ? tid->GetText() : nullptr;
  if (!text || !*text)
    return 0;
  char *end = nullptr;
  const uint64_t value = strtoull(text, &end, 16);
  while (end && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n'))
    ++end;
  return value && end && *end == '\0' ? value : 0;
}

} // namespace

static bool findTitleBase(const std::string &srcFolder, std::string &base) {
  struct stat st{};
  base = srcFolder;
  if (stat((base + "/code/app.xml").c_str(), &st) == 0)
    return true;
  DIR *dir = opendir(base.c_str());
  if (!dir)
    return false;
  bool found = false;
  while (dirent *entry = readdir(dir)) {
    if (isDotEntry(entry->d_name))
      continue;
    const std::string candidate = base + "/" + entry->d_name;
    if (stat((candidate + "/code/app.xml").c_str(), &st) == 0) {
      base = candidate;
      found = true;
      break;
    }
  }
  closedir(dir);
  return found;
}

static bool hasAppXml(const std::string &dir) {
  struct stat st{};
  return stat((dir + "/code/app.xml").c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

static void collectTitleFolders(const std::string &dir, unsigned depth, std::vector<std::string> &out) {
  if (out.size() >= 64)
    return;
  if (hasAppXml(dir)) {
    out.push_back(dir);
    return;
  }
  if (depth == 0)
    return;
  DIR *handle = opendir(dir.c_str());
  if (!handle)
    return;
  std::vector<std::string> children;
  while (dirent *entry = readdir(handle)) {
    if (entry->d_name[0] == '.')
      continue;
    const std::string child = dir + "/" + entry->d_name;
    struct stat st{};
    if (stat(child.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
      children.push_back(child);
  }
  closedir(handle);
  std::sort(children.begin(), children.end());
  for (const auto &child : children)
    collectTitleFolders(child, depth - 1, out);
}

std::vector<std::string> cemu_findTitleFolders(const std::string &srcFolder) {
  std::string root = srcFolder;
  while (root.size() > 1 && root.back() == '/' && root[root.size() - 2] != ':')
    root.pop_back();
  std::vector<std::string> found;
  collectTitleFolders(root, 2, found);
  std::vector<std::string> valid;
  for (const auto &folder : found) {
    const uint64_t tid = readAppTitleId(folder + "/code/app.xml");
    if (((tid >> 48) & 0xFFFF) == 0x0005)
      valid.push_back(folder);
  }
  return valid;
}

uint64_t cemu_peekTitleId(const std::string &srcFolder) {
  std::string base;
  if (!findTitleBase(srcFolder, base))
    return 0;
  const uint64_t tid = readAppTitleId(base + "/code/app.xml");
  return ((tid >> 48) & 0xFFFF) == 0x0005 ? tid : 0;
}

int cemu_installTitle(const std::string &srcFolder, const std::string &mlcRoot,
                      void (*progress)(int), std::string &errMsg) {
  std::string base;
  if (!findTitleBase(srcFolder, base)) {
    errMsg = "No code/app.xml found - point to an extracted title folder";
    return 1;
  }

  const uint64_t tid = readAppTitleId(base + "/code/app.xml");
  if (!tid || ((tid >> 48) & 0xFFFF) != 0x0005) {
    errMsg = "Could not read a valid Wii U title id from app.xml";
    return 2;
  }
  const uint32_t high = static_cast<uint32_t>(tid >> 32);
  const uint32_t low = static_cast<uint32_t>(tid);
  const bool systemTitle = high == 0x00050010 || high == 0x00050030;

  char highName[9];
  char lowName[9];
  if (snprintf(highName, sizeof(highName), "%08x", high) != 8 ||
      snprintf(lowName, sizeof(lowName), "%08x", low) != 8) {
    errMsg = "The title id could not be converted safely";
    return 2;
  }
  const std::string parent = mlcRoot + (systemTitle ? "/sys/title/" : "/usr/title/") + highName;
  const std::string destination = parent + "/" + lowName;
  const std::string stage = parent + "/." + lowName + ".install.tmp";
  const std::string backup = parent + "/." + lowName + ".install.old";
  if (!ensureDirectories(parent)) {
    errMsg = "Could not create the MLC title directory";
    return 3;
  }

  const bool haveDestination = exists(destination);
  const bool haveBackup = exists(backup);
  if (!haveDestination && haveBackup && rename(backup.c_str(), destination.c_str()) != 0) {
    errMsg = "A previous interrupted installation could not be recovered";
    return 3;
  }
  if (exists(destination) && exists(backup) && !removeTransactionTree(backup)) {
    errMsg = "Could not remove a stale installation backup";
    return 3;
  }
  if (exists(stage) && !removeTransactionTree(stage)) {
    errMsg = "Could not remove a stale partial installation";
    return 3;
  }

  std::vector<InstallEntry> entries;
  uint64_t totalBytes = 0;
  for (const char *subdirectory : {"content", "code", "meta"}) {
    bool directory = false;
    if (exists(base + "/" + subdirectory, &directory)) {
      if (!directory || !collectTree(base, subdirectory, entries, totalBytes, 0, errMsg))
        return 3;
    }
  }
  if (entries.empty()) {
    errMsg = "The title has no installable content, code, or meta data";
    return 3;
  }

  struct statvfs space{};
  if (statvfs(parent.c_str(), &space) == 0) {
    const uint64_t blockSize = space.f_frsize ? space.f_frsize : space.f_bsize;
    if (blockSize && space.f_bavail && space.f_bavail <= std::numeric_limits<uint64_t>::max() / blockSize) {
      const uint64_t available = static_cast<uint64_t>(space.f_bavail) * blockSize;
      const uint64_t metadataReserve = 8u * 1024 * 1024 + entries.size() * 4096ull;
      if (totalBytes > available || metadataReserve > available - totalBytes) {
        errMsg = "Not enough free SD space for a safe transactional install";
        return 4;
      }
    }
  }

  if (!ensureDirectory(stage)) {
    errMsg = "Could not create the temporary installation directory";
    return 3;
  }
  size_t fileCount = 0;
  for (const auto &entry : entries)
    if (!entry.directory)
      fileCount++;
  size_t completedFiles = 0;
  uint64_t copiedBytes = 0;
  int lastProgress = -1;
  bool copied = true;
  for (const auto &entry : entries) {
    const std::string target = stage + "/" + entry.relativePath;
    if (entry.directory) {
      copied = ensureDirectory(target);
    } else {
      copied = copyFile(base + "/" + entry.relativePath, target, entry.size,
                        copiedBytes, totalBytes, completedFiles, fileCount,
                        lastProgress, progress);
    }
    if (!copied)
      break;
  }
  if (!copied) {
    removeTransactionTree(stage);
    errMsg = "Copy failed; the existing installed title was left untouched";
    return 3;
  }
  fsdevCommitDevice("sdmc");

  const bool replacing = exists(destination);
  if (replacing && rename(destination.c_str(), backup.c_str()) != 0) {
    removeTransactionTree(stage);
    errMsg = "Could not preserve the existing title before installation";
    return 3;
  }
  if (rename(stage.c_str(), destination.c_str()) != 0) {
    if (replacing && rename(backup.c_str(), destination.c_str()) != 0)
      errMsg = "Install failed and rollback failed; the old title remains in the .install.old directory";
    else
      errMsg = "Install failed; the existing title was restored";
    removeTransactionTree(stage);
    return 3;
  }
  fsdevCommitDevice("sdmc");

  bool oldCleanupPending = false;
  if (replacing && !removeTransactionTree(backup))
    oldCleanupPending = true;
  fsdevCommitDevice("sdmc");
  if (progress && lastProgress != 100)
    progress(100);

  const char *kind = high == 0x0005000E ? "update" : high == 0x0005000C ? "DLC" : "title";
  char message[160];
  snprintf(message, sizeof(message), "Installed %s %016llx%s", kind,
           static_cast<unsigned long long>(tid),
           oldCleanupPending ? " (old backup cleanup pending)" : "");
  errMsg = message;
  return 0;
}

bool cemu_removeInstalledComponent(const std::string &mlcRoot, uint64_t titleId,
                                   std::string &errMsg) {
  errMsg.clear();
  const uint32_t high = static_cast<uint32_t>(titleId >> 32);
  if (high != 0x00050000 && high != 0x0005000C && high != 0x0005000E) {
    errMsg = "Only installed games, updates, and DLC can be removed";
    return false;
  }

  if (!safeMlcRoot(mlcRoot)) {
    errMsg = "The configured MLC path is not safe";
    return false;
  }
  std::string root = mlcRoot;
  const size_t rootStart = root.find(":/") + 2;
  while (root.size() > rootStart && root.back() == '/')
    root.pop_back();
  if (!safeMlcRoot(root)) {
    errMsg = "The configured MLC path is not safe";
    return false;
  }

  char highName[9];
  char lowName[9];
  if (snprintf(highName, sizeof(highName), "%08x", high) != 8 ||
      snprintf(lowName, sizeof(lowName), "%08x",
               static_cast<uint32_t>(titleId)) != 8) {
    errMsg = "The installed title ID is invalid";
    return false;
  }
  const std::string usr = root + "/usr";
  const std::string title = usr + "/title";
  const std::string type = title + "/" + highName;
  const std::string destination = type + "/" + lowName;
  if (!realDirectory(root) || !realDirectory(usr) || !realDirectory(title) ||
      !realDirectory(type)) {
    errMsg = "The MLC title path is missing or unsafe";
    return false;
  }
  struct stat componentStat{};
  if (lstat(destination.c_str(), &componentStat) != 0) {
    errMsg = errno == ENOENT ? "The installed component no longer exists"
                             : "The installed component could not be inspected";
    return false;
  }
  if (!S_ISDIR(componentStat.st_mode) || S_ISLNK(componentStat.st_mode)) {
    errMsg = "The installed component path is not a real directory";
    return false;
  }
  if (!removeTree(destination)) {
    errMsg = "The installed component could not be removed completely";
    fsdevCommitDevice("sdmc");
    return false;
  }
  fsdevCommitDevice("sdmc");
  return true;
}
