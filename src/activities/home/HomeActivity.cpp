#include "HomeActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <LibraryIndexFile.h>
#include <MangaPanel.h>
#include <Utf8.h>
#include <Xtc.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "BookStatsActivity.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "EpubProgressUtil.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "XtcProgressUtil.h"
#include "components/BookActionsMenu.h"
#include "components/HomeTabBar.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Matches LibraryListActivity's hold threshold: the same gesture on every screen that offers book
// actions.
constexpr unsigned long LONG_PRESS_MS = 1000;
}  // namespace

int HomeActivity::getMenuItemCount() const {
  // The cover grid's band is the six-entry tab bar (Home included, and no OPDS), not the classic
  // home menu, so its selector space is books followed by exactly those six.
  int count = coverGridUi ? HomeTabBar::COUNT : 5;  // Library, Browse Files, File Transfer, Insights, Settings
  if (!recentBooks.empty()) {
    count += recentBooks.size();
  }
  if (hasOpdsServers && !coverGridUi) {
    count++;
  }
  return count;
}

bool HomeActivity::showBookOptions(const int bookIndex) {
  if (!coverGridUi || bookIndex < 0 || bookIndex >= static_cast<int>(recentBooks.size())) return false;
  const auto& book = recentBooks[bookIndex];
  BookActionsMenu::show(optionPopup, *this, renderer, mappedInput, book.path, book.title,
                        coverGridUi->progressFor(bookIndex), [this, bookIndex](const bool deleted) {
                          if (deleted) {
                            // The book is gone from the store: rebuild the row the grid draws
                            // from, and re-resolve covers so no card points at a dead path.
                            loadRecentBooks(CoverGridHomeUi::MAX_BOOKS);
                            fillCoverGridFromLibrary();
                            resolveGridCoverPaths();
                            coverGridUi->begin(recentBooks, hasOpdsServers, hasContinueReading);
                            selectorIndex = 0;
                          } else {
                            // Marked read or unread: the badge and the featured card's percentage
                            // both come from the stored progress.
                            coverGridUi->refreshProgress(static_cast<size_t>(bookIndex));
                          }
                          recentsLoaded = false;
                          requestUpdate();
                        });
  requestUpdate();
  return true;
}

