#pragma once

#include <string_view>

namespace FsHelpers {
// A .txt file the device writes for itself rather than a book: the crash log (HalSystem), or the
// clippings export (ClippingsManager). Home and the Library leave these out; the file browser
// still lists and opens them. Takes a path or a bare file name.
inline bool isDeviceTextFile(const std::string_view path) {
  const size_t slash = path.rfind('/');
  const std::string_view name = path.substr(slash == std::string_view::npos ? 0 : slash + 1);
  return name == "crash_report.txt" || name == "My Clippings.txt";
}
}  // namespace FsHelpers
