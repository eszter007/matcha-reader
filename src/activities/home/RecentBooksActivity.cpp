#include "RecentBooksActivity.h"

#include <Bitmap.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>
#include <memory>

#include <Epub.h>
#include <Epub/converters/ImageDecoderFactory.h>
#include <Epub/converters/ImageToFramebufferDecoder.h>
#include <JpegToBmpConverter.h>
#include <MangaPanel.h>
#include <PngToBmpConverter.h>

#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "components/icons/cover.h"
#include "fontIds.h"

namespace {
constexpr unsigned long LONG_PRESS_MS = 1000;
constexpr int COVER_ASPECT_NUM = 2;
constexpr int COVER_ASPECT_DEN = 3;
constexpr int SHELF_THUMB_WIDTH = 36;
constexpr int SHELF_THUMB_HEIGHT = 54;

// Diagnostic artifact written by HalSystem::checkPanic() to the SD root -- a .txt file the
// library walk would otherwise list as a book.
bool isCrashReportFile(const char* name) { return strcmp(name, "crash_report.txt") == 0; }

// Mirrors MangaBook::getCachePath() -- same hash of the folder path, same prefix -- so the
// Library's manga thumbs live alongside the manga's other cached artifacts.
std::string mangaCacheDir(const std::string& mangaFolder) {
  return "/.crosspoint/manga_" + std::to_string(std::hash<std::string>{}(mangaFolder));
}

// Generate (once) 1-bit BMP cover thumbnails for a manga folder from its first page image, at
// the two heights the Library draws: the cover grid / shelf-book rows (metrics.homeCoverHeight)
// and the shelves tab (SHELF_THUMB_HEIGHT). Decoding the raw page JPEG straight to the
// framebuffer cost ~430ms per visible manga cover on EVERY render; the cached BMPs draw in a
// few ms via the same fast path as EPUB covers. Returns the [HEIGHT]-templated thumb path, or
// empty on any failure (caller keeps the raw image path -- the old, slow-but-working behavior).
std::string ensureMangaCoverThumb(const std::string& mangaFolder, const std::string& coverImagePath) {
  const bool isJpg = FsHelpers::hasJpgExtension(coverImagePath);
  const bool isPng = FsHelpers::hasPngExtension(coverImagePath);
  if (!isJpg && !isPng) return "";  // .bmp page images: no converter, keep the raw path

  const std::string cacheDir = mangaCacheDir(mangaFolder);
  const std::string tmpl = cacheDir + "/thumb_[HEIGHT].bmp";
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int gridHeight = metrics.homeCoverHeight > 0 ? metrics.homeCoverHeight : 120;
  const int heights[2] = {gridHeight, SHELF_THUMB_HEIGHT};

  for (const int h : heights) {
    const std::string thumbPath = UITheme::getCoverThumbPath(tmpl, h);
    if (Storage.exists(thumbPath.c_str())) continue;
    Storage.mkdir(cacheDir.c_str());

    HalFile src;
    if (!Storage.openFileForRead("LIB", coverImagePath, src)) return "";
    HalFile out;
    if (!Storage.openFileForWrite("LIB", thumbPath, out)) return "";
    // Manga pages are ~2:3 portrait; a 0.7*h width bound keeps the converter's fit scale driven
    // by the height, same approach as Epub::generateThumbBmp.
    const bool ok = isJpg ? JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(src, out, (h * 7) / 10, h)
                          : PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(src, out, (h * 7) / 10, h);
    // Explicit close() before Storage.remove() on the same path (required despite
    // DESTRUCTOR_CLOSES_FILE); a partial thumb must not survive to masquerade as a cached one.
    out.close();
    if (!ok) {
      Storage.remove(thumbPath.c_str());
      LOG_ERR("LIB", "Manga thumb generation failed for %s (h=%d)", coverImagePath.c_str(), h);
      return "";
    }
  }
  return tmpl;
}
}  // namespace

int RecentBooksActivity::getCellHeight(int cellWidth) const {
  int coverWidth = cellWidth - 2 * COVER_PADDING;
  int coverHeight = coverWidth * COVER_ASPECT_DEN / COVER_ASPECT_NUM;
  int lineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  return COVER_PADDING + coverHeight + CELL_TEXT_GAP + lineHeight + COVER_PADDING;
}

int RecentBooksActivity::getVisibleRows(int cellHeight, int contentHeight) const {
  if (cellHeight <= 0) return 1;
  return std::max(1, contentHeight / cellHeight);
}

int RecentBooksActivity::getContentItemCount() const {
  if (selectedTab == 0) return static_cast<int>(recentBooks.size());
  return static_cast<int>(shelves.size());
}

