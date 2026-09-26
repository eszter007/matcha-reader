#include "BookProgress.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <MangaPanel.h>
#include <Memory.h>
#include <Txt.h>
#include <Xtc.h>

#include <algorithm>

#include "activities/reader/ProgressFile.h"

namespace {
uint32_t readLe32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

void writeLe32(uint8_t* p, const uint32_t value) {
  p[0] = value & 0xFF;
  p[1] = (value >> 8) & 0xFF;
  p[2] = (value >> 16) & 0xFF;
  p[3] = (value >> 24) & 0xFF;
}
}  // namespace

int mangaProgressPercent(const std::string& path) {
  if (!manga::MangaBook::isMangaFolder(path)) return -1;
  const std::string cachePath = "/.crosspoint/manga_" + std::to_string(std::hash<std::string>{}(path));
  HalFile progressFile;
  uint8_t data[4];
  if (!Storage.openFileForRead("PROG", cachePath + "/progress.bin", progressFile) || progressFile.read(data, 4) != 4) {
    return 0;
  }
  const uint32_t currentPage = readLe32(data);
  HalFile idxFile;
  uint8_t header[8];
  if (!Storage.openFileForRead("PROG", path + "/panels.idx", idxFile) || idxFile.read(header, 8) != 8) return 0;
  const uint32_t totalPages = readLe32(header + 4);
  if (totalPages == 0) return 0;
  const int percent =
      static_cast<int>(static_cast<float>(currentPage) / static_cast<float>(totalPages) * 100.0f + 0.5f);
  return std::clamp(percent, 0, 100);
}

std::string bookCachePath(const std::string& path) {
  const std::string hash = std::to_string(std::hash<std::string>{}(path));
  if (FsHelpers::hasEpubExtension(path)) return "/.crosspoint/epub_" + hash;
  if (FsHelpers::hasXtcExtension(path)) return "/.crosspoint/xtc_" + hash;
  if (FsHelpers::hasTxtExtension(path) || FsHelpers::hasMarkdownExtension(path)) return "/.crosspoint/txt_" + hash;
  if (manga::MangaBook::isMangaFolder(path)) return "/.crosspoint/manga_" + hash;
  return {};
}

bool markBookRead(const std::string& path) {
  const std::string cachePath = bookCachePath(path);
  if (cachePath.empty() || !Storage.exists(cachePath.c_str())) {
    LOG_ERR("PROG", "No cache to mark read: %s", path.c_str());
    return false;
  }

  if (manga::MangaBook::isMangaFolder(path)) {
    // Manga progress is the current page; the last page is the finished state.
    HalFile idxFile;
    uint8_t header[8];
    if (!Storage.openFileForRead("PROG", path + "/panels.idx", idxFile) || idxFile.read(header, 8) != 8) return false;
    const uint32_t totalPages = readLe32(header + 4);
    if (totalPages == 0) return false;
    uint8_t data[4];
    writeLe32(data, totalPages);
    return ProgressFile::writeAtomic(cachePath, data, sizeof(data));
  }

  if (FsHelpers::hasEpubExtension(path)) {
    // Byte 8 is the book percent every reader of this record honours; spine and page are set
    // past the last section so the legacy computation lands on 100 as well.
    auto epub = makeUniqueNoThrow<Epub>(path, "/.crosspoint");
    if (!epub || !epub->load(false, true) || epub->getSpineItemsCount() <= 0) return false;
    const uint16_t spine = static_cast<uint16_t>(epub->getSpineItemsCount());
    uint8_t data[9] = {static_cast<uint8_t>(spine & 0xFF),
                       static_cast<uint8_t>(spine >> 8),
                       0,
                       0,
                       0,
                       0,
                       0xFF,  // vertical override: unset
                       0xFF,  // furigana override: unset
                       100};
    return ProgressFile::writeAtomic(cachePath, data, sizeof(data));
  }

  // XTC and TXT both store a single page counter; their last page is the finished state.
  uint32_t lastPage = 0;
  if (FsHelpers::hasXtcExtension(path)) {
    auto xtc = makeUniqueNoThrow<Xtc>(path, "/.crosspoint");
    if (!xtc || !xtc->load() || xtc->getPageCount() == 0) return false;
    lastPage = xtc->getPageCount();
  } else {
    HalFile index;
    uint8_t header[30];
    if (!Storage.openFileForRead("PROG", cachePath + "/index.bin", index) ||
        index.read(header, sizeof(header)) != sizeof(header))
      return false;
    const uint32_t pages = readLe32(header + 26);
    if (pages == 0) return false;
    lastPage = pages - 1;
  }
  uint8_t data[4];
  writeLe32(data, lastPage);
  return ProgressFile::writeAtomic(cachePath, data, sizeof(data));
}

