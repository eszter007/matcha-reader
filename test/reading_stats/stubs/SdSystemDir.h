#pragma once

#include <string>

// Host stub for lib/hal/SdSystemDir.h. The firmware resolves "/.system" or "/system" by probing
// the card; there is no card here, so the visible name is returned unconditionally -- these tests
// are about what the stats files CONTAIN, not where they live, and the stubbed HalStorage next to
// this header keeps their paths in memory anyway.
namespace sdsystem {

inline const char* dir() { return "/system"; }

inline std::string path(const char* leaf) {
  std::string out = dir();
  if (leaf && *leaf) {
    if (*leaf != '/') out += '/';
    out += leaf;
  }
  return out;
}

inline std::string findUserFile(const char* leaf) { return path(leaf); }

}  // namespace sdsystem