void RecentBooksActivity::loadRecentBooks() {
  lastRendered.valid = false;  // content changing under the frame -> next render must be full
  // Start with recent books (most recently opened first).
  recentBooks = RECENT_BOOKS.getBooks();

  constexpr size_t NAME_BUF = 500;
  auto nameBuf = makeUniqueNoThrow<char[]>(NAME_BUF);
  if (!nameBuf) return;

  // Iterative (stack-based, not recursive) directory walk so manga/book
  // folders are found at ANY nesting depth -- not just the SD root or one
  // level down. A manga folder (contains panels.idx) is a leaf: its
  // contents are page images, not something to descend into further.
  std::vector<std::string> dirStack;
  dirStack.reserve(16);
  dirStack.push_back("/");

  while (!dirStack.empty()) {
    std::string dirPath = std::move(dirStack.back());
    dirStack.pop_back();

    auto dir = Storage.open(dirPath.c_str());
    if (!dir || !dir.isDirectory()) continue;
    dir.rewindDirectory();

    for (auto f = dir.openNextFile(); f; f = dir.openNextFile()) {
      f.getName(nameBuf.get(), NAME_BUF);
      if (nameBuf[0] == '.') continue;
      if (strcmp(nameBuf.get(), "System Volume Information") == 0) continue;
      if (strcmp(nameBuf.get(), "dict") == 0) continue;

      std::string fullPath = dirPath;
      if (fullPath.back() != '/') fullPath += '/';
      fullPath += nameBuf.get();

      if (f.isDirectory()) {
        // Check if this directory is a manga folder (contains panels.idx)
        std::string idxPath = fullPath + "/panels.idx";
        if (Storage.exists(idxPath.c_str())) {
          // Find existing recents entry -- may exist but with empty cover.
          RecentBook* existing = nullptr;
          for (auto& r : recentBooks) {
            if (r.path == fullPath) { existing = &r; break; }
          }
          if (existing == nullptr) {
            RecentBook newBook;
            newBook.path = fullPath;
            newBook.title = std::string(nameBuf.get());
            recentBooks.push_back(std::move(newBook));
            existing = &recentBooks.back();
          }
          // Runs for new entries, entries stored without a cover, AND entries whose stored cover
          // is a raw page image (the recents store persists the raw path when a manga is opened
          // from the reader) -- rendering a raw page JPEG costs ~430ms per frame, so those must
          // be routed through the cached-thumb path too.
          const bool coverIsRawImage =
              !existing->coverBmpPath.empty() && existing->coverBmpPath.find("[HEIGHT]") == std::string::npos &&
              (FsHelpers::hasJpgExtension(existing->coverBmpPath) || FsHelpers::hasPngExtension(existing->coverBmpPath));
          if (existing->coverBmpPath.empty() || coverIsRawImage) {
            RecentBook& book = *existing;
            const std::string coverBefore = book.coverBmpPath;
            // A previously generated thumb short-circuits everything, including the manga-folder
            // file scan below (hundreds of page files iterated per manga, per Library open).
            const std::string tmpl = mangaCacheDir(fullPath) + "/thumb_[HEIGHT].bmp";
            const auto& metrics = UITheme::getInstance().getMetrics();
            const int gridHeight = metrics.homeCoverHeight > 0 ? metrics.homeCoverHeight : 120;
            if (Storage.exists(UITheme::getCoverThumbPath(tmpl, gridHeight).c_str())) {
              book.coverBmpPath = tmpl;
            } else if (coverIsRawImage) {
              // The stored raw path already tells us the cover source -- no folder scan needed.
              const std::string thumb = ensureMangaCoverThumb(fullPath, book.coverBmpPath);
              if (!thumb.empty()) book.coverBmpPath = thumb;
            } else {
            // Use the first manga PAGE as the cover -- not just the
            // alphabetically-first image file, since panel-zoom crop files
            // (p<page>_<panel>.jpg) sort before page_NNNN.jpg ('0' < 'a')
            // and would otherwise win, showing a cropped panel fragment
            // instead of the actual first page.
            auto mangaDir = Storage.open(fullPath.c_str());
            if (mangaDir && mangaDir.isDirectory()) {
              mangaDir.rewindDirectory();
              std::string firstPageImage;   // page_NNNN.<ext> -- preferred
              std::string firstAnyImage;    // fallback for older/foreign naming
              for (auto mf = mangaDir.openNextFile(); mf; mf = mangaDir.openNextFile()) {
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
                } else if (!manga::isPanelCropFile(imgName)) {
                  if (firstAnyImage.empty() || imgFn < firstAnyImage) firstAnyImage = imgName;
                }
              }
              mangaDir.close();
              const std::string& chosen = !firstPageImage.empty() ? firstPageImage : firstAnyImage;
              if (!chosen.empty()) {
                const std::string sourcePath = fullPath + "/" + chosen;
                const std::string thumb = ensureMangaCoverThumb(fullPath, sourcePath);
                book.coverBmpPath = !thumb.empty() ? thumb : sourcePath;
              }
            }
            }  // close cover scan block
            // Persist a raw-image -> cached-thumb upgrade back to the store so the Home screen's
            // Continue Reading card (which reads the store directly) also stops decoding the raw
            // page JPEG. One write per upgraded entry, ever -- later opens see the template and
            // never enter this branch. No-op for scan-discovered manga not in the store.
            if (coverIsRawImage && book.coverBmpPath != coverBefore) {
              RECENT_BOOKS.updateBook(book.path, book.title, book.author, book.coverBmpPath);
            }
          }  // close coverBmpPath.empty() check
        } else {
          // Not a manga folder itself -- descend into it to find books/manga nested deeper.
          dirStack.push_back(std::move(fullPath));
        }
        continue;
      }

      std::string_view fn{nameBuf.get()};
      if (!FsHelpers::hasEpubExtension(fn) && !FsHelpers::hasXtcExtension(fn) &&
          !FsHelpers::hasTxtExtension(fn) && !FsHelpers::hasMarkdownExtension(fn))
        continue;
      if (isCrashReportFile(nameBuf.get())) continue;

      bool alreadyInRecents = false;
      for (const auto& r : recentBooks) {
        if (r.path == fullPath) { alreadyInRecents = true; break; }
      }
      if (alreadyInRecents) continue;
      RecentBook book;
      book.path = std::move(fullPath);
      auto dot = fn.find_last_of('.');
      book.title = std::string(dot != std::string_view::npos ? fn.substr(0, dot) : fn);
      recentBooks.push_back(std::move(book));
    }
    dir.close();
  }

  // Generate cover thumbnails for books that don't have one yet.
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int thumbH = metrics.homeCoverHeight > 0 ? metrics.homeCoverHeight : 120;
  for (auto& book : recentBooks) {
    if (!book.coverBmpPath.empty()) continue;
    if (!FsHelpers::hasEpubExtension(book.path)) continue;
    std::string cachePath = "/.crosspoint/epub_" + std::to_string(std::hash<std::string>{}(book.path));
    std::string thumbPath = cachePath + "/thumb_" + std::to_string(thumbH) + ".bmp";
    if (Storage.exists(thumbPath.c_str())) {
      book.coverBmpPath = cachePath + "/thumb_[HEIGHT].bmp";
      continue;
    }
    Epub epub(book.path, "/.crosspoint");
    if (epub.load(true, true) && epub.generateThumbBmp(thumbH)) {
      book.coverBmpPath = cachePath + "/thumb_[HEIGHT].bmp";
      const auto& title = epub.getTitle();
      if (!title.empty()) book.title = title;
    }
  }
}

void RecentBooksActivity::loadBookProgress() {
  bookProgress.clear();
  bookProgress.reserve(recentBooks.size());
  for (const auto& book : recentBooks) {
    BookProgress bp;
    bp.percent = readProgressPercent(book.path);
    bookProgress.push_back(bp);
  }
}

