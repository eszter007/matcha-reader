#include "MangaPanel.h"

#include <Arduino.h>
#include <BmpToBmpConverter.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <PngToBmpConverter.h>

#include <algorithm>
#include <cstring>

namespace manga {

static constexpr size_t IDX_HEADER_SIZE = 8;
static constexpr size_t IDX_RECORD_SIZE = 12;
static constexpr size_t PANEL_HEADER_SIZE = 12;  // x,y,w,h,textCount,pad,translationLen
static constexpr size_t TEXT_HEADER_SIZE = 10;

static uint16_t readU16(const uint8_t* p) { return p[0] | (p[1] << 8); }
static uint32_t readU32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24); }

bool isPanelCropFile(const char* name) {
  if (name[0] != 'p' && name[0] != 'P') return false;
  const char* p = name + 1;
  if (!isdigit(static_cast<unsigned char>(*p))) return false;
  while (isdigit(static_cast<unsigned char>(*p))) p++;
  if (*p != '_') return false;
  p++;
  if (!isdigit(static_cast<unsigned char>(*p))) return false;
  while (isdigit(static_cast<unsigned char>(*p))) p++;
  return *p == '.';
}

static bool containsCaseInsensitive(const std::string& haystack, const char* needle) {
  std::string lower = haystack;
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
  return lower.find(needle) != std::string::npos;
}

// Sorts page images the same way tools/mokuro_convert/prepare_panels.py
// orders pages: cover and copyright files are pinned to the very front,
// since their filenames commonly use a distributor product-code prefix
// (not a page sequence number) that natural sort would otherwise push to
// the end. Everything else uses FsHelpers' natural sort.
static void sortPageFileList(std::vector<std::string>& files) {
  std::stable_sort(files.begin(), files.end(), [](const std::string& a, const std::string& b) {
    const bool aCover = containsCaseInsensitive(a, "cover");
    const bool bCover = containsCaseInsensitive(b, "cover");
    if (aCover != bCover) return aCover;
    const bool aCopyright = containsCaseInsensitive(a, "copyright");
    const bool bCopyright = containsCaseInsensitive(b, "copyright");
    if (aCopyright != bCopyright) return aCopyright;
    return false;  // equal priority - resolved by the stable natural sort pass below
  });

  // Partition out the pinned cover/copyright entries (already at the front
  // in priority order via the stable sort above), then natural-sort the rest.
  size_t pinnedCount = 0;
  while (pinnedCount < files.size() && (containsCaseInsensitive(files[pinnedCount], "cover") ||
                                        containsCaseInsensitive(files[pinnedCount], "copyright"))) {
    pinnedCount++;
  }
  std::vector<std::string> rest(files.begin() + static_cast<long>(pinnedCount), files.end());
  FsHelpers::sortFileList(rest);
  std::copy(rest.begin(), rest.end(), files.begin() + static_cast<long>(pinnedCount));
}

bool MangaBook::isMangaFolder(const std::string& folderPath) {
  std::string idxPath = folderPath;
  if (idxPath.back() != '/') idxPath += '/';
  idxPath += "panels.idx";
  return Storage.exists(idxPath.c_str());
}

std::string MangaBook::getTitle() const {
  if (!metaTitle.empty()) return metaTitle;
  auto pos = folderPath.find_last_of('/');
  if (pos != std::string::npos && pos + 1 < folderPath.size()) {
    return folderPath.substr(pos + 1);
  }
  return folderPath;
}

std::string MangaBook::findCoverImage(const std::string& folderPath, BmpConvertCancelFn shouldCancel, void* cancelCtx) {
  static constexpr const char* kCanonicalCovers[] = {"/page_0000.jpg", "/page_0000.bmp", "/page_0000.png"};
  for (const char* suffix : kCanonicalCovers) {
    if (shouldCancel && shouldCancel(cancelCtx)) return "";
    const std::string path = folderPath + suffix;
    if (Storage.exists(path.c_str())) return path;
  }

  auto dir = Storage.open(folderPath.c_str());
  if (!dir || !dir.isDirectory()) return "";
  dir.rewindDirectory();
  std::string firstPageImage;
  std::string firstAnyImage;
  for (auto mf = dir.openNextFile(); mf; mf = dir.openNextFile()) {
    if (shouldCancel && shouldCancel(cancelCtx)) return "";
    char imgName[200];
    mf.getName(imgName, sizeof(imgName));
    if (imgName[0] == '.' || mf.isDirectory()) continue;
    std::string_view imgFn{imgName};
    if (!FsHelpers::hasJpgExtension(imgFn) && !FsHelpers::hasPngExtension(imgFn) &&
        !FsHelpers::hasBmpExtension(imgFn)) {
      continue;
    }
    if (strncmp(imgName, "page_", 5) == 0) {
      if (firstPageImage.empty() || imgFn < firstPageImage) firstPageImage = imgName;
      // page_0000 is the first page by definition -- no reason to keep listing the remaining
      // hundreds of files just to confirm it.
      if (strncmp(imgName, "page_0000", 9) == 0) break;
    } else if (!isPanelCropFile(imgName)) {
      if (firstAnyImage.empty() || imgFn < firstAnyImage) firstAnyImage = imgName;
    }
  }
  dir.close();
  const std::string& chosen = !firstPageImage.empty() ? firstPageImage : firstAnyImage;
  return chosen.empty() ? "" : folderPath + "/" + chosen;
}

