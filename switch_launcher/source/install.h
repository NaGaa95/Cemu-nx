#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Extracted titles (folders holding code/app.xml) at srcFolder or up to two levels below it, so one folder holding
// a game, its update and its DLC installs at once
std::vector<std::string> cemu_findTitleFolders(const std::string &srcFolder);

uint64_t cemu_peekTitleId(const std::string &srcFolder);

// Title metadata determines the destination; progress may be null.
int cemu_installTitle(const std::string &srcFolder, const std::string &mlcRoot,
                      void (*progress)(int), std::string &errMsg);

bool cemu_removeInstalledComponent(const std::string &mlcRoot, uint64_t titleId,
                                   std::string &errMsg);