void RecentBooksActivity::loadShelves() {
  shelves.clear();

  for (const auto& book : recentBooks) {
    std::string folder = FsHelpers::extractFolderPath(book.path);
    size_t lastSlash = folder.find_last_of('/');
    std::string name = (lastSlash != std::string::npos && lastSlash < folder.size() - 1)
                           ? folder.substr(lastSlash + 1)
                           : folder;
    if (folder == "/") name = "Unsorted";

    bool found = false;
    for (auto& shelf : shelves) {
      if (shelf.folderPath == folder) {
        if (shelf.coverBmpPath.empty() && !book.coverBmpPath.empty()) {
          shelf.coverBmpPath = book.coverBmpPath;
          shelf.coverBookPath = book.path;
        }
        found = true;
        break;
      }
    }

    if (!found) {
      ShelfInfo shelf;
      shelf.folderPath = folder;
      shelf.folderName = name;
      shelf.coverBmpPath = book.coverBmpPath;
      if (!book.coverBmpPath.empty()) shelf.coverBookPath = book.path;
      shelf.bookCount = 0;
      shelves.push_back(std::move(shelf));
    }
  }

  constexpr size_t COUNT_BUF_SIZE = 200;
  char countBuf[COUNT_BUF_SIZE];
  for (auto& shelf : shelves) {
    auto root = Storage.open(shelf.folderPath.c_str());
    if (!root || !root.isDirectory()) continue;
    root.rewindDirectory();
    int count = 0;
    for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
      file.getName(countBuf, COUNT_BUF_SIZE);
      if (countBuf[0] == '.') continue;
      if (file.isDirectory()) {
        // Count manga folders (containing panels.idx)
        std::string subDir = shelf.folderPath;
        if (subDir.back() != '/') subDir += '/';
        subDir += countBuf;
        std::string idxCheck = subDir + "/panels.idx";
        if (Storage.exists(idxCheck.c_str())) count++;
        continue;
      }
      std::string_view fn{countBuf};
      if (isCrashReportFile(countBuf)) continue;
      if (FsHelpers::hasEpubExtension(fn) || FsHelpers::hasXtcExtension(fn) || FsHelpers::hasTxtExtension(fn)) {
        count++;
      }
    }
    root.close();
    shelf.bookCount = count;
  }

  std::sort(shelves.begin(), shelves.end(),
            [](const ShelfInfo& a, const ShelfInfo& b) { return a.folderName < b.folderName; });

  // Resolve a shelf-height thumbnail for each shelf's cover so it renders 1:1
  // (the bitmap downscaler produces all-black at heavy reductions).
  for (auto& shelf : shelves) {
    // Any [HEIGHT]-templated cover (EPUB or manga) may already have its 54px variant on SD --
    // resolve it with a pure existence check. Constructing and load()ing an Epub per shelf just
    // to have generateThumbBmp() discover the file exists cost several hundred ms PER SHELF on
    // every Library open (measured as the bulk of an 11s open, together with one first-time
    // thumb generation).
    if (!shelf.coverBmpPath.empty() && shelf.coverBmpPath.find("[HEIGHT]") != std::string::npos) {
      const std::string p = UITheme::getCoverThumbPath(shelf.coverBmpPath, SHELF_THUMB_HEIGHT);
      if (Storage.exists(p.c_str())) {
        shelf.shelfThumbPath = p;
        continue;
      }
    }
    if (shelf.coverBookPath.empty() || !FsHelpers::hasEpubExtension(shelf.coverBookPath)) continue;
    Epub epub(shelf.coverBookPath, "/.crosspoint");
    if (epub.load(true, true) && epub.generateThumbBmp(SHELF_THUMB_HEIGHT)) {
      shelf.shelfThumbPath = epub.getThumbBmpPath(SHELF_THUMB_HEIGHT);
    }
  }
}

void RecentBooksActivity::loadShelfBooks(const std::string& folderPath) {
  lastRendered.valid = false;  // content changing under the frame -> next render must be full
  shelfBooks.clear();
  shelfBookProgress.clear();

  constexpr size_t NAME_BUF_SIZE = 500;
  auto nameBuffer = makeUniqueNoThrow<char[]>(NAME_BUF_SIZE);
  if (!nameBuffer) {
    LOG_ERR("LIB", "OOM: name buffer");
    return;
  }

  auto root = Storage.open(folderPath.c_str());
  if (!root || !root.isDirectory()) return;
  root.rewindDirectory();

  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(nameBuffer.get(), NAME_BUF_SIZE);
    if (nameBuffer[0] == '.') continue;

    std::string fullPath = folderPath == "/" ? "/" + std::string(nameBuffer.get())
                                             : folderPath + "/" + std::string(nameBuffer.get());

    if (file.isDirectory()) {
      // Manga folders (containing panels.idx) count as books within a shelf.
      std::string idxPath = fullPath + "/panels.idx";
      if (!Storage.exists(idxPath.c_str())) continue;

      ShelfBook book;
      book.path = fullPath;
      book.title = std::string(nameBuffer.get());

      for (const auto& recent : recentBooks) {
        if (recent.path == book.path) {
          if (!recent.title.empty()) book.title = recent.title;
          book.coverBmpPath = recent.coverBmpPath;
          break;
        }
      }
      if (book.coverBmpPath.empty()) {
        // A previously generated thumb short-circuits the folder scan (same as loadRecentBooks).
        const std::string tmpl = mangaCacheDir(fullPath) + "/thumb_[HEIGHT].bmp";
        const auto& metrics = UITheme::getInstance().getMetrics();
        const int gridHeight = metrics.homeCoverHeight > 0 ? metrics.homeCoverHeight : 120;
        if (Storage.exists(UITheme::getCoverThumbPath(tmpl, gridHeight).c_str())) {
          book.coverBmpPath = tmpl;
        } else {
        // Same first-page selection as loadRecentBooks() -- skip panel crops.
        auto mangaDir = Storage.open(fullPath.c_str());
        if (mangaDir && mangaDir.isDirectory()) {
          mangaDir.rewindDirectory();
          std::string firstPageImage, firstAnyImage;
          for (auto mf = mangaDir.openNextFile(); mf; mf = mangaDir.openNextFile()) {
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
            } else if (!manga::isPanelCropFile(imgName)) {
              if (firstAnyImage.empty() || imgFn < firstAnyImage) firstAnyImage = imgName;
            }
          }
          mangaDir.close();
          const std::string& chosen = !firstPageImage.empty() ? firstPageImage : firstAnyImage;
          if (!chosen.empty()) {
            const std::string sourcePath = fullPath + "/" + chosen;
            const std::string thumb = ensureMangaCoverThumb(fullPath, sourcePath);
            book.coverBmpPath = !thumb.empty() ? thumb : sourcePath;
          }
        }
        }
      }

      shelfBooks.push_back(std::move(book));
      continue;
    }

    std::string_view filename{nameBuffer.get()};
    if (!FsHelpers::hasEpubExtension(filename) && !FsHelpers::hasXtcExtension(filename) &&
        !FsHelpers::hasTxtExtension(filename))
      continue;
    if (isCrashReportFile(nameBuffer.get())) continue;

    ShelfBook book;
    book.path = fullPath;

    auto dotPos = filename.find_last_of('.');
    book.title = std::string(dotPos != std::string_view::npos ? filename.substr(0, dotPos) : filename);

    for (const auto& recent : recentBooks) {
      if (recent.path == book.path) {
        if (!recent.title.empty()) book.title = recent.title;
        book.coverBmpPath = recent.coverBmpPath;
        break;
      }
    }

    if (book.coverBmpPath.empty()) {
      std::string cachePath;
      if (FsHelpers::hasEpubExtension(filename)) {
        cachePath = "/.crosspoint/epub_" + std::to_string(std::hash<std::string>{}(book.path));
      } else if (FsHelpers::hasXtcExtension(filename)) {
        cachePath = "/.crosspoint/xtc_" + std::to_string(std::hash<std::string>{}(book.path));
      }
      if (!cachePath.empty()) {
        const auto& metrics = UITheme::getInstance().getMetrics();
        std::string thumbPath = cachePath + "/thumb_" + std::to_string(metrics.homeCoverHeight) + ".bmp";
        if (Storage.exists(thumbPath.c_str())) {
          book.coverBmpPath = cachePath + "/thumb_[HEIGHT].bmp";
        }
      }
    }

    shelfBooks.push_back(std::move(book));
  }
  root.close();

  std::sort(shelfBooks.begin(), shelfBooks.end(),
            [](const ShelfBook& a, const ShelfBook& b) { return a.title < b.title; });

  shelfBookProgress.reserve(shelfBooks.size());
  for (const auto& book : shelfBooks) {
    BookProgress bp;
    bp.percent = readProgressPercent(book.path);
    shelfBookProgress.push_back(bp);
  }
}