std::string MangaBook::getThumbBmpPath() const { return getCachePath() + "/thumb_[HEIGHT].bmp"; }

std::string MangaBook::getThumbBmpPath(int height) const {
  // Keep this stable: users may remove legacy near-black BMP thumbnails manually, after which
  // the corrected BmpToBmpConverter recreates them at this same path. Do not version the filename
  // just to invalidate that historical output; doing so abandons otherwise-valid caches on every
  // device and makes manga behave differently from EPUB/XTC.
  return getCachePath() + "/thumb_" + std::to_string(height) + ".bmp";
}

bool MangaBook::generateThumbBmp(int height, BmpConvertCancelFn shouldCancel, void* cancelCtx) const {
  if (height <= 0) return false;
  const std::string thumbPath = getThumbBmpPath(height);
  if (Storage.exists(thumbPath.c_str())) return true;

  const std::string cover = findCoverImage(folderPath, shouldCancel, cancelCtx);
  if (cover.empty()) return false;
  const bool isJpg = FsHelpers::hasJpgExtension(cover);
  const bool isPng = FsHelpers::hasPngExtension(cover);
  const bool isBmp = FsHelpers::hasBmpExtension(cover);
  if (!isJpg && !isPng && !isBmp) return false;

  Storage.mkdir(getCachePath().c_str());
  HalFile src;
  if (!Storage.openFileForRead("MNG", cover, src)) return false;
  HalFile out;
  if (!Storage.openFileForWrite("MNG", thumbPath, out)) return false;
  // Cover the 2:3 cover box and let the converter trim the overflow -- same box as Epub's, so a
  // manga thumb draws through the same fast packed path instead of a crop at draw time.
  const int targetWidth = (height * 2) / 3;
  bool ok;
  if (isJpg) {
    ok = JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(src, out, targetWidth, height, shouldCancel, cancelCtx);
  } else if (isPng) {
    ok = PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(src, out, targetWidth, height, shouldCancel, cancelCtx);
  } else {
    // Pages that are already bitmaps still need a thumbnail: drawing the full-size page into the
    // cell instead makes a dithered cover come out near-black -- see BmpToBmpConverter.
    ok = BmpToBmpConverter::bmpFileTo1BitBmpStreamWithSize(src, out, targetWidth, height, shouldCancel, cancelCtx);
  }
  // Explicit close() before Storage.remove() on the same path (required despite
  // DESTRUCTOR_CLOSES_FILE); a partial thumb must not survive to masquerade as a cached one.
  src.close();
  out.close();
  if (!ok) {
    Storage.remove(thumbPath.c_str());
    LOG_ERR("MNG", "Thumb generation failed for %s (h=%d)", cover.c_str(), height);
  }
  return ok;
}

std::string MangaBook::getCachePath() const {
  size_t hash = std::hash<std::string>{}(folderPath);
  char buf[64];
  snprintf(buf, sizeof(buf), "/.crosspoint/manga_%zu", hash);
  return std::string(buf);
}

bool MangaBook::load() {
  if (!loadIndex()) return false;
  scanImages();
  loadToc();
  loadMeta();
  return true;
}