bool markBookUnread(const std::string& path) {
  const std::string cachePath = bookCachePath(path);
  if (cachePath.empty()) return false;
  const std::string progressPath = cachePath + "/progress.bin";
  // Absent already counts as unread, so a book that was never opened is not an error.
  if (!Storage.exists(progressPath.c_str())) return true;
  return Storage.remove(progressPath.c_str());
}

int loadBookProgress(const std::string& path) {
  uint8_t data[10]{};
  if (FsHelpers::hasEpubExtension(path)) {
    // Metadata objects exceed the stack budget; only the featured book is loaded, once per entry.
    auto epub = makeUniqueNoThrow<Epub>(path, "/.crosspoint");
    if (!epub) {
      LOG_ERR("HOME", "OOM: progress metadata");
      return -1;
    }
    if (!epub->load(false, true)) return -1;
    HalFile file;
    if (!Storage.openFileForRead("HOME", epub->getCachePath() + "/progress.bin", file)) return -1;
    const int size = file.read(data, sizeof(data));
    if (size < 4) return -1;
    // Byte 8 is the reader's own page-based percent (0xFF = unknown). Records that carry it are
    // authoritative -- the computation below is the fallback for the older 4/6-byte layout.
    if (size >= 9 && data[8] <= 100) return data[8];
    const int spine = data[0] | (data[1] << 8);
    const int page = data[2] | (data[3] << 8);
    const int total = size >= 6 ? data[4] | (data[5] << 8) : 0;
    if (epub->getSpineItemsCount() <= 0 || epub->getBookSize() == 0) return -1;
    if (spine == epub->getSpineItemsCount()) return 100;
    if (spine > epub->getSpineItemsCount()) return -1;
    const float fraction = total > 0 && page != UINT16_MAX ? std::clamp(float(page) / total, 0.0f, 1.0f) : 0;
    return std::clamp(static_cast<int>(epub->calculateProgress(spine, fraction) * 100 + 0.5f), 0, 100);
  }
  if (FsHelpers::hasXtcExtension(path)) {
    auto xtc = makeUniqueNoThrow<Xtc>(path, "/.crosspoint");
    if (!xtc) {
      LOG_ERR("HOME", "OOM: XTC progress metadata");
      return -1;
    }
    if (!xtc->load()) return -1;
    HalFile file;
    if (!Storage.openFileForRead("HOME", xtc->getCachePath() + "/progress.bin", file) || file.read(data, 4) != 4)
      return -1;
    const uint32_t page = readLe32(data);
    if (xtc->getPageCount() == 0) return -1;
    if (page >= xtc->getPageCount()) return 100;
    return xtc->calculateProgress(page);
  }
  if (FsHelpers::hasTxtExtension(path) || FsHelpers::hasMarkdownExtension(path)) {
    Txt txt(path, "/.crosspoint");
    HalFile file;
    if (!Storage.openFileForRead("HOME", txt.getCachePath() + "/progress.bin", file) || file.read(data, 4) != 4)
      return -1;
    const uint32_t page = data[0] | (data[1] << 8);
    HalFile index;
    // TXT index v3: magic, version, file size, four layout fields, alignment, page count.
    uint8_t header[30];
    if (!Storage.openFileForRead("HOME", txt.getCachePath() + "/index.bin", index) ||
        index.read(header, sizeof(header)) != sizeof(header))
      return -1;
    if (readLe32(header) != 0x54585449 || header[4] != 3) return -1;
    const uint32_t pages = readLe32(header + 26);
    if (pages == 0 || pages > (index.size() - sizeof(header)) / 4) return -1;
    HalFile source;
    if (!Storage.openFileForRead("HOME", path, source) || source.size() != readLe32(header + 5)) return -1;
    return std::min<int>(100, static_cast<int>((page + 1) * 100ULL / pages));
  }
  return mangaProgressPercent(path);
}