int RecentBooksActivity::readProgressPercent(const std::string& bookPath) const {
  std::string cachePath;
  if (FsHelpers::hasEpubExtension(bookPath)) {
    cachePath = "/.crosspoint/epub_" + std::to_string(std::hash<std::string>{}(bookPath));
  } else if (FsHelpers::hasXtcExtension(bookPath)) {
    cachePath = "/.crosspoint/xtc_" + std::to_string(std::hash<std::string>{}(bookPath));
  } else if (manga::MangaBook::isMangaFolder(bookPath)) {
    std::string mangaCachePath = "/.crosspoint/manga_" + std::to_string(std::hash<std::string>{}(bookPath));
    HalFile f;
    if (!Storage.openFileForRead("LIB", mangaCachePath + "/progress.bin", f)) return 0;
    uint8_t data[4];
    if (f.read(data, 4) != 4) return 0;
    uint32_t currentPage = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
    HalFile idxFile;
    if (!Storage.openFileForRead("LIB", bookPath + "/panels.idx", idxFile)) return 0;
    uint8_t hdr[8];
    if (idxFile.read(hdr, 8) != 8) return 0;
    uint32_t totalPages = hdr[4] | (hdr[5] << 8) | (hdr[6] << 16) | (hdr[7] << 24);
    if (totalPages == 0) return 0;
    int pct = static_cast<int>((static_cast<float>(currentPage) / static_cast<float>(totalPages)) * 100.0f + 0.5f);
    return std::clamp(pct, 0, 100);
  } else {
    return -1;
  }

  HalFile f;
  if (!Storage.openFileForRead("LIB", cachePath + "/progress.bin", f)) {
    return 0;
  }

  if (FsHelpers::hasEpubExtension(bookPath)) {
    uint8_t data[6];
    if (f.read(data, 6) != 6) return 0;
    uint16_t spineIndex = data[0] | (data[1] << 8);
    uint16_t currentPage = data[2] | (data[3] << 8);
    uint16_t pageCount = data[4] | (data[5] << 8);
    if (pageCount == 0) return 0;

    HalFile bookFile;
    if (Storage.openFileForRead("LIB", cachePath + "/book.bin", bookFile)) {
      uint8_t version;
      uint32_t lutOffset;
      uint16_t spineCount;
      if (bookFile.read(&version, 1) == 1) {
        bookFile.read(reinterpret_cast<uint8_t*>(&lutOffset), 4);
        bookFile.read(reinterpret_cast<uint8_t*>(&spineCount), 2);
        if (spineCount > 0) {
          float sectionProgress = static_cast<float>(currentPage) / static_cast<float>(pageCount);
          float overallProgress = (static_cast<float>(spineIndex) + sectionProgress) / static_cast<float>(spineCount);
          int pct = static_cast<int>(overallProgress * 100.0f + 0.5f);
          return std::clamp(pct, 0, 100);
        }
      }
    }

    int pct = static_cast<int>((static_cast<float>(currentPage) / static_cast<float>(pageCount)) * 100.0f + 0.5f);
    return std::clamp(pct, 0, 100);
  }

  return -1;
}

void RecentBooksActivity::onEnter() {
  Activity::onEnter();

  if (RECENT_BOOKS.pruneMissing()) {
    RECENT_BOOKS.saveToFile();
  }

  loadRecentBooks();
  loadBookProgress();
  loadShelves();

  selectedTab = 0;
  contentIndex = 0;
  scrollRow = 0;
  openShelfIndex = -1;
  requestUpdate();
}

void RecentBooksActivity::onExit() {
  Activity::onExit();
  recentBooks.clear();
  bookProgress.clear();
  shelves.clear();
  shelfBooks.clear();
  shelfBookProgress.clear();
}

void RecentBooksActivity::loop() {
  if (openShelfIndex >= 0) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      openShelfIndex = -1;
      shelfBooks.clear();
      shelfBookProgress.clear();
      requestUpdate();
      return;
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (shelfContentIndex < static_cast<int>(shelfBooks.size())) {
        LOG_DBG("RBA", "Selected shelf book: %s", shelfBooks[shelfContentIndex].path.c_str());
        onSelectBook(shelfBooks[shelfContentIndex].path);
        return;
      }
    }

    const int shelfItemCount = static_cast<int>(shelfBooks.size());
    if (shelfItemCount > 0) {
      buttonNavigator.onNextRelease([this, shelfItemCount] {
        shelfContentIndex = ButtonNavigator::nextIndex(shelfContentIndex, shelfItemCount);
        requestUpdate();
      });
      buttonNavigator.onPreviousRelease([this, shelfItemCount] {
        shelfContentIndex = ButtonNavigator::previousIndex(shelfContentIndex, shelfItemCount);
        requestUpdate();
      });
    }
    return;
  }

  bool hasChangedTab = false;

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (contentIndex == 0) {
      selectedTab = (selectedTab + 1) % TAB_COUNT;
      hasChangedTab = true;
      requestUpdate();
    } else {
      const int itemIdx = contentIndex - 1;
      if (selectedTab == 0) {
        if (itemIdx < static_cast<int>(recentBooks.size())) {
          LOG_DBG("RBA", "Selected recent book: %s", recentBooks[itemIdx].path.c_str());
          onSelectBook(recentBooks[itemIdx].path);
          return;
        }
      } else {
        if (itemIdx < static_cast<int>(shelves.size())) {
          LOG_DBG("RBA", "Opening shelf: %s", shelves[itemIdx].folderPath.c_str());
          openShelfIndex = itemIdx;
          shelfContentIndex = 0;
          shelfScrollRow = 0;
          loadShelfBooks(shelves[itemIdx].folderPath);
          requestUpdate();
          return;
        }
      }
    }
  }

  if (longPressFired) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      longPressFired = false;
    }
    return;
  }

  if (selectedTab == 0 && contentIndex > 0) {
    const int itemIdx = contentIndex - 1;
    if (itemIdx < static_cast<int>(recentBooks.size()) &&
        mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= LONG_PRESS_MS) {
      longPressFired = true;
      promptRemoveBook(recentBooks[itemIdx].path, recentBooks[itemIdx].title);
      return;
    }
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (contentIndex > 0) {
      contentIndex = 0;
      scrollRow = 0;
      requestUpdate();
    } else {
      onGoHome();
    }
    return;
  }

  const int totalItems = getContentItemCount() + 1;

  buttonNavigator.onNextRelease([this, totalItems] {
    contentIndex = ButtonNavigator::nextIndex(contentIndex, totalItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this, totalItems] {
    contentIndex = ButtonNavigator::previousIndex(contentIndex, totalItems);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, &hasChangedTab] {
    hasChangedTab = true;
    selectedTab = ButtonNavigator::nextIndex(selectedTab, TAB_COUNT);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, &hasChangedTab] {
    hasChangedTab = true;
    selectedTab = ButtonNavigator::previousIndex(selectedTab, TAB_COUNT);
    requestUpdate();
  });

  if (hasChangedTab) {
    contentIndex = (contentIndex == 0) ? 0 : 1;
    scrollRow = 0;
  }
}