bool MangaBook::loadIndex() {
  std::string idxPath = folderPath;
  if (idxPath.back() != '/') idxPath += '/';
  idxPath += "panels.idx";

  HalFile f;
  if (!Storage.openFileForRead("MNG", idxPath, f)) {
    LOG_ERR("MNG", "Cannot open panels.idx: %s", idxPath.c_str());
    return false;
  }

  uint8_t header[IDX_HEADER_SIZE];
  if (f.read(header, IDX_HEADER_SIZE) != static_cast<int>(IDX_HEADER_SIZE)) {
    LOG_ERR("MNG", "Short read on idx header");
    return false;
  }

  uint32_t version = readU32(header);
  if (version != FORMAT_VERSION) {
    LOG_ERR("MNG", "Unsupported panel format version: %u", version);
    return false;
  }

  pageCount = readU32(header + 4);
  if (pageCount == 0 || pageCount > 10000) {
    LOG_ERR("MNG", "Invalid page count: %u", pageCount);
    return false;
  }

  pageIndex.clear();
  pageIndex.reserve(pageCount);

  uint8_t rec[IDX_RECORD_SIZE];
  for (uint32_t i = 0; i < pageCount; i++) {
    if (f.read(rec, IDX_RECORD_SIZE) != static_cast<int>(IDX_RECORD_SIZE)) {
      LOG_ERR("MNG", "Short read on idx record %u", i);
      return false;
    }
    PageInfo pi;
    pi.dataOffset = readU32(rec);
    pi.dataLength = readU32(rec + 4);
    pi.imgWidth = readU16(rec + 8);
    pi.imgHeight = readU16(rec + 10);
    pageIndex.push_back(pi);
  }

  LOG_DBG("MNG", "Loaded panels.idx: %u pages", pageCount);
  return true;
}

// Derives the page list from panels.idx's page count and the converter's canonical
// page_NNNN.<ext> naming, instead of discovering it by walking the directory.
//
// The walk is what made opening a book slow, and its cost is per *directory entry*, not per page
// image -- so anything else sharing the folder is charged for. On the test card that was 974
// panel crops (since moved to panels/) and 1198 macOS AppleDouble ._* sidecars, one per file, put
// there by Finder copying onto FAT and invisible to every filter this scan applies. Deriving the
// names sidesteps all of it: two Storage.exists() probes regardless of how much junk is present.
//
// Only claims the list when both the first and last page resolve, so a folder that is not in the
// canonical layout (hand-assembled image folders, mixed extensions) falls through to the walk
// rather than producing paths that would fail to load later.
bool MangaBook::buildCanonicalPageList() {
  if (pageCount == 0) return false;  // load() calls loadIndex() first, so this is populated

  std::string dirPath = folderPath;
  if (dirPath.empty() || dirPath.back() != '/') dirPath += '/';

  static constexpr const char* kExts[] = {".jpg", ".bmp", ".png"};
  char name[32];
  for (const char* ext : kExts) {
    snprintf(name, sizeof(name), "page_%04u%s", 0u, ext);
    if (!Storage.exists((dirPath + name).c_str())) continue;
    snprintf(name, sizeof(name), "page_%04u%s", static_cast<unsigned>(pageCount - 1), ext);
    if (!Storage.exists((dirPath + name).c_str())) continue;

    imageFiles.clear();
    imageFiles.reserve(pageCount);
    for (uint32_t i = 0; i < pageCount; i++) {
      snprintf(name, sizeof(name), "page_%04u%s", static_cast<unsigned>(i), ext);
      imageFiles.emplace_back(name);
    }
    return true;  // already in page order -- no sort needed
  }
  return false;
}

bool MangaBook::scanImages() {
  imageFiles.clear();

  const unsigned long canonStart = millis();
  if (buildCanonicalPageList()) {
    LOG_DBG("MNG", "Canonical page list: %u pages in %ums (directory not walked)", (unsigned)imageFiles.size(),
            (unsigned)(millis() - canonStart));
    return true;
  }

  std::string dirPath = folderPath;
  auto dir = Storage.open(dirPath.c_str());
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    LOG_ERR("MNG", "Cannot open manga folder: %s", dirPath.c_str());
    return false;
  }

  // This scan dominates opening a manga (6017ms of an 8225ms first loop iteration, measured on
  // device). The per-entry cost is the same slow-directory behaviour that makes a single file open
  // ~85ms, so what decides the fix is how many entries there are versus how many are page images:
  // a converted book carries a p<page>_<panel> crop per panel in the same folder, which would put
  // most of the cost in entries this loop only skips. Counted rather than assumed.
  char name[200];
  uint32_t entriesSeen = 0, cropsSkipped = 0;
  const unsigned long scanStart = millis();
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    entriesSeen++;
    if (!file.isDirectory()) {
      file.getName(name, sizeof(name));
      if (name[0] != '.' && !isPanelCropFile(name)) {
        std::string_view sv(name);
        if (FsHelpers::hasBmpExtension(sv) || FsHelpers::hasJpgExtension(sv) || FsHelpers::hasPngExtension(sv)) {
          imageFiles.emplace_back(name);
        }
      } else if (isPanelCropFile(name)) {
        cropsSkipped++;
      }
    }
    file.close();
  }
  dir.close();
  const unsigned long scanMs = millis() - scanStart;

  sortPageFileList(imageFiles);
  LOG_DBG("MNG", "Found %u page images in %s (%ums, %u dir entries, %u panel crops skipped)",
          (unsigned)imageFiles.size(), dirPath.c_str(), (unsigned)scanMs, (unsigned)entriesSeen,
          (unsigned)cropsSkipped);
  return true;
}