void HomeActivity::loadRecentBooks(int maxBooks) {
  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(coverGridUi ? maxBooks : std::min(static_cast<int>(books.size()), maxBooks));

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
    if (FsHelpers::hasReflowableBookExtension(path))
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

void HomeActivity::fillCoverGridFromLibrary() {
  if (recentBooks.size() >= CoverGridHomeUi::MAX_BOOKS) return;
  // Keep the index and record together off the task stack; reuse for every row.
  struct LibraryReader {
    library::LibraryIndexFile index;
    library::ClixRecord record;
  };
  auto reader = makeUniqueNoThrow<LibraryReader>();
  if (!reader) {
    LOG_ERR("HOME", "OOM: library index");
    return;
  }
  auto& index = reader->index;
  auto& record = reader->record;
  if (!index.open(library::libraryIndexPath())) {
    index.close();
    GUI.drawPopup(renderer, tr(STR_LIBRARY_REBUILDING));
    library::BuildStats stats;
    if (!library::buildLibraryIndex("/", stats, SETTINGS.libraryUseMetadata != 0) ||
        !index.open(library::libraryIndexPath())) {
      LOG_ERR("HOME", "Cannot populate cover grid from library");
      return;
    }
  }
  for (uint16_t row = 0; row < index.bookCount() && recentBooks.size() < CoverGridHomeUi::MAX_BOOKS; ++row) {
    RecentBook book;
    if (!index.readRecord(index.ordinalForRow(library::SortOrder::RecentDesc, row), record) ||
        !index.readPath(record, book.path))
      continue;
    if (std::any_of(recentBooks.begin(), recentBooks.end(),
                    [&](const RecentBook& existing) { return existing.path == book.path; }) ||
        RecentBooksStore::isMissing(book))
      continue;
    if (!index.readTitle(record, book.title) && !index.readName(record, book.title)) continue;
    index.readAuthor(record, book.author);
    if (index.ioFailed()) break;
    recentBooks.push_back(std::move(book));
  }
}

void HomeActivity::resolveGridCoverPaths() {
  for (auto& book : recentBooks) {
    if (!book.coverBmpPath.empty()) continue;
    // Constructors only derive cache paths; no metadata parsing or image generation.
    // Keep these large objects off the task stack and release each before the next book.
    if (FsHelpers::hasReflowableBookExtension(book.path)) {
      auto epub = makeUniqueNoThrow<Epub>(book.path, "/.crosspoint");
      if (!epub) {
        LOG_ERR("HOME", "OOM: EPUB thumbnail path");
        continue;
      }
      book.coverBmpPath = epub->getThumbBmpPath();
    } else if (FsHelpers::hasXtcExtension(book.path)) {
      auto xtc = makeUniqueNoThrow<Xtc>(book.path, "/.crosspoint");
      if (!xtc) {
        LOG_ERR("HOME", "OOM: XTC thumbnail path");
        continue;
      }
      book.coverBmpPath = xtc->getThumbBmpPath();
    }
  }
}

// One book per call: find the next recent book whose thumb is missing and hand it to the worker.
// This used to convert every cover inline behind a progress popup, which froze the screen for as
// long as it took to open each book (213ms typical, 2347ms worst on device). The grid now paints
// straight away with the title-only placeholder UITheme::drawBookCover falls back to, and each
// finished cover replaces its placeholder.
void HomeActivity::postNextCoverJob(const int coverHeight) {
  if (coverHeight <= 0 || coverWorker_.busy() || !coverWorker_.running()) return;
  // A render in flight means the Try below will fail anyway. Checked first because the scan
  // opens the book's thumbnail on SD to test it, and while renders ran back to back (stepping
  // through covers) that open and its log line repeated every loop pass, ~60 times a second.
  // peek() is racy on its own, which is fine: it only skips work, and Try stays the real gate.
  if (RenderLock::peek()) return;
  while (coverScanIndex_ < static_cast<int>(recentBooks.size())) {
    RecentBook& book = recentBooks[coverScanIndex_];
    // An EMPTY cover path is "not produced yet", not "this book has none" -- see the result
    // handling in applyCoverResult(). Treating it as the latter is what left one EPUB
    // permanently without a cover on device while its neighbours were fine.
    //
    // A RAW image path (no [HEIGHT] placeholder) is a source, not a cover. Every generator
    // produces templated thumb paths, so anything else means none was made yet -- and because
    // the raw file of course exists, an exists() check alone reported "cover present" and the
    // card drew the full-size page scaled into the cell. For a dithered manga page that comes
    // out near-black (device report).
    // Templated, or a concrete thumb_<height>.bmp. The concrete form is what the Library
    // publishes for a book whose exact size cannot be generated but which still has a
    // thumbnail at another one -- it is a GENERATED cover, not a raw source, so it must not
    // send this card back through the generator on every visit.
    const bool coverIsThumb =
        book.coverBmpPath.find("[HEIGHT]") != std::string::npos || UITheme::isGeneratedThumbPath(book.coverBmpPath);
    // hasCompleteBmp(), not exists() or hasContent(): a 0-byte sentinel from an older build,
    // or a thumbnail truncated by an interrupted conversion, would otherwise count as a cover --
    // the card then skipped regeneration and drew a placeholder forever.
    const bool coverMissing =
        !coverIsThumb || !FsHelpers::hasCompleteBmp("HOME", UITheme::getCoverThumbPath(book.coverBmpPath, coverHeight));
    if (!coverMissing) {
      coverScanIndex_++;
      continue;
    }
    CoverWorker::Job job;
    job.book = book;
    job.gridHeight = coverHeight;
    job.addTargetHeight(coverHeight);
    // Coalesce the heap before handing the job over. Both halves of the conversion are
    // heap-hungry: the stylesheet parse gates on 64KB free per file and the cover inflates
    // through a 32KB zip window, while the XTH cover page needs ~104KB contiguous and the manga
    // converter ~52KB. Measured on device, the parse skipped its last stylesheet at 45904 bytes
    // free and then DISCARDED the whole parse -- throwing away ~3.5s of work that would be redone
    // and re-discarded next time. Font caches reload on demand.
    //
    // Under the render lock, as CoverLibraryActivity does it: this runs on the loop task while the
    // render task may be inside the font decompressor drawing a card title, and releasing there
    // freed the hot-group buffer under it -- a double free that corrupted the heap's free list
    // (device: tlsf_malloc fault in remove_free_block). Try, not block: a busy render just means
    // this book is posted on a later loop pass, since coverScanIndex_ has not moved.
    RenderLock lock{RenderLock::Try{}};
    if (!lock.held()) return;
    if (auto* fcm = renderer.getFontCacheManager()) fcm->releaseAllFontMemory();
    if (!coverWorker_.post(std::move(job))) {
      LOG_ERR("HOME", "Cover post: worker refused job for %s", book.path.c_str());
    }
    return;
  }
  recentsLoaded = true;
}

// The worker's outcome for one book, applied on the loop task.
void HomeActivity::applyCoverResult() {
  auto* result = coverWorker_.takeResult();
  if (!result) return;
  const std::string path = result->book.path;
  const std::string title = result->book.title;
  const std::string coverPath = result->book.coverBmpPath;
  const bool hasThumb = result->hasGridThumb;
  const bool knownAbsent = result->coverKnownAbsent;
  // A cancelled job leaves the cursor where it is and is retried on the next tick; only a job
  // that ran its course advances the scan.
  const bool completed = result->completed;
  coverWorker_.consumeResult();

  const auto live = std::find_if(recentBooks.begin(), recentBooks.end(),
                                 [&path](const RecentBook& item) { return item.path == path; });
  if (live != recentBooks.end()) {
    if (hasThumb) {
      // Also covers the recovery case: a book whose path was cleared by an earlier build gets it
      // back here instead of staying blank forever.
      if (!title.empty()) live->title = title;
      live->coverBmpPath = coverPath;
      RECENT_BOOKS.updateBook(path, live->title, live->author, coverPath);
    } else if (knownAbsent) {
      // Genuinely no cover in the book: a permanent fact, worth recording so later visits stop
      // re-parsing it.
      RECENT_BOOKS.updateBook(path, live->title, live->author, "");
      live->coverBmpPath.clear();
    } else {
      // The book HAS a cover, the conversion just didn't fit right now. Clearing the path here
      // would turn one low-heap moment into a permanent verdict: the entry loses its path, the
      // "is it missing?" test above skips it from then on, and the cover never comes back.
      LOG_ERR("HOME", "Cover thumb failed for %s; keeping path to retry", path.c_str());
    }
  }
  if (completed) coverScanIndex_++;

  // Discard the placeholder snapshot captured on an earlier paint, or the next render restores
  // the stale (cover-not-yet-generated) tile instead of reading the fresh thumb.
  coverRendered = false;
  coverBufferStored = false;
  freeCoverBuffer();
  if (coverGridUi) coverGridUi->refreshCoverPaths();
  requestUpdate();
}

// The size the cards actually resolve their thumb paths at. 0 while the grid's first pass has
// not measured a slot yet, which suppresses generation until it has.
int HomeActivity::coverTargetHeight() const {
  if (!coverGridUi) return UITheme::getInstance().getMetrics().homeCoverHeight;
  return coverGridUi->thumbHeightMeasured() ? coverGridUi->thumbHeightFor() : 0;
}

void HomeActivity::onEnter() {
  Activity::onEnter();
  coverScanIndex_ = 0;
  recentsLoaded = false;
  coverWorker_.start("HomeCover");

  hasOpdsServers = OPDS_STORE.hasServers();

  const auto& metrics = UITheme::getInstance().getMetrics();
  if (UITheme::getInstance().hasCoverGridHome()) {
    // Screen-lifetime interaction tables and component properties exceed the stack budget.
    coverGridUi = makeUniqueNoThrow<CoverGridHomeUi>(renderer);
    if (!coverGridUi) LOG_ERR("HOME", "OOM: cover grid UI; using standard home");
  }
  loadRecentBooks(coverGridUi ? CoverGridHomeUi::MAX_BOOKS : metrics.homeRecentBooksCount);
  hasContinueReading = !recentBooks.empty();
  if (coverGridUi) {
    fillCoverGridFromLibrary();
    resolveGridCoverPaths();
  }
  // A Chinese title (or one with a rare kanji) needs glyphs the built-in CJK subset lacks: bring
  // the SD companion in for the UI before anything is drawn. A Latin-only shelf pays nothing.
  {
    CjkScript uiScript = CjkScript::None;
    for (const auto& book : recentBooks) {
      uiScript = sdFontSystem.uiCjkScriptFor(book.title.c_str());
      if (uiScript != CjkScript::None) break;
    }
    sdFontSystem.setUiCjkNeeded(renderer, uiScript);
  }
  if (coverGridUi) coverGridUi->begin(recentBooks, hasOpdsServers, hasContinueReading);

  const auto base = static_cast<int>(recentBooks.size());
  selectorIndex =
      initialMenuItem == HomeMenuItem::NONE
          ? 0
          : base + (coverGridUi ? tabIndexFor(initialMenuItem) : menuItemToIndex(initialMenuItem, hasOpdsServers));

  // Trigger first update
  requestUpdate();
}

void HomeActivity::onExit() {
  // Before anything else: the worker task holds `this`, and the activity is deleted on exit.
  coverWorker_.stop();
  Activity::onExit();

  coverGridUi.reset();

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
  // A real key press must not wait for a conversion; the abandoned job is retried on a later tick
  // and the heights already written to disk are kept.
  if (mappedInput.anyButtonDownRaw()) coverWorker_.requestCancel();
  applyCoverResult();
  // Only once a card has measured the slot it draws into: a job at any other height writes a
  // thumb no card ever asks for, and every slot keeps drawing the placeholder.
  if (!recentsLoaded) postNextCoverJob(coverTargetHeight());

  // The long-press menu is a modal over the grid: while it is up it takes every input.
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;
  const int menuCount = getMenuItemCount();
  const auto& metrics = UITheme::getInstance().getMetrics();

  auto activateSelection = [this] {
    if (selectorIndex < recentBooks.size()) {
      onSelectBook(recentBooks[selectorIndex].path);
      return;
    }
    const int menuIndex = selectorIndex - static_cast<int>(recentBooks.size());
    if (coverGridUi) {
      // Tab band: the index IS the tab. Home is the screen we are on, so it falls through as a
      // no-op rather than rebuilding this activity under the user's finger.
      if (menuIndex >= 0 && menuIndex < HomeTabBar::COUNT) {
        HomeTabBar::activate(static_cast<HomeTab>(menuIndex), HomeTab::Home);
      }
      return;
    }
    switch (indexToMenuItem(menuIndex, hasOpdsServers)) {
      case HomeMenuItem::FILE_BROWSER:
        onFileBrowserOpen();
        break;
      case HomeMenuItem::LIBRARY:
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

  // Cover grid home splits navigation by button group (see below); the flat
  // next/previous cycle is for the classic list home only.
  if (!coverGridUi) {
    buttonNavigator.onNext([this, menuCount] {
      selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
      requestUpdate();
    });

    buttonNavigator.onPrevious([this, menuCount] {
      selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
      requestUpdate();
    });
  }

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
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) && hasContinueReading && !recentBooks.empty()) {
    onSelectBook(recentBooks[0].path);
    return;
  }

  if (coverGridUi) {
    // Long press on a cover opens that book's stats -- the Library grid's gesture, same screen.
    if (showBookOptions(coverGridUi->takeLongPressedBook())) return;
    // Same menu from the Confirm hold, for boards with no touch panel. Fires at the threshold
    // mid-hold; wasLongPressed() arms the release it suppresses, which ActivityManager::loop()
    // consumes before any activity runs, so it cannot land in the popup and pick its default.
    if (mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, LONG_PRESS_MS) &&
        showBookOptions(selectorIndex)) {
      return;
    }
    const int touched = coverGridUi->selectedAction(mappedInput);
    if (touched >= 0 && touched < menuCount) {
      selectorIndex = touched;
      activateSelection();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      activateSelection();
      return;
    }
    // One ring through everything on screen -- the covers, then the tab bar -- on every key.
    // Splitting them (side keys for covers, front keys for tabs) meant each pair was trapped in
    // its own band, so there was no way to walk from the last cover to the tabs and round.
    const auto step = [this, menuCount](const int dir) {
      if (menuCount <= 0) return;
      selectorIndex = (selectorIndex + menuCount + dir) % menuCount;
      requestUpdate();
    };
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up}, [&step] { step(-1); });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down}, [&step] { step(+1); });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [&step] { step(-1); });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [&step] { step(+1); });
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
  // While the long-press menu is up it owns the frame; repainting the grid under it would erase
  // the dialog and leave its hit table pointing at nothing.
  if (optionPopup.processRender(renderer, mappedInput)) {
    // The dialog drew over the grid, so the frame underneath is gone.
    if (coverGridUi) coverGridUi->invalidateFrame();
    return;
  }
  // Fast path: a cursor move between two covers over an intact frame moves the ring and nothing
  // else. A full render re-opens and re-decodes every cover thumb from SD, which is what made
  // stepping sluggish on the boards with no PSRAM to snapshot them into.
  if (coverGridUi && firstRenderDone && coverGridUi->tryMoveSelection(lastSelectorIndex, selectorIndex)) {
    lastSelectorIndex = selectorIndex;
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }
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

  renderer.clearScreen();
  if (coverGridUi) {
    lastSelectorIndex = selectorIndex;
    coverGridUi->setSelection(selectorIndex);
    UITheme::getInstance().drawCoverGridHome(*coverGridUi);
    renderer.displayBuffer(cleanInitialRefresh && !firstRenderDone ? HalDisplay::HALF_REFRESH
                                                                   : HalDisplay::FAST_REFRESH);
    // Slot heights are recorded during the draw above; a change (first layout pass, orientation
    // switch) means the paths must point at those sizes, and the scan has to walk the list again
    // because thumbs at the old size are no longer the ones the cards ask for.
    const bool coverSpecChanged = coverGridUi->takeThumbHeightChanged();
    if (coverSpecChanged) {
      coverGridUi->refreshCoverPaths();
      recentsLoaded = false;
      coverScanIndex_ = 0;
    }
    if (!firstRenderDone) {
      firstRenderDone = true;
      requestUpdate();
    }
    return;
  }
  bool bufferRestored = coverBufferStored && restoreCoverBuffer();

  // Band spans topPadding..homeTopPadding: the cover tile starts at the fixed
  // homeTopPadding, so the height must shrink by topPadding or the band (and a
  // centered title, e.g. RoundedRaff's book title) sinks into the tile.
  // Home is the stack root: no back button in its header.
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding - metrics.topPadding},
                 metrics.homeContinueReadingInMenu && !recentBooks.empty() ? recentBooks[0].title.c_str() : nullptr,
                 nullptr, false);

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

  // Back carries no action on the home menu, so it gets no hint. It used to open
  // the most recent book, which sits under a button the reader otherwise treats
  // as "go back" and was too easy to hit by accident.
  const auto labels = mappedInput.mapLabels("", tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Captured before lastRenderValid is set: it is this fork's firstRenderDone, and the
  // clean-refresh test below needs the value from *before* this paint.
  const bool firstPaint = !lastRenderValid;
  lastRenderValid = true;
  lastSelectorIndex = selectorIndex;
  // Splashless wake with no retained Quick Resume frame leaves the sleep image on the glass;
  // only a HALF pass scrubs it. FAST otherwise, which is what the bare call already defaulted to.
  renderer.displayBuffer(cleanInitialRefresh && firstPaint ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
}

void HomeActivity::onSelectBook(const std::string& path) { activityManager.goToReader(path, true); }

void HomeActivity::onFileBrowserOpen() { activityManager.goToFileBrowser(); }

void HomeActivity::onLibraryOpen() { activityManager.goToLibrary(); }

void HomeActivity::onSettingsOpen() { activityManager.goToSettings(); }

void HomeActivity::onFileTransferOpen() { activityManager.goToFileTransfer(); }

void HomeActivity::onStatsOpen() { activityManager.goToReadingStats(); }

void HomeActivity::onOpdsBrowserOpen() { activityManager.goToBrowser(); }