void RecentBooksActivity::promptRemoveBook(const std::string& path, const std::string& title) {
  auto handler = [this, path](const ActivityResult& res) {
    // The confirmation dialog painted over our frame (whether confirmed or cancelled), so the
    // next render must be a full one -- a partial redraw would leave dialog remnants on screen.
    lastRendered.valid = false;
    if (res.isCancelled) {
      LOG_DBG("RBA", "Remove from recents cancelled");
      return;
    }
    if (RECENT_BOOKS.removeByPath(path)) {
      LOG_DBG("RBA", "Removed from recents: %s", path.c_str());
      loadRecentBooks();
      loadBookProgress();
      loadShelves();
      if (recentBooks.empty()) {
        contentIndex = 0;
      } else if (contentIndex - 1 >= static_cast<int>(recentBooks.size())) {
        contentIndex = static_cast<int>(recentBooks.size());
      }
      requestUpdate(true);
    }
  };

  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_REMOVE_FROM_RECENTS), title),
      std::move(handler));
}

void RecentBooksActivity::drawGridSelectionBorder(const int cellX, const int cellY, const int cellWidth,
                                                  const int cellHeight, const bool on) {
  (void)cellHeight;
  const int coverWidth = cellWidth - 2 * COVER_PADDING;
  const int coverHeight = coverWidth * COVER_ASPECT_DEN / COVER_ASPECT_NUM;
  const int coverX = cellX + COVER_PADDING;
  const int coverY = cellY + COVER_PADDING;
  // Two nested 1px rects = a 2px ring at offsets -2/-3 from the cover box: a 1px white gap
  // separates it from the cover's own 1px outline, and it stays clear of the progress label
  // below (labelY = cover bottom + CELL_TEXT_GAP).
  renderer.drawRect(coverX - 2, coverY - 2, coverWidth + 4, coverHeight + 4, on);
  renderer.drawRect(coverX - 3, coverY - 3, coverWidth + 6, coverHeight + 6, on);
}

void RecentBooksActivity::drawGridCell(const int cellX, const int cellY, const int cellWidth, const int cellHeight,
                                       const std::string& coverBmpPath, const std::string& title,
                                       const int progressPercent, const bool selected) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int coverWidth = cellWidth - 2 * COVER_PADDING;
  const int coverHeight = coverWidth * COVER_ASPECT_DEN / COVER_ASPECT_NUM;
  const int thumbHeight = metrics.homeCoverHeight;
  const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int coverX = cellX + COVER_PADDING;
  const int coverY = cellY + COVER_PADDING;

  if (selected) {
    drawGridSelectionBorder(cellX, cellY, cellWidth, cellHeight, true);
  }

  bool hasCover = false;
  if (!coverBmpPath.empty()) {
    const std::string coverPath = UITheme::getCoverThumbPath(coverBmpPath, thumbHeight);
    if (FsHelpers::hasJpgExtension(coverPath) || FsHelpers::hasPngExtension(coverPath)) {
      ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(coverPath);
      if (decoder) {
        ImageDimensions dims = {0, 0};
        if (decoder->getDimensions(coverPath, dims) && dims.width > 0 && dims.height > 0) {
          RenderConfig config;
          config.x = coverX;
          config.y = coverY;
          config.maxWidth = coverWidth;
          config.maxHeight = coverHeight;
          config.useGrayscale = false;
          config.useDithering = true;
          // Manga covers are raw page images (not pre-cropped like Epub
          // BMP thumbnails below) -- fill the cell and crop the overflow
          // (bottom-cropped, top stays put) instead of letterboxing.
          config.fillCrop = true;
          config.cropWidth = coverWidth;
          config.cropHeight = coverHeight;
          if (decoder->decodeToFramebuffer(coverPath, renderer, config)) {
            hasCover = true;
          }
        }
      }
    } else {
      HalFile file;
      if (Storage.openFileForRead("LIB", coverPath, file)) {
        Bitmap bitmap(file);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          const float bmpRatio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
          const float cellRatio = static_cast<float>(coverWidth) / static_cast<float>(coverHeight);
          float cropX = 0.0f, cropY = 0.0f;
          if (bmpRatio > cellRatio) {
            cropX = 1.0f - (cellRatio / bmpRatio);
          } else {
            cropY = 1.0f - (bmpRatio / cellRatio);
          }
          renderer.drawBitmap(bitmap, coverX, coverY, coverWidth, coverHeight, cropX, cropY);
          hasCover = true;
        }
        file.close();
      }
    }
  }

  renderer.drawRect(coverX, coverY, coverWidth, coverHeight, true);

  if (!hasCover) {
    renderer.drawIcon(CoverIcon, coverX + (coverWidth - 32) / 2, coverY + (coverHeight - 32) / 2, 32, 32);
    auto titleLines = renderer.wrappedText(SMALL_FONT_ID, title.c_str(), coverWidth - 8, 3);
    int textY = coverY + (coverHeight - 32) / 2 + 36;
    for (const auto& line : titleLines) {
      if (textY + lineHeight > coverY + coverHeight) break;
      const int textW = renderer.getTextWidth(SMALL_FONT_ID, line.c_str());
      const int textX = coverX + (coverWidth - textW) / 2;
      renderer.drawText(SMALL_FONT_ID, textX, textY, line.c_str(), true);
      textY += lineHeight;
    }
  }

  const int labelY = coverY + coverHeight + CELL_TEXT_GAP;
  if (progressPercent >= 0) {
    char pctBuf[8];
    snprintf(pctBuf, sizeof(pctBuf), "%d%%", progressPercent);
    const int pctW = renderer.getTextWidth(SMALL_FONT_ID, pctBuf);
    renderer.drawText(SMALL_FONT_ID, coverX + (coverWidth - pctW) / 2, labelY, pctBuf, true);
  }
}