std::string MangaBook::getPageImagePath(uint32_t pageIdx) const {
  if (pageIdx >= imageFiles.size()) return "";
  std::string path = folderPath;
  if (path.back() != '/') path += '/';
  path += imageFiles[pageIdx];
  return path;
}

uint16_t MangaBook::getPageImgWidth(uint32_t pageIdx) const {
  if (pageIdx >= pageIndex.size()) return 0;
  return pageIndex[pageIdx].imgWidth;
}

uint16_t MangaBook::getPageImgHeight(uint32_t pageIdx) const {
  if (pageIdx >= pageIndex.size()) return 0;
  return pageIndex[pageIdx].imgHeight;
}

bool MangaBook::loadPagePanels(uint32_t pageIdx, std::vector<Panel>& panels) const {
  panels.clear();
  if (pageIdx >= pageIndex.size()) return false;

  const auto& pi = pageIndex[pageIdx];
  if (pi.dataLength == 0) return true;

  std::string datPath = folderPath;
  if (datPath.back() != '/') datPath += '/';
  datPath += "panels.dat";

  HalFile f;
  if (!Storage.openFileForRead("MNG", datPath, f)) {
    LOG_ERR("MNG", "Cannot open panels.dat");
    return false;
  }

  if (!f.seekSet(pi.dataOffset)) {
    LOG_ERR("MNG", "Seek failed to offset %u", pi.dataOffset);
    return false;
  }

  if (pi.dataLength > 32768) {
    LOG_ERR("MNG", "Page data too large: %u bytes", pi.dataLength);
    return false;
  }

  auto buf = makeUniqueNoThrow<uint8_t[]>(pi.dataLength);
  if (!buf) {
    LOG_ERR("MNG", "OOM: %u bytes for page data", pi.dataLength);
    return false;
  }

  if (f.read(buf.get(), pi.dataLength) != static_cast<int>(pi.dataLength)) {
    LOG_ERR("MNG", "Short read on page data");
    return false;
  }

  size_t pos = 0;
  if (pos + 2 > pi.dataLength) return false;
  uint8_t panelCount = buf[pos];
  pos += 2;

  panels.reserve(panelCount);

  for (uint8_t p = 0; p < panelCount; p++) {
    if (pos + PANEL_HEADER_SIZE > pi.dataLength) {
      LOG_ERR("MNG", "Panel header overrun at panel %u", p);
      return false;
    }

    Panel panel;
    panel.x = readU16(buf.get() + pos);
    panel.y = readU16(buf.get() + pos + 2);
    panel.w = readU16(buf.get() + pos + 4);
    panel.h = readU16(buf.get() + pos + 6);
    uint8_t textCount = buf[pos + 8];
    uint16_t translationLen = readU16(buf.get() + pos + 10);
    pos += PANEL_HEADER_SIZE;

    if (pos + translationLen > pi.dataLength) {
      LOG_ERR("MNG", "Translation data overrun: need %u bytes", translationLen);
      return false;
    }
    panel.translation.assign(reinterpret_cast<const char*>(buf.get() + pos), translationLen);
    pos += translationLen;

    panel.textBlocks.reserve(textCount);

    for (uint8_t t = 0; t < textCount; t++) {
      if (pos + TEXT_HEADER_SIZE > pi.dataLength) {
        LOG_ERR("MNG", "Text header overrun at text %u", t);
        return false;
      }

      TextBlock tb;
      tb.x = readU16(buf.get() + pos);
      tb.y = readU16(buf.get() + pos + 2);
      tb.w = readU16(buf.get() + pos + 4);
      tb.h = readU16(buf.get() + pos + 6);
      uint16_t textLen = readU16(buf.get() + pos + 8);
      pos += TEXT_HEADER_SIZE;

      if (pos + textLen > pi.dataLength) {
        LOG_ERR("MNG", "Text data overrun: need %u bytes", textLen);
        return false;
      }

      tb.text.assign(reinterpret_cast<const char*>(buf.get() + pos), textLen);
      pos += textLen;
      panel.textBlocks.push_back(std::move(tb));
    }

    panels.push_back(std::move(panel));
  }

  return true;
}

