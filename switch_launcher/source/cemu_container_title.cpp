#include "cemu_container_title.h"

#include <switch.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include <zarchive/zarchivereader.h>
#pragma GCC diagnostic pop

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <string_view>
#include <vector>

namespace {

constexpr uint64_t kDiscSectorSize = 0x8000;
constexpr uint64_t kPartitionTableOffset = kDiscSectorSize * 3;
constexpr size_t kMaxSiFstSize = 32 * 1024 * 1024;
constexpr size_t kMaxTicketSize = 4 * 1024 * 1024;

uint16_t readBe16(const void *data) {
  const auto *p = static_cast<const uint8_t *>(data);
  return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

uint32_t readBe32(const void *data) {
  const auto *p = static_cast<const uint8_t *>(data);
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
         (uint32_t)p[2] << 8 | p[3];
}

uint32_t readLe32(const void *data) {
  const auto *p = static_cast<const uint8_t *>(data);
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
         (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

uint64_t readLe64(const void *data) {
  const auto *p = static_cast<const uint8_t *>(data);
  return (uint64_t)readLe32(p) | (uint64_t)readLe32(p + 4) << 32;
}

bool checkedAdd(uint64_t left, uint64_t right, uint64_t &out) {
  if (right > std::numeric_limits<uint64_t>::max() - left)
    return false;
  out = left + right;
  return true;
}

bool checkedMul(uint64_t left, uint64_t right, uint64_t &out) {
  if (left && right > std::numeric_limits<uint64_t>::max() / left)
    return false;
  out = left * right;
  return true;
}

std::string lowerExtension(const std::string &path) {
  size_t dot = path.find_last_of('.');
  std::string extension = dot == std::string::npos ? std::string() : path.substr(dot);
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char c) { return (char)std::tolower(c); });
  return extension;
}

uint64_t normalizeBaseTitleId(uint64_t titleId) {
  if ((titleId >> 48) != 0x0005)
    return 0;
  uint8_t type = (uint8_t)(titleId >> 32);
  if (type == 0x00 || type == 0x02)
    return titleId;
  if (type == 0x0C || type == 0x0E)
    return 0x0005000000000000ULL | (titleId & 0xFFFFFFFFULL);
  return 0;
}

bool parseWuaFolder(std::string_view name, uint64_t &titleId) {
  if (name.size() < 19 || name[16] != '_' ||
      (name[17] != 'v' && name[17] != 'V'))
    return false;
  uint64_t parsed = 0;
  for (size_t i = 0; i < 16; ++i) {
    unsigned digit;
    char c = name[i];
    if (c >= '0' && c <= '9') digit = (unsigned)(c - '0');
    else if (c >= 'a' && c <= 'f') digit = (unsigned)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') digit = (unsigned)(c - 'A' + 10);
    else return false;
    parsed = (parsed << 4) | digit;
  }
  std::string_view version = name.substr(18);
  if (version.empty() || (version.size() > 1 && version.front() == '0'))
    return false;
  unsigned value = 0;
  for (char c : version) {
    if (c < '0' || c > '9' || value > 6553)
      return false;
    value = value * 10 + (unsigned)(c - '0');
  }
  if (value > 65535)
    return false;
  titleId = parsed;
  return true;
}

uint64_t readWuaTitleId(const std::string &path, std::string *error) {
  std::unique_ptr<ZArchiveReader> archive(ZArchiveReader::OpenFromFile(std::filesystem::path(path)));
  if (!archive) {
    if (error) *error = "The WUA archive is invalid or cannot be read.";
    return 0;
  }
  ZArchiveNodeHandle root = archive->LookUp("", false, true);
  if (root == ZARCHIVE_INVALID_NODE || !archive->IsDirectory(root)) {
    if (error) *error = "The WUA archive has no valid root directory.";
    return 0;
  }

  uint64_t fallback = 0;
  uint32_t count = archive->GetDirEntryCount(root);
  for (uint32_t i = 0; i < count; ++i) {
    ZArchiveReader::DirEntry entry{};
    uint64_t titleId = 0;
    if (!archive->GetDirEntry(root, i, entry) || !entry.isDirectory ||
        !parseWuaFolder(entry.name, titleId))
      continue;
    uint8_t type = (uint8_t)(titleId >> 32);
    if (type == 0x00 || type == 0x02)
      return titleId;
    if (!fallback)
      fallback = normalizeBaseTitleId(titleId);
  }
  if (fallback)
    return fallback;
  if (error) *error = "No Wii U base title was found in this WUA archive.";
  return 0;
}

class DiscReader {
public:
  ~DiscReader() {
    if (m_file) fclose(m_file);
  }

  bool open(const std::string &path) {
    m_file = fopen(path.c_str(), "rb");
    if (!m_file || fseeko(m_file, 0, SEEK_END) != 0)
      return false;
    off_t end = ftello(m_file);
    if (end < 0)
      return false;
    m_fileSize = (uint64_t)end;
    if (m_fileSize < 0x10004)
      return false;

    std::array<uint8_t, 32> header{};
    if (!readPhysical(0, header.data(), header.size()))
      return false;
    static constexpr uint8_t wuxMagic[8] = {0x57, 0x55, 0x58, 0x30,
                                             0x2e, 0xd0, 0x99, 0x10};
    if (memcmp(header.data(), wuxMagic, sizeof(wuxMagic)) == 0) {
      m_wux = true;
      m_sectorSize = readLe32(header.data() + 8);
      m_logicalSize = readLe64(header.data() + 16);
      if (m_sectorSize < 0x100 || m_sectorSize >= 0x10000000 ||
          m_logicalSize < 0x20000)
        return false;
      uint64_t roundedSize = 0;
      if (!checkedAdd(m_logicalSize, m_sectorSize - 1, roundedSize))
        return false;
      m_indexCount = roundedSize / m_sectorSize;
      uint64_t tableBytes = 0, tableEnd = 0, alignedTableEnd = 0;
      if (!checkedMul(m_indexCount, 4, tableBytes) ||
          !checkedAdd(32, tableBytes, tableEnd) ||
          !checkedAdd(tableEnd, m_sectorSize - 1, alignedTableEnd))
        return false;
      m_sectorArray = alignedTableEnd / m_sectorSize * m_sectorSize;
      if (m_sectorArray >= m_fileSize)
        return false;
    } else {
      m_logicalSize = m_fileSize;
    }
    return true;
  }

  bool read(uint64_t offset, void *output, size_t size) {
    uint64_t end = 0;
    if (!m_file || !checkedAdd(offset, size, end) || end > m_logicalSize)
      return false;
    auto *dst = static_cast<uint8_t *>(output);
    if (!m_wux)
      return readPhysical(offset, dst, size);

    while (size) {
      uint64_t logicalSector = offset / m_sectorSize;
      uint64_t sectorOffset = offset % m_sectorSize;
      if (logicalSector >= m_indexCount)
        return false;
      std::array<uint8_t, 4> encodedIndex{};
      if (!readPhysical(32 + logicalSector * 4, encodedIndex.data(), encodedIndex.size()))
        return false;
      uint64_t physicalSector = readLe32(encodedIndex.data());
      uint64_t physicalOffset = 0, sectorBase = 0;
      if (!checkedMul(physicalSector, m_sectorSize, sectorBase) ||
          !checkedAdd(m_sectorArray, sectorBase, physicalOffset) ||
          !checkedAdd(physicalOffset, sectorOffset, physicalOffset))
        return false;
      size_t chunk = (size_t)std::min<uint64_t>(size, m_sectorSize - sectorOffset);
      if (!readPhysical(physicalOffset, dst, chunk))
        return false;
      dst += chunk;
      offset += chunk;
      size -= chunk;
    }
    return true;
  }

private:
  bool readPhysical(uint64_t offset, void *output, size_t size) {
    uint64_t end = 0;
    if (!checkedAdd(offset, size, end) || end > m_fileSize ||
        offset > (uint64_t)std::numeric_limits<off_t>::max() ||
        fseeko(m_file, (off_t)offset, SEEK_SET) != 0)
      return false;
    return fread(output, 1, size, m_file) == size;
  }

  FILE *m_file = nullptr;
  bool m_wux = false;
  uint64_t m_fileSize = 0;
  uint64_t m_logicalSize = 0;
  uint64_t m_sectorSize = 0;
  uint64_t m_indexCount = 0;
  uint64_t m_sectorArray = 0;
};

using AesKey = std::array<uint8_t, 16>;

bool parseKey(std::string text, AesKey &key) {
  size_t comment = text.find_first_of("#;");
  if (comment != std::string::npos)
    text.resize(comment);
  text.erase(std::remove_if(text.begin(), text.end(), [](char c) {
    return std::isspace(static_cast<unsigned char>(c)) || c == '-' || c == '_';
  }), text.end());
  if (text.size() != 32)
    return false;
  for (size_t i = 0; i < key.size(); ++i) {
    auto digit = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    int high = digit(text[i * 2]), low = digit(text[i * 2 + 1]);
    if (high < 0 || low < 0)
      return false;
    key[i] = (uint8_t)(high << 4 | low);
  }
  return true;
}

void addKey(std::vector<AesKey> &keys, const AesKey &key) {
  if (std::find(keys.begin(), keys.end(), key) == keys.end())
    keys.push_back(key);
}

std::vector<AesKey> loadKeys(const std::string &discPath, const std::string &keysPath) {
  std::vector<AesKey> keys;
  std::string adjacent = discPath;
  size_t dot = adjacent.find_last_of('.');
  if (dot == std::string::npos) adjacent += ".key";
  else adjacent.replace(dot, std::string::npos, ".key");
  if (FILE *file = fopen(adjacent.c_str(), "rb")) {
    AesKey key{};
    if (fread(key.data(), 1, key.size(), file) == key.size())
      addKey(keys, key);
    fclose(file);
  }

  if (FILE *file = fopen(keysPath.c_str(), "r")) {
    char line[1024];
    while (fgets(line, sizeof(line), file)) {
      AesKey key{};
      if (parseKey(line, key))
        addKey(keys, key);
    }
    fclose(file);
  }
  return keys;
}

bool decryptCbc(const AesKey &key, const uint8_t iv[16],
                const void *source, void *destination, size_t size) {
  if ((size & 15) != 0)
    return false;
  Aes128CbcContext context{};
  aes128CbcContextCreate(&context, key.data(), iv, false);
  return aes128CbcDecrypt(&context, destination, source, size) == size;
}

bool findDiscKey(DiscReader &disc, const std::vector<AesKey> &keys, AesKey &found) {
  std::array<uint8_t, 48> sample{};
  if (!disc.read(kPartitionTableOffset + 0x100, sample.data(), sample.size()))
    return false;
  std::array<uint8_t, 32> decrypted{};
  for (const AesKey &key : keys) {
    if (!decryptCbc(key, sample.data(), sample.data() + 16,
                    decrypted.data(), decrypted.size()))
      continue;
    if (std::all_of(decrypted.begin(), decrypted.end(), [](uint8_t byte) { return byte == 0; })) {
      found = key;
      return true;
    }
  }
  return false;
}

struct FstEntry {
  bool directory = false;
  uint32_t parent = 0;
  uint32_t offset = 0;
  uint32_t size = 0;
  uint16_t cluster = 0;
  std::string_view name;
};

bool parseFst(const std::vector<uint8_t> &fst, uint32_t gmPartition,
              uint32_t &offsetFactor, std::vector<uint32_t> &clusterOffsets,
              std::vector<uint8_t> &clusterModes, FstEntry &ticket) {
  if (fst.size() < 0x20 || readBe32(fst.data()) != 0x46535400)
    return false;
  offsetFactor = readBe32(fst.data() + 4);
  uint32_t clusterCount = readBe32(fst.data() + 8);
  if (!offsetFactor || clusterCount >= 0x1000 ||
      (uint64_t)0x20 + (uint64_t)clusterCount * 0x20 + 0x10 > fst.size())
    return false;

  clusterOffsets.resize(clusterCount);
  clusterModes.resize(clusterCount);
  for (uint32_t i = 0; i < clusterCount; ++i) {
    const uint8_t *cluster = fst.data() + 0x20 + (size_t)i * 0x20;
    clusterOffsets[i] = readBe32(cluster);
    clusterModes[i] = cluster[0x14];
  }

  const uint8_t *table = fst.data() + 0x20 + (size_t)clusterCount * 0x20;
  uint32_t rootType = readBe32(table) >> 24;
  uint32_t entryCount = readBe32(table + 8);
  if ((rootType & 1) == 0 || readBe32(table + 4) != 0 || !entryCount ||
      (uint64_t)(table - fst.data()) + (uint64_t)entryCount * 0x10 > fst.size())
    return false;
  const uint8_t *names = table + (size_t)entryCount * 0x10;
  size_t namesSize = fst.data() + fst.size() - names;

  static constexpr char hex[]="0123456789abcdef";
  if(gmPartition>0xFF) return false;
  char partitionName[3]={hex[(gmPartition>>4)&0xF],hex[gmPartition&0xF],0};
  std::vector<std::pair<uint32_t, uint32_t>> stack;
  stack.emplace_back(0, entryCount);
  uint32_t partitionDirectory = UINT32_MAX;
  for (uint32_t i = 0; i < entryCount; ++i) {
    while (!stack.empty() && i >= stack.back().second)
      stack.pop_back();
    if (stack.empty())
      return false;
    const uint8_t *raw = table + (size_t)i * 0x10;
    uint32_t typeAndName = readBe32(raw);
    uint8_t flags = (uint8_t)(typeAndName >> 24);
    bool directory = (flags & 1) != 0;
    uint32_t nameOffset = typeAndName & 0xFFFFFF;
    if ((flags & ~0x83) != 0 || nameOffset >= namesSize)
      return false;
    const char *name = reinterpret_cast<const char *>(names + nameOffset);
    const void *terminator = memchr(name, 0, namesSize - nameOffset);
    if (!terminator)
      return false;
    std::string_view entryName(name, static_cast<const char *>(terminator) - name);
    uint32_t parent = stack.back().first;

    if (directory) {
      uint32_t declaredParent = readBe32(raw + 4);
      uint32_t end = readBe32(raw + 8);
      if (declaredParent != parent || end <= i || end > stack.back().second)
        return false;
      if (parent == 0 && entryName == partitionName)
        partitionDirectory = i;
      stack.emplace_back(i, end);
    } else {
      uint16_t cluster = readBe16(raw + 0x0E);
      if (cluster >= clusterCount)
        return false;
      if (parent == partitionDirectory && entryName == "title.tik") {
        ticket = {false, parent, readBe32(raw + 4), readBe32(raw + 8), cluster, entryName};
        return ticket.size >= 0x220 && ticket.size <= kMaxTicketSize;
      }
    }
  }
  return false;
}

bool readFstFile(DiscReader &disc, uint64_t partitionBase, const AesKey &key,
                 uint32_t offsetFactor, const std::vector<uint32_t> &clusterOffsets,
                 const std::vector<uint8_t> &clusterModes, const FstEntry &file,
                 std::vector<uint8_t> &output) {
  if (file.cluster >= clusterOffsets.size() || clusterModes[file.cluster] > 2)
    return false;
  uint64_t fileOffset = 0;
  if (!checkedMul(file.offset, offsetFactor, fileOffset))
    return false;
  output.resize(file.size);
  size_t copied = 0;
  bool hashed=clusterModes[file.cluster]==2;
  size_t rawBlockSize=hashed?0x10000:(size_t)kDiscSectorSize;
  size_t dataBlockSize=hashed?0xFC00:(size_t)kDiscSectorSize;
  std::vector<uint8_t> encrypted(rawBlockSize),decrypted(rawBlockSize);
  while (copied < output.size()) {
    uint64_t current = fileOffset + copied;
    uint64_t block = current / dataBlockSize;
    size_t inside = (size_t)(current % dataBlockSize);
    uint64_t clusterBase = 0, blockOffset = 0, absolute = 0;
    if (!checkedMul(clusterOffsets[file.cluster], kDiscSectorSize, clusterBase) ||
        !checkedMul(block, rawBlockSize, blockOffset) ||
        !checkedAdd(partitionBase, clusterBase, absolute) ||
        !checkedAdd(absolute, blockOffset, absolute) ||
        !disc.read(absolute, encrypted.data(), rawBlockSize))
      return false;

    size_t dataOffset=0;
    if(hashed){
      uint8_t zeroIv[16]{};
      if(!decryptCbc(key,zeroIv,encrypted.data(),decrypted.data(),0x400)) return false;
      const uint8_t *fileIv=decrypted.data()+20*(block%16);
      if(!decryptCbc(key,fileIv,encrypted.data()+0x400,decrypted.data()+0x400,0xFC00)) return false;
      dataOffset=0x400;
    } else {
      uint8_t iv[16]{};
      if (block == 0) {
        iv[0] = (uint8_t)(file.cluster >> 8);
        iv[1] = (uint8_t)file.cluster;
      } else if (absolute < sizeof(iv) || !disc.read(absolute - sizeof(iv), iv, sizeof(iv))) {
        return false;
      }
      if (!decryptCbc(key, iv, encrypted.data(), decrypted.data(), rawBlockSize)) return false;
    }
    size_t chunk = std::min(output.size() - copied, dataBlockSize - inside);
    memcpy(output.data() + copied, decrypted.data() + dataOffset + inside, chunk);
    copied += chunk;
  }
  return true;
}

uint64_t readDiscTitleId(const std::string &path, const std::string &keysPath,
                         std::string *error) {
  DiscReader disc;
  if (!disc.open(path)) {
    if (error) *error = "The WUD/WUX disc image is invalid or cannot be read.";
    return 0;
  }
  std::array<uint8_t, 4> headerMagic{};
  if (!disc.read(0x10000, headerMagic.data(), headerMagic.size()) ||
      readBe32(headerMagic.data()) != 0xCC549EB9) {
    if (error) *error = "The file is not a valid Wii U disc image.";
    return 0;
  }

  std::vector<AesKey> keys = loadKeys(path, keysPath);
  AesKey discKey{};
  if (keys.empty() || !findDiscKey(disc, keys, discKey)) {
    if (error) *error = "Could not decrypt the WUD/WUX metadata with the available keys.";
    return 0;
  }

  std::array<uint8_t, kDiscSectorSize> encryptedTable{};
  std::array<uint8_t, kDiscSectorSize> partitionTable{};
  uint8_t zeroIv[16]{};
  if (!disc.read(kPartitionTableOffset, encryptedTable.data(), encryptedTable.size()) ||
      !decryptCbc(discKey, zeroIv, encryptedTable.data(), partitionTable.data(), partitionTable.size()) ||
      readBe32(partitionTable.data()) != 0xCCA6E67B ||
      readBe32(partitionTable.data() + 4) != kDiscSectorSize) {
    if (error) *error = "The disc partition table is invalid.";
    return 0;
  }

  uint32_t count = readBe32(partitionTable.data() + 0x1C);
  if (!count || count > 30) {
    if (error) *error = "The disc partition layout is unsupported.";
    return 0;
  }
  uint32_t siIndex = UINT32_MAX, gmIndex = UINT32_MAX, siAddress = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t *entry = partitionTable.data() + 0x800 + (size_t)i * 0x80;
    if (entry[0x1F] != 1) {
      if (error) *error = "The disc uses an unsupported partition layout.";
      return 0;
    }
    if (entry[0] == 'S' && entry[1] == 'I') {
      if (siIndex != UINT32_MAX) {
        if (error) *error = "The disc contains multiple SI partitions.";
        return 0;
      }
      siIndex = i;
      siAddress = readBe32(entry + 0x20);
    } else if (entry[0] == 'G' && entry[1] == 'M' && gmIndex == UINT32_MAX) {
      gmIndex = i;
    }
  }
  if (siIndex == UINT32_MAX || gmIndex == UINT32_MAX) {
    if (error) *error = "The disc is missing its SI or game partition.";
    return 0;
  }

  uint64_t siBase = 0;
  if (!checkedMul(siAddress, kDiscSectorSize, siBase))
    return 0;
  std::array<uint8_t, 0x60> partitionHeader{};
  if (!disc.read(siBase, partitionHeader.data(), partitionHeader.size()) ||
      readBe32(partitionHeader.data()) != 0xCC93A4F5 ||
      readBe32(partitionHeader.data() + 4) != kDiscSectorSize ||
      partitionHeader[0x24] != 0 || partitionHeader[0x25] != 1) {
    if (error) *error = "The disc SI partition is invalid or unsupported.";
    return 0;
  }
  uint32_t fstSize = readBe32(partitionHeader.data() + 0x14);
  uint32_t fstSector = readBe32(partitionHeader.data() + 0x18);
  size_t paddedFstSize = ((size_t)fstSize + 15) & ~(size_t)15;
  if (fstSize < 0x30 || fstSize > kMaxSiFstSize || paddedFstSize < fstSize) {
    if (error) *error = "The disc SI file table is invalid.";
    return 0;
  }
  uint64_t fstRelative = 0, fstAbsolute = 0;
  if (!checkedMul(fstSector, kDiscSectorSize, fstRelative) ||
      !checkedAdd(siBase, fstRelative, fstAbsolute))
    return 0;
  std::vector<uint8_t> encryptedFst(paddedFstSize), fst(paddedFstSize);
  if (!disc.read(fstAbsolute, encryptedFst.data(), encryptedFst.size()) ||
      !decryptCbc(discKey, zeroIv, encryptedFst.data(), fst.data(), fst.size())) {
    if (error) *error = "The disc SI file table could not be read.";
    return 0;
  }
  fst.resize(fstSize);

  uint32_t offsetFactor = 0;
  std::vector<uint32_t> clusterOffsets;
  std::vector<uint8_t> clusterModes;
  FstEntry ticket;
  if (!parseFst(fst, gmIndex, offsetFactor, clusterOffsets, clusterModes, ticket)) {
    if (error) *error = "The game ticket was not found in the disc metadata.";
    return 0;
  }
  std::vector<uint8_t> ticketData;
  if (!readFstFile(disc, siBase, discKey, offsetFactor, clusterOffsets,
                   clusterModes, ticket, ticketData)) {
    if (error) *error = "The game ticket could not be decrypted.";
    return 0;
  }
  uint64_t titleId = (uint64_t)readBe32(ticketData.data() + 0x1DC) << 32 |
                     readBe32(ticketData.data() + 0x1E0);
  titleId = normalizeBaseTitleId(titleId);
  if (!titleId && error)
    *error = "The disc ticket contains an unsupported title ID.";
  return titleId;
}

} // namespace

bool cemu_hasConfiguredDiscKey(const std::string &keysPath) {
  AesKey exampleKey{};
  if (!parseKey("541b9889519b27d363cd21604b97c67a", exampleKey))
    return false;

  FILE *file = fopen(keysPath.c_str(), "r");
  if (!file)
    return false;

  bool configured = false;
  char line[1024];
  while (fgets(line, sizeof(line), file)) {
    AesKey key{};
    if (parseKey(line, key) && key != exampleKey) {
      configured = true;
      break;
    }
  }
  fclose(file);
  return configured;
}

uint64_t cemu_readContainerBaseTitleId(const std::string &path,
                                       const std::string &keysPath,
                                       std::string *error) {
  if (error) error->clear();
  std::string extension = lowerExtension(path);
  try {
    if (extension == ".wua")
      return readWuaTitleId(path, error);
    if (extension == ".wud" || extension == ".wux" || extension == ".iso")
      return readDiscTitleId(path, keysPath, error);
  } catch (...) {
    if (error) *error = "The container metadata could not be read safely.";
  }
  return 0;
}