void RecentBooksActivity::renderBooksTab(int contentTop, int contentHeight) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  if (recentBooks.empty()) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, tr(STR_NO_RECENT_BOOKS));
    return;
  }

  const int gridWidth = pageWidth - 2 * metrics.contentSidePadding;
  const int cellWidth = gridWidth / GRID_COLS;
  const int coverWidth = cellWidth - 2 * COVER_PADDING;
  const int coverHeight = coverWidth * COVER_ASPECT_DEN / COVER_ASPECT_NUM;
  const int thumbHeight = metrics.homeCoverHeight;
  const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int cellHeight = getCellHeight(cellWidth);
  const int visibleRows = getVisibleRows(cellHeight, contentHeight);
  const int totalRows = (static_cast<int>(recentBooks.size()) + GRID_COLS - 1) / GRID_COLS;
  const int selectedItem = contentIndex - 1;

  int selectedRow = selectedItem >= 0 ? selectedItem / GRID_COLS : 0;
  if (selectedRow < scrollRow) scrollRow = selectedRow;
  if (selectedRow >= scrollRow + visibleRows) scrollRow = selectedRow - visibleRows + 1;

  // Render one extra row beyond the visible area so the next row peeks
  // behind the button bar, hinting that more content is available.
  const int renderRows = visibleRows + (totalRows > visibleRows ? 1 : 0);

  // Prewarm titles for visible cells (drawn as a fallback when a cell has no cover thumbnail) --
  // see the matching comment in renderShelvesTab for why this matters for non-Latin text.
  if (auto* fcm = renderer.getFontCacheManager()) {
    std::string prewarmBuf;
    prewarmBuf.reserve(256);
    for (int row = scrollRow; row < std::min(scrollRow + renderRows, totalRows); row++) {
      for (int col = 0; col < GRID_COLS; col++) {
        const int idx = row * GRID_COLS + col;
        if (idx >= static_cast<int>(recentBooks.size())) break;
        prewarmBuf += recentBooks[idx].title;
        prewarmBuf += ' ';
      }
    }
    fcm->prewarmCache(SMALL_FONT_ID, prewarmBuf.c_str(), 1 << EpdFontFamily::REGULAR);
  }

  for (int row = scrollRow; row < std::min(scrollRow + renderRows, totalRows); row++) {
    for (int col = 0; col < GRID_COLS; col++) {
      const int idx = row * GRID_COLS + col;
      if (idx >= static_cast<int>(recentBooks.size())) break;
      const int cellX = metrics.contentSidePadding + col * cellWidth;
      const int cellY = contentTop + (row - scrollRow) * cellHeight;
      const int pct = idx < static_cast<int>(bookProgress.size()) ? bookProgress[idx].percent : -1;
      drawGridCell(cellX, cellY, cellWidth, cellHeight, recentBooks[idx].coverBmpPath, recentBooks[idx].title, pct,
                   idx == selectedItem);
    }
  }

  // Release the page slots claimed by the prewarm above -- see the matching comment in
  // LyraTheme::drawRecentBookCover for why this is required (FontDecompressor's page-slot pool
  // is small and shared globally; an unreleased prewarm permanently starves every other screen).
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->clearCache();
  }
}

void RecentBooksActivity::drawShelfRow(const int shelfIdx, const int itemY, const bool selected) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const int rowHeight = metrics.listWithSubtitleRowHeight;
  const int chevronMargin = 15;
  const auto& shelf = shelves[shelfIdx];

  if (selected) {
    renderer.fillRoundedRect(metrics.contentSidePadding - 6, itemY - 4,
                             pageWidth - 2 * metrics.contentSidePadding + 12, rowHeight + 8, SELECTION_RADIUS,
                             Color::LightGray);
  }

  int thumbX = metrics.contentSidePadding + 4;
  int thumbY = itemY + (rowHeight - SHELF_THUMB_HEIGHT) / 2;

  bool hasThumb = false;
  // The shelf-height thumbnail (generated in loadShelves) renders 1:1, avoiding
  // the bitmap downscaler that produces all-black at heavy reductions.
  if (!shelf.shelfThumbPath.empty()) {
    HalFile file;
    if (Storage.openFileForRead("LIB", shelf.shelfThumbPath, file)) {
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        // Center the (already small) thumb in the slot; no scaling needed.
        const int bw = bitmap.getWidth();
        const int bh = bitmap.getHeight();
        const int dx = thumbX + (SHELF_THUMB_WIDTH - bw) / 2;
        const int dy = thumbY + (SHELF_THUMB_HEIGHT - bh) / 2;
        renderer.drawBitmap(bitmap, dx, dy, bw, bh);
        hasThumb = true;
      }
      file.close();
    }
  } else if (!shelf.coverBmpPath.empty() && (FsHelpers::hasJpgExtension(shelf.coverBmpPath) ||
                                             FsHelpers::hasPngExtension(shelf.coverBmpPath))) {
    // Manga covers are JPG/PNG page images (no pre-rendered shelf thumb) --
    // decode and scale directly into the thumb slot.
    ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(shelf.coverBmpPath);
    if (decoder) {
      ImageDimensions dims = {0, 0};
      if (decoder->getDimensions(shelf.coverBmpPath, dims) && dims.width > 0 && dims.height > 0) {
        const int drawW = SHELF_THUMB_HEIGHT * dims.width / dims.height;
        RenderConfig config;
        config.x = thumbX + (SHELF_THUMB_WIDTH - drawW) / 2;
        config.y = thumbY;
        config.maxWidth = drawW;
        config.maxHeight = SHELF_THUMB_HEIGHT;
        config.useGrayscale = false;
        config.useDithering = true;
        if (decoder->decodeToFramebuffer(shelf.coverBmpPath, renderer, config)) {
          hasThumb = true;
        }
      }
    }
  }

  if (!hasThumb) {
    renderer.drawRect(thumbX, thumbY, SHELF_THUMB_WIDTH, SHELF_THUMB_HEIGHT, true);
  }

  const int textX = thumbX + SHELF_THUMB_WIDTH + 12;
  const int smallLH = renderer.getLineHeight(SMALL_FONT_ID);
  const int nameY = itemY + rowHeight / 2 - smallLH;

  renderer.drawText(UI_10_FONT_ID, textX, nameY, shelf.folderName.c_str(), true, EpdFontFamily::BOLD);

  char countBuf[32];
  if (shelf.bookCount == 1) {
    snprintf(countBuf, sizeof(countBuf), "%s", tr(STR_SHELF_BOOK_COUNT_1));
  } else {
    snprintf(countBuf, sizeof(countBuf), tr(STR_SHELF_BOOK_COUNT_N), shelf.bookCount);
  }
  renderer.drawText(SMALL_FONT_ID, textX, nameY + smallLH + 1, countBuf, true);

  const int chevronSize = 6;
  const int chevronX = pageWidth - metrics.contentSidePadding - chevronMargin;
  const int chevronY = itemY + rowHeight / 2;
  renderer.drawLine(chevronX, chevronY - chevronSize, chevronX + chevronSize, chevronY, true);
  renderer.drawLine(chevronX + chevronSize, chevronY, chevronX, chevronY + chevronSize, true);
  renderer.drawLine(chevronX + 1, chevronY - chevronSize, chevronX + chevronSize + 1, chevronY, true);
  renderer.drawLine(chevronX + chevronSize + 1, chevronY, chevronX + 1, chevronY + chevronSize, true);
}

