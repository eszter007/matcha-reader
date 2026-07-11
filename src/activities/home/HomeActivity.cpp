#include "HomeActivity.h"

#include "EpubProgressUtil.h"
#include "XtcProgressUtil.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <MangaPanel.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Utf8.h>
#include <Xtc.h>

#include <cstring>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

int HomeActivity::getMenuItemCount() const {
  int count = 5;  // Library, Browse Files, File Transfer, Insights, Settings
  if (!recentBooks.empty()) {
    count += recentBooks.size();
  }
  if (hasOpdsServers) {
    count++;
  }
  return count;
}

void HomeActivity::loadRecentBooks(int maxBooks) {
  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(std::min(static_cast<int>(books.size()), maxBooks));

  for (const RecentBook& book : books) {
    // Limit to maximum number of recent books
    if (recentBooks.size() >= maxBooks) {
      break;
    }

    // Skip if file no longer exists
    if (RecentBooksStore::isMissing(book)) {
      continue;
    }

    recentBooks.push_back(book);
  }

  // Compute reading progress for the first (continue reading) book.
  currentBookProgress = -1;
  if (!recentBooks.empty()) {
    const auto& path = recentBooks[0].path;
    std::string cachePath;
    if (FsHelpers::hasEpubExtension(path))
      cachePath = "/.crosspoint/epub_" + std::to_string(std::hash<std::string>{}(path));
    else if (FsHelpers::hasXtcExtension(path)) {
      currentBookProgress = XtcProgress::percentForBook(path);  // page-based; -1 if none yet
    } else if (manga::MangaBook::isMangaFolder(path)) {
      std::string mangaCache = "/.crosspoint/manga_" + std::to_string(std::hash<std::string>{}(path));
      HalFile f;
      if (Storage.openFileForRead("HOME", mangaCache + "/progress.bin", f)) {
        uint8_t data[4];
        if (f.read(data, 4) == 4) {
          uint32_t currentPage = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
          HalFile idxFile;
          if (Storage.openFileForRead("HOME", path + "/panels.idx", idxFile)) {
            uint8_t hdr[8];
            if (idxFile.read(hdr, 8) == 8) {
              uint32_t totalPages = hdr[4] | (hdr[5] << 8) | (hdr[6] << 16) | (hdr[7] << 24);
              if (totalPages > 0)
                currentBookProgress = std::clamp(
                    static_cast<int>((static_cast<float>(currentPage) / totalPages) * 100.0f + 0.5f), 0, 100);
            }
          }
        }
      }
    }
    if (!cachePath.empty()) {
      // Byte-weighted whole-book percentage, same math as the reader and Library -- see
      // EpubProgress::percentFromCache.
      currentBookProgress = EpubProgress::percentFromCache(cachePath, "HOME");
    }
  }
}

void HomeActivity::loadRecentCovers(int coverHeight) {
  recentsLoading = true;
  bool showingLoading = false;
  Rect popupRect;

  int progress = 0;
  for (RecentBook& book : recentBooks) {
    if (!book.coverBmpPath.empty()) {
      std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, coverHeight);
      if (!Storage.exists(coverPath.c_str())) {
        // If epub, try to load the metadata for title/author and cover
        if (FsHelpers::hasEpubExtension(book.path)) {
          Epub epub(book.path, "/.crosspoint");
          // Build the metadata cache if missing so the cover can be generated on
          // the first home visit (e.g. right after the book was added to recents
          // but before book.bin exists). Skip CSS — only metadata is needed here.
          epub.load(true, true);

          // Try to generate thumbnail image for Continue Reading card
          if (!showingLoading) {
            showingLoading = true;
            popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
          }
          GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
          bool success = epub.generateThumbBmp(coverHeight);
          if (!success) {
            RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
            book.coverBmpPath = "";
          }
          // Discard the placeholder buffer captured on the first paint so the
          // next render redraws the real cover instead of restoring the stale
          // (cover-not-yet-generated) snapshot.
          coverRendered = false;
          coverBufferStored = false;
          freeCoverBuffer();
          requestUpdate();
        } else if (FsHelpers::hasXtcExtension(book.path)) {
          // Handle XTC file
          Xtc xtc(book.path, "/.crosspoint");
          if (xtc.load()) {
            // Try to generate thumbnail image for Continue Reading card
            if (!showingLoading) {
              showingLoading = true;
              popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
            }
            GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
            // The XTH cover page needs a large contiguous buffer (~104KB for 2-bit 528x792) --
            // more than the largest free block once the font caches are warm, so the thumb
            // generation would fail and the cover would be missing. Coalesce the heap first.
            if (auto* fcm = renderer.getFontCacheManager()) fcm->releaseAllFontMemory();
            bool success = xtc.generateThumbBmp(coverHeight);
            if (!success) {
              RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
              book.coverBmpPath = "";
            }
            coverRendered = false;
            coverBufferStored = false;
            freeCoverBuffer();
            requestUpdate();
          }
        }
      }
    }
    progress++;
  }

  recentsLoaded = true;
  recentsLoading = false;
}

