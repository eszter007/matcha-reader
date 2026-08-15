#include "HomeActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <MangaPanel.h>
#include <Utf8.h>
#include <Xtc.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "EpubProgressUtil.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "XtcProgressUtil.h"
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
    // An EMPTY cover path is "not produced yet", not "this book has none" -- see the failure
    // handling below. Treating it as the latter is what left one EPUB permanently without a
    // cover on device while its neighbours were fine.
    {
      // A RAW image path (no [HEIGHT] placeholder) is a source, not a cover. Every generator
      // produces templated thumb paths, so anything else means none was made yet -- and because
      // the raw file of course exists, an exists() check alone reported "cover present" and the
      // card drew the full-size page scaled into the cell. For a dithered manga page that comes
      // out near-black (device report).
      const bool coverIsThumb = book.coverBmpPath.find("[HEIGHT]") != std::string::npos;
      // hasContent(), not exists(): SD cards written by earlier builds still carry the 0-byte
      // sentinel Epub::generateThumbBmp used to leave on failure, and exists() counted it as a
      // cover -- the card then skipped regeneration and drew a placeholder forever.
      const bool coverMissing =
          !coverIsThumb || !FsHelpers::hasContent("HOME", UITheme::getCoverThumbPath(book.coverBmpPath, coverHeight));
      if (coverMissing) {
        // If epub, try to load the metadata for title/author and cover
        if (FsHelpers::hasEpubExtension(book.path)) {
          Epub epub(book.path, "/.crosspoint");
          // Build the CSS cache alongside the first cover while the loading UI is already
          // active, so the later book click does not synchronously parse every stylesheet.
          epub.load(true, SETTINGS.embeddedStyle == 0);

          // Try to generate thumbnail image for Continue Reading card
          if (!showingLoading) {
            showingLoading = true;
            popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
          }
          GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
          const bool success = epub.generateThumbBmp(coverHeight);
          if (success) {
            // Also covers the recovery case: a book whose path was cleared by an earlier build
            // gets it back here instead of staying blank forever.
            book.coverBmpPath = epub.getThumbBmpPath();
            RECENT_BOOKS.updateBook(book.path, book.title, book.author, book.coverBmpPath);
          } else if (!epub.hasCoverImage()) {
            // Genuinely no cover in the book: a permanent fact, worth recording so later visits
            // stop re-parsing it.
            RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
            book.coverBmpPath = "";
          } else {
            // The book HAS a cover, the conversion just didn't fit right now. Clearing the path
            // here (as this did) turned one low-heap moment into a permanent verdict: the entry
            // lost its path, the "is it missing?" check above skipped it from then on, and the
            // cover never came back. Keep it and retry on the next visit.
            LOG_ERR("HOME", "Cover thumb failed for %s; keeping path to retry", book.path.c_str());
          }
          // Discard the placeholder buffer captured on the first paint so the
          // next render redraws the real cover instead of restoring the stale
          // (cover-not-yet-generated) snapshot.
          coverRendered = false;
          coverBufferStored = false;
          freeCoverBuffer();
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
            const bool success = xtc.generateThumbBmp(coverHeight);
            if (success) {
              book.coverBmpPath = xtc.getThumbBmpPath();
              RECENT_BOOKS.updateBook(book.path, book.title, book.author, book.coverBmpPath);
            } else {
              // Keep the path and retry next visit -- see the EPUB branch. This one fails on
              // heap most of all (the XTH cover page needs ~104KB contiguous), which is exactly
              // the transient condition that must not be recorded as permanent.
              LOG_ERR("HOME", "Cover thumb failed for %s; keeping path to retry", book.path.c_str());
            }
            coverRendered = false;
            coverBufferStored = false;
            freeCoverBuffer();
          }
        } else if (manga::MangaBook::isMangaFolder(book.path)) {
          // Manga folders used to fall through here with no generator at all, so the card
          // resolved its own [HEIGHT] against a thumb only the Library ever wrote -- and only at
          // the heights the Library itself draws. The X4 hid that behind a live page decode
          // (~466ms per render); the X3 has no heap for one, so its manga covers were simply
          // missing. Generating here makes the home screen self-sufficient, like the two
          // branches above, instead of depending on the Library's idle scan having run.
          if (!showingLoading) {
            showingLoading = true;
            popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
          }
          GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
          // Same reason as the XTC branch: the converter needs ~52KB contiguous, which the warm
          // font caches would otherwise be sitting on -- the difference between a cover and no
          // cover on an X3.
          if (auto* fcm = renderer.getFontCacheManager()) fcm->releaseAllFontMemory();
          const manga::MangaBook mangaBook(book.path);
          if (mangaBook.generateThumbBmp(coverHeight)) {
            // Point the entry at the thumb: leaving the raw page path would send every later
            // render back through the full-size scale that this generation exists to avoid.
            book.coverBmpPath = mangaBook.getThumbBmpPath();
            RECENT_BOOKS.updateBook(book.path, book.title, book.author, book.coverBmpPath);
          } else {
            LOG_ERR("HOME", "Failed to generate manga cover thumb for %s", book.path.c_str());
          }
          coverRendered = false;
          coverBufferStored = false;
          freeCoverBuffer();
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
  const auto& metrics = UITheme::getInstance().getMetrics();

  auto activateSelection = [this] {
    if (selectorIndex < recentBooks.size()) {
      onSelectBook(recentBooks[selectorIndex].path);
      return;
    }
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
  };

  buttonNavigator.onNext([this, menuCount] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
    requestUpdate();
  });

  buttonNavigator.onPrevious([this, menuCount] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
    requestUpdate();
  });

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
    requestUpdate();
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
    requestUpdate();
    return;
  }

  // Back is otherwise unused on the home menu: open the most recently read
  // book directly (recentBooks is most-recent-first and already pruned of
  // files missing from the SD card).
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) && !recentBooks.empty()) {
    onSelectBook(recentBooks[0].path);
    return;
  }

  const int coverColumnCount = std::max(1, metrics.homeRecentBooksCount);
  const int recentCount = std::min(static_cast<int>(recentBooks.size()), coverColumnCount);
  const int coverColumnWidth = (renderer.getScreenWidth() - 2 * metrics.contentSidePadding) / coverColumnCount;
  int touchedBook = -1;
  const auto coverTouch = mappedInput.colTouch(touchedBook, metrics.contentSidePadding, coverColumnWidth, recentCount,
                                               metrics.homeTopPadding,
                                               metrics.homeTopPadding + metrics.homeCoverTileHeight, coverColumnWidth);
  if (coverTouch != MappedInputManager::RowTouch::None) {
    if (coverTouch == MappedInputManager::RowTouch::Down) {
      if (selectorIndex != touchedBook) {
        selectorIndex = touchedBook;
        requestUpdate();
      }
    } else {
      selectorIndex = touchedBook;
      activateSelection();
    }
  }

  const int menuTop = metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.homeMenuTopOffset;
  const int renderedMenuSelection =
      metrics.homeContinueReadingInMenu ? selectorIndex : selectorIndex - recentBooks.size();
  const int renderedMenuCount =
      menuCount - (metrics.homeContinueReadingInMenu ? 0 : static_cast<int>(recentBooks.size()));
  int menuRow = -1;
  // Row height from the theme, not the metrics table: RoundedRaff draws
  // font-derived rows and the touch grid must match the visuals exactly.
  const int menuRowHeight = GUI.getMenuRowHeight(renderer);
  const auto menuTouch = mappedInput.rowTouch(menuRow, menuTop, menuRowHeight + metrics.menuSpacing, renderedMenuCount,
                                              0, INT32_MAX, menuRowHeight);
  if (menuTouch != MappedInputManager::RowTouch::None) {
    const int touchedIndex =
        metrics.homeContinueReadingInMenu ? menuRow : menuRow + static_cast<int>(recentBooks.size());
    if (menuTouch == MappedInputManager::RowTouch::Down) {
      if (selectorIndex != touchedIndex) {
        selectorIndex = touchedIndex;
        requestUpdate();
      }
    } else {
      selectorIndex = touchedIndex;
      activateSelection();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateSelection();
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

  const int menuTop = metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.homeMenuTopOffset;
  const Rect menuRect{0, menuTop, pageWidth, pageHeight - menuTop - metrics.buttonHintsHeight};
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

  // Covers BEFORE the first paint, not after. Generating them afterwards meant the card was
  // drawn and pushed to the panel with no cover yet -- a half-empty tile the user then watched
  // being replaced (device report: "the white right half should never be visible"). Nothing to
  // generate is the common case and costs only a few exists() checks, so the usual path still
  // paints exactly once; when there IS work, the progress popup covers it and the card is drawn
  // once, finished.
  if (!recentsLoaded && !recentsLoading) {
    recentsLoading = true;
    loadRecentCovers(metrics.homeCoverHeight);
  }

  renderer.clearScreen();
  bool bufferRestored = coverBufferStored && restoreCoverBuffer();

  // Band spans topPadding..homeTopPadding: the cover tile starts at the fixed
  // homeTopPadding, so the height must shrink by topPadding or the band (and a
  // centered title, e.g. RoundedRaff's book title) sinks into the tile.
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding - metrics.topPadding},
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

  const auto labels = mappedInput.mapLabels(recentBooks.empty() ? "" : tr(STR_RESUME), tr(STR_SELECT), tr(STR_DIR_UP),
                                            tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  lastRenderValid = true;
  lastSelectorIndex = selectorIndex;
  renderer.displayBuffer();
}

void HomeActivity::onSelectBook(const std::string& path) { activityManager.goToReader(path, true); }

void HomeActivity::onFileBrowserOpen() { activityManager.goToFileBrowser(); }

void HomeActivity::onLibraryOpen() { activityManager.goToRecentBooks(); }

void HomeActivity::onSettingsOpen() { activityManager.goToSettings(); }

void HomeActivity::onFileTransferOpen() { activityManager.goToFileTransfer(); }

void HomeActivity::onStatsOpen() { activityManager.goToReadingStats(); }

void HomeActivity::onOpdsBrowserOpen() { activityManager.goToBrowser(); }