void MangaBook::loadToc() {
  tocEntries.clear();

  std::string tocPath = folderPath;
  if (tocPath.back() != '/') tocPath += '/';
  tocPath += "toc.idx";

  if (!Storage.exists(tocPath.c_str())) return;  // optional file -- most manga don't have one

  HalFile f;
  if (!Storage.openFileForRead("MNG", tocPath, f)) {
    LOG_ERR("MNG", "Cannot open toc.idx");
    return;
  }

  uint8_t header[8];
  if (f.read(header, sizeof(header)) != sizeof(header)) {
    LOG_ERR("MNG", "Short read on toc.idx header");
    return;
  }
  const uint32_t entryCount = readU32(header + 4);
  if (entryCount > 1000) {
    LOG_ERR("MNG", "Implausible toc.idx entry count: %u", entryCount);
    return;
  }

  tocEntries.reserve(entryCount);
  for (uint32_t i = 0; i < entryCount; i++) {
    uint8_t entryHeader[6];
    if (f.read(entryHeader, sizeof(entryHeader)) != sizeof(entryHeader)) {
      LOG_ERR("MNG", "Short read on toc.idx entry %u header", i);
      tocEntries.clear();
      return;
    }
    TocEntry entry;
    entry.pageIndex = readU32(entryHeader);
    const uint16_t titleLen = readU16(entryHeader + 4);
    if (titleLen > 0) {
      auto titleBuf = makeUniqueNoThrow<char[]>(titleLen);
      if (!titleBuf || f.read(reinterpret_cast<uint8_t*>(titleBuf.get()), titleLen) != titleLen) {
        LOG_ERR("MNG", "Short read on toc.idx entry %u title", i);
        tocEntries.clear();
        return;
      }
      entry.title.assign(titleBuf.get(), titleLen);
    }
    tocEntries.push_back(std::move(entry));
  }

  LOG_DBG("MNG", "Loaded toc.idx: %u chapter(s)", static_cast<unsigned>(tocEntries.size()));
}

void MangaBook::loadMeta() {
  metaTitle.clear();
  author.clear();
  language.clear();

  std::string metaPath = folderPath;
  if (metaPath.back() != '/') metaPath += '/';
  metaPath += "meta.bin";

  if (!Storage.exists(metaPath.c_str())) return;

  HalFile f;
  if (!Storage.openFileForRead("MNG", metaPath, f)) return;

  uint8_t hdr[8];
  if (f.read(hdr, 8) != 8) return;

  // uint32 version, uint16 titleLen, uint16 authorLen
  const uint32_t version = readU32(hdr);
  if (version != 1) return;
  const uint16_t titleLen = readU16(hdr + 4);
  const uint16_t authorLen = readU16(hdr + 6);

  if (titleLen > 0) {
    auto buf = makeUniqueNoThrow<char[]>(titleLen);
    if (!buf || f.read(reinterpret_cast<uint8_t*>(buf.get()), titleLen) != titleLen) return;
    metaTitle.assign(buf.get(), titleLen);
  }
  if (authorLen > 0) {
    auto buf = makeUniqueNoThrow<char[]>(authorLen);
    if (!buf || f.read(reinterpret_cast<uint8_t*>(buf.get()), authorLen) != authorLen) return;
    author.assign(buf.get(), authorLen);
  }

  // Optional language trailer (uint16 languageLen + UTF-8 tag), appended after the author
  // without a version bump -- see convert_manga.py's meta.bin format notes. A short read here
  // just means the file predates the trailer, which is the common case; not an error.
  uint8_t langHdr[2];
  if (f.read(langHdr, 2) == 2) {
    const uint16_t languageLen = readU16(langHdr);
    // Language tags are a handful of bytes ("ja", "zh-Hant"); anything longer is a malformed
    // or misaligned file, so ignore it rather than allocate on its say-so.
    char langBuf[16];
    if (languageLen > 0 && languageLen <= sizeof(langBuf) &&
        f.read(reinterpret_cast<uint8_t*>(langBuf), languageLen) == languageLen) {
      language.assign(langBuf, languageLen);
    }
  }

  LOG_DBG("MNG", "Loaded meta.bin: title=%s author=%s lang=%s", metaTitle.c_str(), author.c_str(), language.c_str());
}

}  // namespace manga
