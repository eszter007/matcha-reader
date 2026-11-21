#pragma once

#include <string>
#include <string_view>

namespace FsHelpers {
inline bool checkFileExtension(std::string_view path, const char* extension) { return path.ends_with(extension); }
inline bool hasEpubExtension(const std::string& path) { return checkFileExtension(path, ".epub"); }
inline bool hasXtcExtension(std::string_view path) {
  return checkFileExtension(path, ".xtc") || checkFileExtension(path, ".xtch");
}
inline bool hasMatchaExtension(std::string_view path) { return checkFileExtension(path, ".matcha"); }
inline std::string_view stripMatchaExtension(std::string_view path) {
  return hasMatchaExtension(path) ? path.substr(0, path.size() - (sizeof(".matcha") - 1)) : path;
}
}  // namespace FsHelpers
