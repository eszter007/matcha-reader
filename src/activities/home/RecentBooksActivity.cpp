#include "RecentBooksActivity.h"

#include <Bitmap.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>
#include <memory>

#include <Epub.h>

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
  // Start with recent books (most recently opened first).
  recentBooks = RECENT_BOOKS.getBooks();

  // Scan the SD card root for all book files not already in recents.
  constexpr size_t NAME_BUF = 500;
  auto nameBuf = makeUniqueNoThrow<char[]>(NAME_BUF);
  if (!nameBuf) return;

  auto scanDir = [&](const char* dirPath) {
    auto dir = Storage.open(dirPath);
    if (!dir || !dir.isDirectory()) return;
    dir.rewindDirectory();
    for (auto f = dir.openNextFile(); f; f = dir.openNextFile()) {
      f.getName(nameBuf.get(), NAME_BUF);
      if (nameBuf[0] == '.' || f.isDirectory()) continue;
      std::string_view fn{nameBuf.get()};
      if (!FsHelpers::hasEpubExtension(fn) && !FsHelpers::hasXtcExtension(fn) &&
          !FsHelpers::hasTxtExtension(fn) && !FsHelpers::hasMarkdownExtension(fn))
        continue;
      std::string fullPath = std::string(dirPath);
      if (fullPath.back() != '/') fullPath += '/';
      fullPath += fn;
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
  };
  scanDir("/");

  // Also scan subdirectories one level deep.
  auto root = Storage.open("/");
  if (root && root.isDirectory()) {
    root.rewindDirectory();
    for (auto f = root.openNextFile(); f; f = root.openNextFile()) {
      f.getName(nameBuf.get(), NAME_BUF);
      if (nameBuf[0] == '.' || !f.isDirectory()) continue;
      if (strcmp(nameBuf.get(), "System Volume Information") == 0) continue;
      if (strcmp(nameBuf.get(), "dict") == 0) continue;
      std::string subPath = std::string("/") + nameBuf.get();
      scanDir(subPath.c_str());
    }
    root.close();
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
    if (folder == "/") name = "All";

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
      if (file.isDirectory()) continue;
      std::string_view fn{countBuf};
      if (FsHelpers::hasEpubExtension(fn) || FsHelpers::hasXtcExtension(fn) || FsHelpers::hasTxtExtension(fn)) {
        count++;
      }
    }
    root.close();
    shelf.bookCount = count;
  }

  std::sort(shelves.begin(), shelves.end(),
            [](const ShelfInfo& a, const ShelfInfo& b) { return a.folderName < b.folderName; });

  // Generate a shelf-height thumbnail for each shelf's cover so it renders 1:1
  // (the bitmap downscaler produces all-black at heavy reductions).
  for (auto& shelf : shelves) {
    if (shelf.coverBookPath.empty() || !FsHelpers::hasEpubExtension(shelf.coverBookPath)) continue;
    Epub epub(shelf.coverBookPath, "/.crosspoint");
    if (epub.load(true, true) && epub.generateThumbBmp(SHELF_THUMB_HEIGHT)) {
      shelf.shelfThumbPath = epub.getThumbBmpPath(SHELF_THUMB_HEIGHT);
    }
  }
}