void HomeActivity::onEnter() {
  Activity::onEnter();

  hasOpdsServers = OPDS_STORE.hasServers();

  const auto& metrics = UITheme::getInstance().getMetrics();
  loadRecentBooks(metrics.homeRecentBooksCount);

  const auto base = static_cast<int>(recentBooks.size());
  selectorIndex = initialMenuItem == HomeMenuItem::NONE ? 0 : base + menuItemToIndex(initialMenuItem, hasOpdsServers);

  // Trigger first update
  requestUpdate();
}

void HomeActivity::onExit() {
  Activity::onExit();

  // Free the stored cover buffer if any
  freeCoverBuffer();
}

bool HomeActivity::storeCoverBuffer() {
  // render() must have already set the cover rect; without it we'd be back to
  // cloning the whole framebuffer.
  if (coverRectW <= 0 || coverRectH <= 0) return false;
  freeCoverBuffer();
  const size_t needed = renderer.getRegionByteSize(coverRectX, coverRectY, coverRectW, coverRectH);
  if (needed == 0) return false;
  coverBuffer = static_cast<uint8_t*>(malloc(needed));
  if (!coverBuffer) {
    LOG_ERR("HOME", "OOM: cover buffer (%u bytes)", (unsigned)needed);
    return false;
  }
  coverBufferSize = needed;
  if (!renderer.copyRegionToBuffer(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize)) {
    free(coverBuffer);
    coverBuffer = nullptr;
    coverBufferSize = 0;
    return false;
  }
  return true;
}

bool HomeActivity::restoreCoverBuffer() {
  if (!coverBuffer || coverRectW <= 0 || coverRectH <= 0) return false;
  return renderer.copyBufferToRegion(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize);
}

void HomeActivity::freeCoverBuffer() {
  if (coverBuffer) {
    free(coverBuffer);
    coverBuffer = nullptr;
  }
  coverBufferSize = 0;
  coverBufferStored = false;
}

void HomeActivity::loop() {
  const int menuCount = getMenuItemCount();

  buttonNavigator.onNext([this, menuCount] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
    requestUpdate();
  });

  buttonNavigator.onPrevious([this, menuCount] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectorIndex < recentBooks.size()) {
      onSelectBook(recentBooks[selectorIndex].path);
    } else {
      const int menuIndex = selectorIndex - static_cast<int>(recentBooks.size());
      switch (indexToMenuItem(menuIndex, hasOpdsServers)) {
        case HomeMenuItem::FILE_BROWSER:
          onFileBrowserOpen();
          break;
        case HomeMenuItem::RECENTS:
          onLibraryOpen();
          break;
        case HomeMenuItem::OPDS_BROWSER:
          onOpdsBrowserOpen();
          break;
        case HomeMenuItem::FILE_TRANSFER:
          onFileTransferOpen();
          break;
        case HomeMenuItem::READING_STATS:
          onStatsOpen();
          break;
        case HomeMenuItem::SETTINGS_MENU:
          onSettingsOpen();
          break;
        default:
          break;
      }
    }
  }
}

void HomeActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // Build menu items dynamically (both render paths need the same model)
  std::vector<const char*> menuItems = {tr(STR_MENU_RECENT_BOOKS), tr(STR_BROWSE_FILES), tr(STR_FILE_TRANSFER),
                                        tr(STR_STATS), tr(STR_SETTINGS_TITLE)};
  std::vector<UIIcon> menuIcons = {Library, Folder, Transfer, Stats, Settings};

  if (hasOpdsServers) {
    menuItems.insert(menuItems.begin() + 2, tr(STR_OPDS_BROWSER));
    menuIcons.insert(menuIcons.begin() + 2, Library);
  }

  if (metrics.homeContinueReadingInMenu && !recentBooks.empty()) {
    // Insert Continue Reading at the top if enabled in theme
    menuItems.insert(menuItems.begin(), tr(STR_CONTINUE_READING));
    menuIcons.insert(menuIcons.begin(), Book);
  }

  const Rect menuRect{0, metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.homeMenuTopOffset, pageWidth,
                      pageHeight - (metrics.headerHeight + metrics.homeTopPadding + metrics.verticalSpacing +
                                    metrics.homeMenuTopOffset + metrics.buttonHintsHeight)};
  const int menuSelected =
      metrics.homeContinueReadingInMenu ? selectorIndex : selectorIndex - static_cast<int>(recentBooks.size());

  // Fast path: a cursor move between two MENU rows over an intact frame erases and redraws just
  // the menu block, with the exact same theme drawing as the full render. The header (SD-font
  // book title) and the cover tile -- whose glyph/cover reloads dominate a full render -- stay
  // untouched in the framebuffer. Moves that involve the cover-tile selection fall through.
  const int menuStart = metrics.homeContinueReadingInMenu ? 0 : static_cast<int>(recentBooks.size());
  if (lastRenderValid && selectorIndex != lastSelectorIndex && selectorIndex >= menuStart &&
      lastSelectorIndex >= menuStart) {
    renderer.fillRect(menuRect.x, menuRect.y, menuRect.width, menuRect.height, false);
    GUI.drawButtonMenu(
        renderer, menuRect, static_cast<int>(menuItems.size()), menuSelected,
        [&menuItems](int index) { return std::string(menuItems[index]); },
        [&menuIcons](int index) { return menuIcons[index]; });
    lastSelectorIndex = selectorIndex;
    renderer.displayBuffer();
    return;
  }

  renderer.clearScreen();
  bool bufferRestored = coverBufferStored && restoreCoverBuffer();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding},
                 metrics.homeContinueReadingInMenu && !recentBooks.empty() ? recentBooks[0].title.c_str() : nullptr);

  // Record the tile rect so storeCoverBuffer (called from the theme) knows
  // which sub-region of the framebuffer to snapshot. ~16 KB in Portrait
  // instead of the 48 KB full framebuffer the previous bind captured.
  coverRectX = 0;
  coverRectY = metrics.homeTopPadding;
  coverRectW = pageWidth;
  coverRectH = metrics.homeCoverTileHeight;

  GUI.drawRecentBookCover(renderer, Rect{0, metrics.homeTopPadding, pageWidth, metrics.homeCoverTileHeight},
                          recentBooks, selectorIndex, coverRendered, coverBufferStored, bufferRestored,
                          std::bind(&HomeActivity::storeCoverBuffer, this), currentBookProgress);

  GUI.drawButtonMenu(
      renderer, menuRect, static_cast<int>(menuItems.size()), menuSelected,
      [&menuItems](int index) { return std::string(menuItems[index]); },
      [&menuIcons](int index) { return menuIcons[index]; });

  const auto labels = mappedInput.mapLabels("", tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  lastRenderValid = true;
  lastSelectorIndex = selectorIndex;
  renderer.displayBuffer();

  if (!firstRenderDone) {
    firstRenderDone = true;
    requestUpdate();
  } else if (!recentsLoaded && !recentsLoading) {
    recentsLoading = true;
    loadRecentCovers(metrics.homeCoverHeight);
  }
}

void HomeActivity::onSelectBook(const std::string& path) { activityManager.goToReader(path); }

void HomeActivity::onFileBrowserOpen() { activityManager.goToFileBrowser(); }

void HomeActivity::onLibraryOpen() { activityManager.goToRecentBooks(); }

void HomeActivity::onSettingsOpen() { activityManager.goToSettings(); }

void HomeActivity::onFileTransferOpen() { activityManager.goToFileTransfer(); }

void HomeActivity::onStatsOpen() { activityManager.goToReadingStats(); }

void HomeActivity::onOpdsBrowserOpen() { activityManager.goToBrowser(); }
