// Switch 2 forwarder.h: no HOME Menu shortcuts (kaiHEN gives homebrew no ns/ncm/spl, and a forwarder would boot
// without kaiHEN). Every call fails with a message the launcher shows.
#include "forwarder.h"

#include <cstdio>
#include <string>

#if !defined(__SWITCH2__)
#error "forwarder_switch2.cpp is only for the Switch 2 launcher"
#endif

std::string g_forwarderSelfPath;

namespace {

bool refuse(char *err, std::size_t errSize) {
  if (err && errSize)
    std::snprintf(err, errSize, "HOME Menu shortcuts are not available on the Switch 2.");
  return false;
}

}  // namespace

bool forwarder_create(const std::string &, const std::string &, const std::string &, const std::string &,
                      const std::string &, char *err, std::size_t errSize) {
  return refuse(err, errSize);
}

bool forwarder_create_launcher(char *err, std::size_t errSize) {
  return refuse(err, errSize);
}