void RecentBooksActivity::loadShelfBooks(const std::string& folderPath) {
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
    if (file.isDirectory()) continue;

    std::string_view filename{nameBuffer.get()};
    if (!FsHelpers::hasEpubExtension(filename) && !FsHelpers::hasXtcExtension(filename) &&
        !FsHelpers::hasTxtExtension(filename))
      continue;

    ShelfBook book;
    if (folderPath == "/") {
      book.path = "/" + std::string(filename);
    } else {
      book.path = folderPath + "/" + std::string(filename);
    }

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
  for (int row = scrollRow; row < std::min(scrollRow + renderRows, totalRows); row++) {
    for (int col = 0; col < GRID_COLS; col++) {
      const int idx = row * GRID_COLS + col;
      if (idx >= static_cast<int>(recentBooks.size())) break;

      const auto& book = recentBooks[idx];
      const int cellX = metrics.contentSidePadding + col * cellWidth;
      const int cellY = contentTop + (row - scrollRow) * cellHeight;
      const int coverX = cellX + COVER_PADDING;
      const int coverY = cellY + COVER_PADDING;
      const bool selected = (idx == selectedItem);

      if (selected) {
        renderer.fillRoundedRect(cellX + 2, cellY + 2, cellWidth - 4, cellHeight - 4, SELECTION_RADIUS,
                                 Color::LightGray);
      }

      bool hasCover = false;
      if (!book.coverBmpPath.empty()) {
        const std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, thumbHeight);
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

      renderer.drawRect(coverX, coverY, coverWidth, coverHeight, true);

      if (!hasCover) {
        renderer.drawIcon(CoverIcon, coverX + (coverWidth - 32) / 2, coverY + (coverHeight - 32) / 2, 32, 32);
        auto titleLines = renderer.wrappedText(SMALL_FONT_ID, book.title.c_str(), coverWidth - 8, 3);
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
      if (idx < static_cast<int>(bookProgress.size()) && bookProgress[idx].percent >= 0) {
        char pctBuf[8];
        snprintf(pctBuf, sizeof(pctBuf), "%d%%", bookProgress[idx].percent);
        const int pctW = renderer.getTextWidth(SMALL_FONT_ID, pctBuf);
        renderer.drawText(SMALL_FONT_ID, coverX + (coverWidth - pctW) / 2, labelY, pctBuf, true);
      }
    }
  }

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

  for (int i = scrollOffset; i < std::min(scrollOffset + visibleItems, shelfCount); i++) {
    const auto& shelf = shelves[i];
    const int itemY = contentTop + (i - scrollOffset) * rowHeight;
    const bool selected = (i == selectedItem);

    if (selected) {
      renderer.fillRoundedRect(metrics.contentSidePadding - 6, itemY - 4,
                               pageWidth - 2 * metrics.contentSidePadding + 12, rowHeight + 8,
                               SELECTION_RADIUS, Color::LightGray);
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

  if (shelfCount > visibleItems) {
    const int barX = pageWidth - metrics.scrollBarRightOffset - metrics.scrollBarWidth;
    const int thumbBarH = std::max(10, contentHeight * visibleItems / shelfCount);
    const int thumbBarY = contentTop + (contentHeight - thumbBarH) * scrollOffset / (shelfCount - visibleItems);
    renderer.fillRect(barX, thumbBarY, metrics.scrollBarWidth, thumbBarH, true);
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

  for (int row = shelfScrollRow; row < std::min(shelfScrollRow + visibleRows, totalRows); row++) {
    for (int col = 0; col < GRID_COLS; col++) {
      const int idx = row * GRID_COLS + col;
      if (idx >= static_cast<int>(shelfBooks.size())) break;

      const auto& book = shelfBooks[idx];
      const int cellX = metrics.contentSidePadding + col * cellWidth;
      const int cellY = contentTop + (row - shelfScrollRow) * cellHeight;
      const int coverX = cellX + COVER_PADDING;
      const int coverY = cellY + COVER_PADDING;
      const bool selected = (idx == shelfContentIndex);

      if (selected) {
        renderer.fillRoundedRect(cellX + 2, cellY + 2, cellWidth - 4, cellHeight - 4, SELECTION_RADIUS,
                                 Color::LightGray);
      }

      bool hasCover = false;
      if (!book.coverBmpPath.empty()) {
        const std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, thumbHeight);
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

      renderer.drawRect(coverX, coverY, coverWidth, coverHeight, true);

      if (!hasCover) {
        renderer.drawIcon(CoverIcon, coverX + (coverWidth - 32) / 2, coverY + (coverHeight - 32) / 2, 32, 32);
        auto titleLines = renderer.wrappedText(SMALL_FONT_ID, book.title.c_str(), coverWidth - 8, 3);
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
      if (idx < static_cast<int>(shelfBookProgress.size()) && shelfBookProgress[idx].percent >= 0) {
        char pctBuf[8];
        snprintf(pctBuf, sizeof(pctBuf), "%d%%", shelfBookProgress[idx].percent);
        const int pctW = renderer.getTextWidth(SMALL_FONT_ID, pctBuf);
        renderer.drawText(SMALL_FONT_ID, coverX + (coverWidth - pctW) / 2, labelY, pctBuf, true);
      }
    }
  }

  if (totalRows > visibleRows) {
    const int barX = pageWidth - metrics.scrollBarRightOffset - metrics.scrollBarWidth;
    const int thumbH = std::max(10, contentHeight * visibleRows / totalRows);
    const int thumbY = contentTop + (contentHeight - thumbH) * shelfScrollRow / (totalRows - visibleRows);
    renderer.fillRect(barX, thumbY, metrics.scrollBarWidth, thumbH, true);
  }
}

void RecentBooksActivity::render(RenderLock&&) {
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

  renderer.displayBuffer();
}