void RecentBooksActivity::renderShelvesTab(int contentTop, int contentHeight) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  if (shelves.empty()) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, tr(STR_NO_SHELVES));
    return;
  }

  const int rowHeight = metrics.listWithSubtitleRowHeight;
  const int visibleItems = std::max(1, contentHeight / rowHeight);
  const int selectedItem = contentIndex - 1;
  const int shelfCount = static_cast<int>(shelves.size());

  int scrollOffset = 0;
  if (selectedItem >= visibleItems) {
    scrollOffset = selectedItem - visibleItems + 1;
  }

  const int thumbH = metrics.homeCoverHeight;
  const int chevronMargin = 15;

  // Prewarm the font cache with all visible folder names before drawing. Folder names are drawn
  // unconditionally on every render (unlike cover-fallback titles below), so without this, non-Latin
  // names (e.g. Japanese folders) hit FontDecompressor's slow single-slot fallback on every navigation
  // step -- one full ~64KB group decompression per name that lands in a different glyph group.
  if (auto* fcm = renderer.getFontCacheManager()) {
    std::string prewarmBuf;
    prewarmBuf.reserve(256);
    for (int i = scrollOffset; i < std::min(scrollOffset + visibleItems, shelfCount); i++) {
      prewarmBuf += shelves[i].folderName;
      prewarmBuf += ' ';
    }
    fcm->prewarmCache(UI_10_FONT_ID, prewarmBuf.c_str(), 1 << EpdFontFamily::BOLD);
  }

  for (int i = scrollOffset; i < std::min(scrollOffset + visibleItems, shelfCount); i++) {
    const int itemY = contentTop + (i - scrollOffset) * rowHeight;
    drawShelfRow(i, itemY, i == selectedItem);
  }

  if (shelfCount > visibleItems) {
    const int barX = pageWidth - metrics.scrollBarRightOffset - metrics.scrollBarWidth;
    const int thumbBarH = std::max(10, contentHeight * visibleItems / shelfCount);
    const int thumbBarY = contentTop + (contentHeight - thumbBarH) * scrollOffset / (shelfCount - visibleItems);
    renderer.fillRect(barX, thumbBarY, metrics.scrollBarWidth, thumbBarH, true);
  }

  // Release the page slots claimed by the prewarm above -- see the matching comment in
  // LyraTheme::drawRecentBookCover for why this is required.
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->clearCache();
  }
}

void RecentBooksActivity::renderShelfBooksView(int contentTop, int contentHeight) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  if (shelfBooks.empty()) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, tr(STR_NO_FILES_FOUND));
    return;
  }

  const int gridWidth = pageWidth - 2 * metrics.contentSidePadding;
  const int cellWidth = gridWidth / GRID_COLS;
  const int coverWidth = cellWidth - 2 * COVER_PADDING;
  const int coverHeight = coverWidth * COVER_ASPECT_DEN / COVER_ASPECT_NUM;
  const int thumbHeight = metrics.homeCoverHeight;
  const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int cellHeight = getCellHeight(cellWidth);
  const int visibleRows = getVisibleRows(cellHeight, contentHeight);
  const int totalRows = (static_cast<int>(shelfBooks.size()) + GRID_COLS - 1) / GRID_COLS;

  int selectedRow = shelfContentIndex >= 0 ? shelfContentIndex / GRID_COLS : 0;
  if (selectedRow < shelfScrollRow) shelfScrollRow = selectedRow;
  if (selectedRow >= shelfScrollRow + visibleRows) shelfScrollRow = selectedRow - visibleRows + 1;

  // Prewarm titles for visible cells (drawn as a fallback when a cell has no cover thumbnail) --
  // see the matching comment in renderShelvesTab for why this matters for non-Latin text.
  if (auto* fcm = renderer.getFontCacheManager()) {
    std::string prewarmBuf;
    prewarmBuf.reserve(256);
    for (int row = shelfScrollRow; row < std::min(shelfScrollRow + visibleRows, totalRows); row++) {
      for (int col = 0; col < GRID_COLS; col++) {
        const int idx = row * GRID_COLS + col;
        if (idx >= static_cast<int>(shelfBooks.size())) break;
        prewarmBuf += shelfBooks[idx].title;
        prewarmBuf += ' ';
      }
    }
    fcm->prewarmCache(SMALL_FONT_ID, prewarmBuf.c_str(), 1 << EpdFontFamily::REGULAR);
  }

  for (int row = shelfScrollRow; row < std::min(shelfScrollRow + visibleRows, totalRows); row++) {
    for (int col = 0; col < GRID_COLS; col++) {
      const int idx = row * GRID_COLS + col;
      if (idx >= static_cast<int>(shelfBooks.size())) break;
      const int cellX = metrics.contentSidePadding + col * cellWidth;
      const int cellY = contentTop + (row - shelfScrollRow) * cellHeight;
      const int pct = idx < static_cast<int>(shelfBookProgress.size()) ? shelfBookProgress[idx].percent : -1;
      drawGridCell(cellX, cellY, cellWidth, cellHeight, shelfBooks[idx].coverBmpPath, shelfBooks[idx].title, pct,
                   idx == shelfContentIndex);
    }
  }

  if (totalRows > visibleRows) {
    const int barX = pageWidth - metrics.scrollBarRightOffset - metrics.scrollBarWidth;
    const int thumbH = std::max(10, contentHeight * visibleRows / totalRows);
    const int thumbY = contentTop + (contentHeight - thumbH) * shelfScrollRow / (totalRows - visibleRows);
    renderer.fillRect(barX, thumbY, metrics.scrollBarWidth, thumbH, true);
  }

  // Release the page slots claimed by the prewarm above -- see the matching comment in
  // LyraTheme::drawRecentBookCover for why this is required.
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->clearCache();
  }
}

