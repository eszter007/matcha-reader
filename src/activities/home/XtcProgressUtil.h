#pragma once

#include <FsHelpers.h>
#include <HalStorage.h>

#include <string>

// Page-based reading progress for an XTC/XTCH book, computed cheaply for the Library/Home lists
// without loading the whole file. The XTC reader stores just the 0-based current page (4 bytes)
// in progress.bin; the total page count is a uint16 at offset 0x06 of the XTC file header (see
// xtc::XtcHeader). Returns 0-100, or -1 when unavailable (no progress yet / unreadable).
namespace XtcProgress {

inline int percentForBook(const std::string& bookPath) {
  const std::string cachePath = "/.crosspoint/xtc_" + std::to_string(std::hash<std::string>{}(bookPath));
  HalFile progressFile;
  if (!Storage.openFileForRead("XTP", cachePath + "/progress.bin", progressFile)) return -1;
  uint8_t p[4];
  if (progressFile.read(p, 4) != 4) return -1;
  const uint32_t currentPage = p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);

  HalFile xtcFile;
  if (!Storage.openFileForRead("XTP", bookPath, xtcFile)) return -1;
  uint8_t hdr[8];
  if (xtcFile.read(hdr, 8) != 8) return -1;
  const uint16_t pageCount = hdr[6] | (hdr[7] << 8);  // XtcHeader::pageCount at offset 0x06
  if (pageCount == 0) return -1;

  const int pct = static_cast<int>((static_cast<float>(currentPage) / static_cast<float>(pageCount)) * 100.0f + 0.5f);
  return pct < 0 ? 0 : (pct > 100 ? 100 : pct);
}

}  // namespace XtcProgress
