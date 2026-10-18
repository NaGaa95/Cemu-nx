#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace ShaderPacks {

struct Pack {
  std::string titleId;
  std::string name;
  std::string region;
};

enum class Phase { Downloading, Extracting, Installing };

struct Progress {
  std::atomic<Phase> phase{Phase::Downloading};
  std::atomic_uint64_t current{0};
  std::atomic_uint64_t total{0};
};

bool LoadCatalog(std::vector<Pack> &packs, std::string &error,
                 const std::atomic_bool &cancel);
bool IsInstalled(const std::string &cacheDir, const Pack &pack);
bool Install(const std::string &cacheDir, const Pack *pack, Progress &progress,
             const std::atomic_bool &cancel, std::string &error);

}