bool RecentBooksActivity::tryPartialSelectionRedraw() {
  if (!lastRendered.valid) return false;
  if (openShelfIndex != lastRendered.openShelf || selectedTab != lastRendered.tab) return false;

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  auto* fcm = renderer.getFontCacheManager();

  if (openShelfIndex >= 0) {
    // Shelf-books grid: pure cell-to-cell move within the rendered scroll window.
    const int oldIdx = lastRendered.shelfContentIndex;
    const int newIdx = shelfContentIndex;
    if (contentIndex != lastRendered.contentIndex) return false;
    if (oldIdx == newIdx || oldIdx < 0 || newIdx < 0) return false;
    if (newIdx >= static_cast<int>(shelfBooks.size()) || oldIdx >= static_cast<int>(shelfBooks.size())) return false;

    const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
    const int gridWidth = pageWidth - 2 * metrics.contentSidePadding;
    const int cellWidth = gridWidth / GRID_COLS;
    const int cellHeight = getCellHeight(cellWidth);
    const int visibleRows = getVisibleRows(cellHeight, contentHeight);
    const int newRow = newIdx / GRID_COLS;
    // The move must not scroll (renderShelfBooksView would adjust shelfScrollRow).
    if (newRow < shelfScrollRow || newRow >= shelfScrollRow + visibleRows) return false;
    if (shelfScrollRow != lastRendered.shelfScrollRow) return false;

    // The border ring never overlaps cell content, so the move is just erase + draw.
    for (const int idx : {oldIdx, newIdx}) {
      const int row = idx / GRID_COLS;
      const int col = idx % GRID_COLS;
      const int cellX = metrics.contentSidePadding + col * cellWidth;
      const int cellY = contentTop + (row - shelfScrollRow) * cellHeight;
      drawGridSelectionBorder(cellX, cellY, cellWidth, cellHeight, idx == newIdx);
    }
    return true;
  }

  // Main view: index 0 is the tab bar -- entering/leaving it changes the tab bar highlight, so
  // only moves between two real items qualify.
  const int oldIdx = lastRendered.contentIndex;
  const int newIdx = contentIndex;
  if (oldIdx == newIdx || oldIdx < 1 || newIdx < 1) return false;

  const int tabBarY = metrics.topPadding + metrics.headerHeight;
  const int contentTop = tabBarY + metrics.tabBarHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  if (selectedTab == 0) {
    if (newIdx > static_cast<int>(recentBooks.size()) || oldIdx > static_cast<int>(recentBooks.size())) return false;
    const int gridWidth = pageWidth - 2 * metrics.contentSidePadding;
    const int cellWidth = gridWidth / GRID_COLS;
    const int cellHeight = getCellHeight(cellWidth);
    const int visibleRows = getVisibleRows(cellHeight, contentHeight);
    const int newRow = (newIdx - 1) / GRID_COLS;
    // The move must not scroll (renderBooksTab would adjust scrollRow).
    if (newRow < scrollRow || newRow >= scrollRow + visibleRows) return false;
    if (scrollRow != lastRendered.scrollRow) return false;

    // The border ring never overlaps cell content, so the move is just erase + draw.
    for (const int idx : {oldIdx - 1, newIdx - 1}) {
      const int row = idx / GRID_COLS;
      const int col = idx % GRID_COLS;
      const int cellX = metrics.contentSidePadding + col * cellWidth;
      const int cellY = contentTop + (row - scrollRow) * cellHeight;
      drawGridSelectionBorder(cellX, cellY, cellWidth, cellHeight, idx == newIdx - 1);
    }
    return true;
  }

  // Shelves list rows.
  if (newIdx > static_cast<int>(shelves.size()) || oldIdx > static_cast<int>(shelves.size())) return false;
  const int rowHeight = metrics.listWithSubtitleRowHeight;
  const int visibleItems = std::max(1, contentHeight / rowHeight);
  auto scrollOffsetFor = [visibleItems](const int selectedItem) {
    return selectedItem >= visibleItems ? selectedItem - visibleItems + 1 : 0;
  };
  if (scrollOffsetFor(newIdx - 1) != scrollOffsetFor(oldIdx - 1)) return false;
  const int scrollOffset = scrollOffsetFor(newIdx - 1);

  if (fcm) {
    fcm->prewarmCache(UI_10_FONT_ID, (shelves[oldIdx - 1].folderName + ' ' + shelves[newIdx - 1].folderName).c_str(),
                      1 << EpdFontFamily::BOLD);
  }
  for (const int idx : {oldIdx - 1, newIdx - 1}) {
    const int itemY = contentTop + (idx - scrollOffset) * rowHeight;
    // Clear the full row footprint including the selection highlight's -6/-4 overhang.
    renderer.fillRect(metrics.contentSidePadding - 6, itemY - 4, pageWidth - 2 * metrics.contentSidePadding + 12,
                      rowHeight + 8, false);
    drawShelfRow(idx, itemY, idx == newIdx - 1);
  }
  if (fcm) fcm->clearCache();
  return true;
}

void RecentBooksActivity::render(RenderLock&&) {
  if (tryPartialSelectionRedraw()) {
    lastRendered.contentIndex = contentIndex;
    lastRendered.shelfContentIndex = shelfContentIndex;
    renderer.displayBuffer();
    return;
  }

  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  if (openShelfIndex >= 0 && openShelfIndex < static_cast<int>(shelves.size())) {
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                   shelves[openShelfIndex].folderName.c_str());

    const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

    renderShelfBooksView(contentTop, contentHeight);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

    lastRendered = {true, openShelfIndex, selectedTab, contentIndex, scrollRow, shelfContentIndex, shelfScrollRow};
    renderer.displayBuffer();
    return;
  }

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_MENU_RECENT_BOOKS));

  std::vector<TabInfo> tabs;
  tabs.reserve(TAB_COUNT);
  tabs.push_back({tr(STR_TAB_BOOKS), selectedTab == 0});
  tabs.push_back({tr(STR_TAB_SHELVES), selectedTab == 1});
  const int tabBarY = metrics.topPadding + metrics.headerHeight;
  GUI.drawTabBar(renderer, Rect{0, tabBarY, pageWidth, metrics.tabBarHeight}, tabs, contentIndex == 0);

  const int contentTop = tabBarY + metrics.tabBarHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  if (selectedTab == 0) {
    renderBooksTab(contentTop, contentHeight);
  } else {
    renderShelvesTab(contentTop, contentHeight);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_HOME), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  lastRendered = {true, openShelfIndex, selectedTab, contentIndex, scrollRow, shelfContentIndex, shelfScrollRow};
  renderer.displayBuffer();
}
