#include "MangaReaderActivity.h"

#include <Bitmap.h>
#include <DictIndex.h>
#include <Epub/converters/BmpToFramebufferConverter.h>
#include <Epub/converters/ImageDecoderFactory.h>
#include <Epub/converters/ImageToFramebufferDecoder.h>
#include <Epub/converters/PixelCache.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <ctime>
#include <iterator>
#include <utility>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "EpubReaderTranslationActivity.h"
#include "MangaBookmarksActivity.h"
#include "MangaChapterSelectionActivity.h"
#include "MangaWordLookupActivity.h"
#include "MappedInputManager.h"
#include "ProgressFile.h"
#include "QrDisplayActivity.h"
#include "ReaderUtils.h"
#include "ReadingStatsStore.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "WordSelectionScan.h"
#include "activities/settings/SettingsActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookmarkFile.h"
#include "util/BookmarkUtil.h"
#include "util/ScreenshotUtil.h"

namespace {
constexpr int PAGE_TURN_RATES[] = {1, 1, 3, 6, 12};

// Idle-dwell gates (ms since the last render) before speculative pixel-cache prefetch. The
// next-page / next-panel warms wait the full dwell so rapid flipping doesn't queue a blocking
// decode between presses. The first panel of a paneled page uses a shorter dwell and is warmed
// *first* (ahead of the next-page cache): zooming in is the likely next action there, and its cold
// JPEG decode is the slowest step of the full-page -> panel transition, so getting it cached even
// after a brief pause is what makes that transition feel instant.
constexpr unsigned long PREFETCH_DWELL_MS = 400;
constexpr unsigned long FIRST_PANEL_PREFETCH_DWELL_MS = 150;

// Heap floor before a background decode starts. The old in-loop prefetch also needed a ~48KB
// framebuffer snapshot; the cache-only worker doesn't (it never touches the framebuffer), but
// the decoder (~20KB JPEG / ~44KB PNG) plus the streaming cache band (<=24KB) still want real
// headroom -- and a foreground render may now allocate concurrently. Keep the old conservative
// floor rather than shaving it.
constexpr uint32_t PREFETCH_HEAP_FLOOR = 60000;

// Prefetch worker stack, in bytes. Mirrors the render task (ActivityManager::begin), which runs
// these same JPEG/PNG decode paths -- the one stack size proven for them on hardware.
constexpr uint32_t PREFETCH_TASK_STACK = 8192;
}  // namespace

void MangaReaderActivity::onEnter() {
  Activity::onEnter();

  // Press-driven entry leaves a release pending; release/touch-driven entry does not.
  ignoreNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);

  if (!book) {
    // Retires renderer font registrations, so it cannot run beside the render task.
    // Manga renders no book text: drop the Japanese companion policy explicitly, since
    // releaseAllResidentFonts() frees memory without deciding what should come back.
    {
      RenderLock lock;
      sdFontSystem.releaseAllResidentFonts(renderer);
      sdFontSystem.setJpFallbackNeeded(renderer, false);
    }
    book = makeUniqueNoThrow<manga::MangaBook>(std::move(pendingBookPath));
    if (!book) {
      LOG_ERR("MRA", "Failed to allocate MangaBook");
      {
        RenderLock lock;
        sdFontSystem.ensureLoaded(renderer);
      }
      finish();
      return;
    }
    if (!book->load()) {
      LOG_ERR("MRA", "Failed to load manga: %s", book->getFolder().c_str());
      book.reset();
      {
        RenderLock lock;
        sdFontSystem.ensureLoaded(renderer);
      }
      finish();
      return;
    }
  }

  // Which layout this book's panel crops use. Newer conversions put them in a subfolder so the
  // book folder holds only page images: MangaBook::scanImages() walks every entry on open, and on
  // device that ran 6499ms for 2396 entries of which 219 were pages and 974 were crops. Probed
  // once per book, never per page -- an SD open is ~85ms here. The flat layout stays supported
  // indefinitely; re-converting is an optimisation the reader may take, not a requirement.
  {
    std::string subdir = book->getFolder();
    if (subdir.empty() || subdir.back() != '/') subdir += '/';
    subdir += PANEL_CROP_SUBDIR;
    panelCropsInSubdir = Storage.exists(subdir.c_str());
    LOG_DBG("MRA", "Panel crops: %s layout", panelCropsInSubdir ? "subfolder" : "flat (legacy)");
  }
  // Do not base this on the current page: cover/splash records normally have no crop because the
  // full-page image is already the best presentation. panels.idx is in memory after MangaBook::load(),
  // so this remains a one-time, no-SD-scan capability check for the whole book.
  bookHasPanelCropCapability = book->hasPanelCropCapability();

  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  // Base screen dims for the prefetch worker's geometry math -- captured here, the one moment
  // that is single-threaded by construction (no render has been requested yet). See the header.
  baseScreenW = renderer.getScreenWidth();
  baseScreenH = renderer.getScreenHeight();
  loadProgress();
  loadCachedBookmarks();
  updateBookmarkFlag();

  APP_STATE.openEpubPath = book->getFolder();
  APP_STATE.saveToFile();

  // Use author from meta.bin; fall back to any author already in recents.
  std::string bookAuthor = book->getAuthor();
  if (bookAuthor.empty()) {
    const auto& books = RECENT_BOOKS.getBooks();
    const auto r =
        std::find_if(books.begin(), books.end(), [this](const auto& b) { return b.path == book->getFolder(); });
    if (r != books.end()) bookAuthor = r->author;
  }
  // The [HEIGHT]-templated thumb path, like Epub and Xtc record -- NOT the raw first page.
  // Storing the raw page here overwrote the thumb path on every open, so the home screen found
  // no cover, showed its progress popup and "generated" a thumbnail that was already on the card
  // (device report: Loading on every return from a manga). Drawing it also meant scaling a
  // full-size page into the card instead of blitting the cached one.
  RECENT_BOOKS.addBook(book->getFolder(), book->getTitle(), bookAuthor, book->getThumbBmpPath());
  BookStats::recordOpen(book->getFolder().c_str());

  readingSessionStartMs = millis();

  // Background prefetch worker (see the header doc block). Same priority as the loop and render
  // tasks so it timeslices instead of starving either; stack mirrors the render task, which runs
  // these same decode paths. Creation failure (OOM) just disables prefetching -- postPrefetchJob
  // no-ops on a null handle and every render path decodes on demand exactly as before.
  // Do not spend an 8 KB task stack when that would push a foreground PNG decode below its
  // 60 KB allocation floor. Prefetch is optional; page rendering always has an on-demand path.
  constexpr uint32_t prefetchStartupFloor = PREFETCH_HEAP_FLOOR + PREFETCH_TASK_STACK + 8 * 1024;
  if (ESP.getFreeHeap() < prefetchStartupFloor) {
    prefetchTaskHandle = nullptr;
    LOG_INF("MRA", "Low heap (%u); prefetch disabled to reserve image decoder memory", ESP.getFreeHeap());
  } else if (xTaskCreate(&prefetchTaskTrampoline, "MangaPrefetch", PREFETCH_TASK_STACK, this, 1, &prefetchTaskHandle) !=
             pdPASS) {
    prefetchTaskHandle = nullptr;
    LOG_ERR("MRA", "Failed to create prefetch worker; prefetch disabled");
  }

  loadCurrentPagePanels();
  requestUpdate();
}

void MangaReaderActivity::onExit() {
  // Join the prefetch worker FIRST, before any teardown below: it may be mid-decode against book
  // paths and activity members. The exit flag makes the decode's cancel probe abort within one
  // MCU block/scanline (partial tmp dropped by the converter), so this spin is short -- worst
  // case one in-flight SD transaction plus cleanup. Deadlock-free even though ActivityManager's
  // pop path calls onExit() while holding RenderLock: the worker never takes that lock (see the
  // header doc block).
  if (prefetchTaskHandle) {
    prefetchExitRequested = true;
    xTaskNotifyGive(prefetchTaskHandle);  // wake it if idle-blocked on the notification
    while (!prefetchTaskExited) {
      vTaskDelay(1);
    }
    prefetchTaskHandle = nullptr;
  }

  // Record the sub-interval tail of the session; whole minutes were already flushed
  // periodically from loop() (see ReaderUtils::flushReadingStats).
  ReaderUtils::flushReadingStats(readingSessionStartMs, /*force=*/true, book ? book->getFolder().c_str() : nullptr,
                                 book ? book->getLanguage().c_str() : nullptr);

  // On the last page or the end-screen sentinel, regardless of how long this
  // particular session lasted.
  // Previously this was nested inside `minutes > 0` AND compared against
  // pageCount instead of pageCount-1, so it could never fire -- manga never
  // got marked finished even at 100% progress, unlike Epub/Txt readers.
  const bool atLastPage = book && book->getPageCount() > 0 && currentPage >= book->getPageCount() - 1;
  if (atLastPage) {
    READING_STATS_STORE.loadFromFile();
    READING_STATS_STORE.markBookFinished(book->getFolder());
    READING_STATS_STORE.saveToFile();
  }

  saveProgress();
  endOfBookOptionsReady.store(false, std::memory_order_release);
  endOfBookOptions.reset();
  panels.clear();
  book.reset();

  // Reset orientation back to portrait for the rest of the UI (matches
  // EpubReaderActivity/TxtReaderActivity) -- Home/Library never call
  // applyOrientation themselves, so any rotation left active here (from the
  // reading orientation setting, or a page/panel that triggered the
  // fill-the-screen rotate) would otherwise leak into their layout.
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  // Home/Library and the next text reader need the user's selected font again. The manga entry
  // released it only from RAM; ensureLoaded() restores the unchanged saved selection here.
  {
    RenderLock lock;
    sdFontSystem.setJpFallbackNeeded(renderer, false);
    sdFontSystem.ensureLoaded(renderer);
  }

  Activity::onExit();
}

void MangaReaderActivity::loadCurrentPagePanels() {
  // render() runs on the dedicated render task and reads `panels`/`panelDims` under RenderLock.
  // Do the slow SD work (panel index load, crop probe) into locals WITHOUT the lock -- holding
  // it across SD I/O would stall the render task -- then publish everything in one short locked
  // swap so the render task never observes a half-cleared or reallocating vector.
  std::vector<manga::Panel> loadedPanels;
  bool loaded = false;
  if (book && currentPage < book->getPageCount()) {
    loaded = book->loadPagePanels(currentPage, loadedPanels);
  }
  // Probe once per page what the input handler and panel renders need on every press: whether
  // real crop files exist (full-page panels like covers have none), which format they use
  // (converter writes .jpg normally or .bmp with --mono; uniform per book), and -- for BMP -- whether
  // they're 1-bit monochrome (single-BW-wave fast path). Per-panel dimension slots are filled
  // lazily by prefetch or first render.
  bool hasCrops = false;
  bool cropsBmp = false;
  bool panelsBw = false;
  int firstCrop = -1;
  PanelCropDims firstPanelDims;
  if (!loadedPanels.empty() && book) {
    // Do not assume panel 0 has a file. Older converters skipped a redundant crop when a
    // detected panel covered the whole page, while later panels on that same page can still
    // have real crops. Stop at the first hit so the normal case remains one/two exists calls.
    for (size_t i = 0; i < loadedPanels.size(); i++) {
      const std::string bmp = panelCropPathExt(static_cast<int>(i), ".bmp");
      if (Storage.exists(bmp.c_str())) {
        hasCrops = true;
        cropsBmp = true;
        firstCrop = static_cast<int>(i);
        BmpToFramebufferConverter::Metadata metadata;
        if (BmpToFramebufferConverter::getMetadataStatic(bmp, metadata)) {
          panelsBw = metadata.monochrome;
          firstPanelDims = {metadata.dimensions.width, metadata.dimensions.height};
        }
        break;
      }
      if (Storage.exists(panelCropPathExt(static_cast<int>(i), ".jpg").c_str())) {
        hasCrops = true;
        firstCrop = static_cast<int>(i);
        break;
      }
    }
  }
  // A 1-bit BMP full page renders BW-only (single wave, no gray planes); probe it once here
  // (cheap header read) so renderFullPage can pick the fast path. Only .bmp pages can be
  // monochrome -- jpg/png always go the grayscale route.
  bool bwOnly = false;
  bool hasPageImage = false;
  ImageDimensions bmpDims{0, 0};
  if (book) {
    const std::string pageImg = book->getPageImagePath(currentPage);
    hasPageImage = !pageImg.empty();
    if (FsHelpers::hasBmpExtension(pageImg)) {
      BmpToFramebufferConverter::Metadata metadata;
      if (BmpToFramebufferConverter::getMetadataStatic(pageImg, metadata)) {
        bwOnly = metadata.monochrome;
        bmpDims = metadata.dimensions;
      }
    }
  }
  // Arm the first-panel prefetch for this page only when panel-zoom has been used in this book
  // -- full-page-only reading must not pay speculative decodes and cache writes.
  const bool armFirst = panelPrefetchArmed && hasCrops;
  {
    RenderLock lock;  // callers are all loop/onEnter/result-handler context -- never already held
    // Invalidate any in-flight prefetch-worker job: its results were computed against the OLD
    // page's panels/panelDims and must be dropped by applyPrefetchResult's generation check.
    // Bumped inside the same locked section as the panelDims swap so a job can never observe
    // the new vector with the old generation.
    pageGeneration++;
    panels = std::move(loadedPanels);
    panelsLoaded = loaded;
    pageHasPanelCrops = hasCrops;
    firstPanelWithCrop = firstCrop;
    currentPageHasImage = hasPageImage;
    currentPageBwOnly = bwOnly;
    currentPageBmpWidth = bmpDims.width;
    currentPageBmpHeight = bmpDims.height;
    panelCropIsBmp = cropsBmp;
    panelsBwOnly = panelsBw;
    panelDims.assign(panels.size(), {});
    if (firstPanelWithCrop >= 0 && firstPanelWithCrop < static_cast<int>(panelDims.size()) && firstPanelDims.w > 0 &&
        firstPanelDims.h > 0) {
      panelDims[firstPanelWithCrop] = firstPanelDims;
    }
    firstPanelPrefetched = !armFirst;
    nextPanelPrefetched = true;
    // A panels-only conversion has no full-page overview to render. Enter its first available
    // crop automatically, including after restoring progress or changing pages.
    if ((!currentPageHasImage || panelsOnlyMode) && firstPanelWithCrop >= 0) {
      currentPanel = firstPanelWithCrop;
      viewMode = ViewMode::PanelZoom;
    }
  }
  updateBookmarkFlag();
}

std::string MangaReaderActivity::panelCropPath(const int panelIdx) const {
  return panelCropPathExt(panelIdx, panelCropIsBmp ? ".bmp" : ".jpg");
}

std::string MangaReaderActivity::panelCropPathExt(const int panelIdx, const char* ext) const {
  char cropName[64];
  snprintf(cropName, sizeof(cropName), "p%u_%d%s", static_cast<unsigned>(currentPage), panelIdx, ext);
  std::string path = book->getFolder();
  if (path.empty() || path.back() != '/') path += '/';  // path.back() on an empty string is UB
  if (panelCropsInSubdir) {
    path += PANEL_CROP_SUBDIR;
    path += '/';
  }
  path += cropName;
  return path;
}

void MangaReaderActivity::nextPanel() {
  // Leaving the current panel: cancel any deferred gray upgrade queued/elapsed for it so a stale
  // dwell can't run the gray pass against the panel we're stepping to before it has rendered BW.
  panelGrayPending = false;
  panelGrayUpgrade = false;

  if (panels.empty()) {
    nextPage();
    return;
  }

  if (currentPanel < 0) {
    currentPanel = firstPanelWithCrop;
    viewMode = ViewMode::PanelZoom;
    requestUpdate();
    return;
  } else if (currentPanel < static_cast<int>(panels.size()) - 1) {
    currentPanel++;
    requestUpdate();
    return;
  }

  // Normal mode shows every page overview before its crops. Panels Only (and conversions that
  // physically omit full pages) stays in crops continuously across the page boundary.
  nextPage(panelsOnlyMode || !currentPageHasImage);
}

void MangaReaderActivity::prevPanel() {
  // See nextPanel(): cancel a deferred gray upgrade queued for the panel we're leaving.
  panelGrayPending = false;
  panelGrayUpgrade = false;

  if (currentPanel > firstPanelWithCrop) {
    currentPanel--;
    requestUpdate();
  } else if (!panelsOnlyMode && currentPageHasImage) {
    // Reverse of normal mode's full-page -> panels sequence.
    currentPanel = -1;
    viewMode = ViewMode::FullPage;
    requestUpdate();
  } else {
    // Panels Only must land on the previous page's LAST real crop, not its full-page overview or
    // first crop. prevPage(true) uses the full page only when that page has no crop at all.
    prevPage(true);
  }
}

int MangaReaderActivity::findPanelWithCrop(const int start, const int step) const {
  if (step == 0 || panels.empty()) return -1;
  for (int i = start; i >= 0 && i < static_cast<int>(panels.size()); i += step) {
    if (Storage.exists(panelCropPath(i).c_str())) return i;
  }
  return -1;
}

void MangaReaderActivity::nextPage(const bool keepPanelMode) {
  if (!book) return;
  if (currentPage + 1 < book->getPageCount()) {
    currentPage++;
    currentPanel = -1;
    viewMode = ViewMode::FullPage;
    loadCurrentPagePanels();
    if ((keepPanelMode || panelsOnlyMode) && pageHasPanelCrops) {
      currentPanel = firstPanelWithCrop;
      viewMode = ViewMode::PanelZoom;
    }
    // In Panels Only a page with no detected crops still matters (cover, splash page, failed
    // detection): leave FullPage selected as the fallback, then resume crops on the next page.
    requestUpdate();
  } else if (currentPage < book->getPageCount()) {
    currentPage = book->getPageCount();
    currentPanel = -1;
    viewMode = ViewMode::FullPage;
    automaticPageTurnActive = false;
    requestUpdate();
  }
}

void MangaReaderActivity::prevPage(const bool keepPanelMode) {
  if (!book) return;
  if (currentPage > 0) {
    currentPage--;
    currentPanel = -1;
    viewMode = ViewMode::FullPage;
    loadCurrentPagePanels();
    if ((keepPanelMode || panelsOnlyMode) && pageHasPanelCrops) {
      const int lastCrop = findPanelWithCrop(static_cast<int>(panels.size()) - 1, -1);
      if (lastCrop >= 0) {
        currentPanel = lastCrop;
        viewMode = ViewMode::PanelZoom;
      }
    }
    // No crop: FullPage remains the intentional fallback in both navigation directions.
    requestUpdate();
  }
}

bool MangaReaderActivity::isAtEndOfBook() const { return book && currentPage >= book->getPageCount(); }

void MangaReaderActivity::clearEndOfBookOptionsIfNeeded() {
  if (isAtEndOfBook() || !endOfBookOptionsReady.load(std::memory_order_acquire)) return;
  RenderLock lock{RenderLock::Try{}};
  if (!lock.held()) return;
  endOfBookOptionsReady.store(false, std::memory_order_release);
  endOfBookOptions.reset();
}

void MangaReaderActivity::onReturnFromEndOfBook() {
  currentPage = book && book->getPageCount() > 0 ? book->getPageCount() - 1 : 0;
  currentPanel = -1;
  viewMode = ViewMode::FullPage;
  loadCurrentPagePanels();
}

bool MangaReaderActivity::handleEndOfBookMenu() {
  if (!isAtEndOfBook() || !endOfBookOptionsReady.load(std::memory_order_acquire) || !endOfBookOptions->menuActive()) {
    return false;
  }

  std::string openPath;
  switch (endOfBookOptions->handleMenuInput(mappedInput, &openPath)) {
    case EndOfBookOptions::Action::OpenBook:
      activityManager.goToReader(openPath);
      return true;
    case EndOfBookOptions::Action::GoHome:
      onGoHome();
      return true;
    case EndOfBookOptions::Action::LastPage:
      onReturnFromEndOfBook();
      requestUpdate();
      return true;
    case EndOfBookOptions::Action::Redraw:
      requestUpdate();
      return true;
    case EndOfBookOptions::Action::None:
      return false;
  }
  return false;
}

bool MangaReaderActivity::handleEndOfBookPageTurn(const bool prevTriggered, const bool nextTriggered) {
  if (!isAtEndOfBook()) return false;
  if (endOfBookOptionsReady.load(std::memory_order_acquire) && endOfBookOptions->menuActive()) return true;
  if (nextTriggered) {
    onGoHome();
  } else if (prevTriggered) {
    onReturnFromEndOfBook();
    requestUpdate();
  }
  return true;
}

bool MangaReaderActivity::renderEndOfBook() {
  if (!isAtEndOfBook()) return false;
  displayedRotated_ = false;  // this screen is drawn in the base orientation, whatever preceded it
  displayedMap_.valid = false;
  if (!endOfBookOptions) {
    endOfBookOptions = makeUniqueNoThrow<EndOfBookOptions>(renderer);
    if (!endOfBookOptions) LOG_ERR("MRA", "OOM: EndOfBookOptions");
    endOfBookOptionsReady.store(endOfBookOptions != nullptr, std::memory_order_release);
  }
  renderer.clearScreen();
  if (endOfBookOptions) {
    endOfBookOptions->loadOnce(book->getFolder());
    endOfBookOptions->render(renderer, mappedInput);
  } else {
    renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() * 3 / 8, tr(STR_END_OF_BOOK), true,
                              EpdFontFamily::BOLD);
  }
  renderer.displayBuffer();
  return true;
}

void MangaReaderActivity::toggleAutoPageTurn(const uint8_t selectedPageTurnOption) {
  if (selectedPageTurnOption == 0 || selectedPageTurnOption >= std::size(PAGE_TURN_RATES)) {
    automaticPageTurnActive = false;
    return;
  }
  lastPageTurnTime = millis();
  pageTurnDuration = (1UL * 60 * 1000) / PAGE_TURN_RATES[selectedPageTurnOption];
  automaticPageTurnActive = true;
}

void MangaReaderActivity::loop() {
  // Crash-proof stats: flush whole minutes every few minutes so an exit path that
  // never reaches onExit() (hang/reset on sleep, battery pull) can't lose the day.
  ReaderUtils::flushReadingStats(readingSessionStartMs, /*force=*/false, book ? book->getFolder().c_str() : nullptr,
                                 book ? book->getLanguage().c_str() : nullptr);

  // Publish any finished prefetch-worker result (panelDims slot + done flags) under the loop
  // task's normal locking rules. Runs before everything else so a completed warm is visible to
  // this very tick's input handling, and so the single job slot frees up for the next post.
  applyPrefetchResult();

  clearEndOfBookOptionsIfNeeded();
  if (handleEndOfBookMenu()) return;

  // Same touch contract as every other reader (EpubReaderActivity, XtcReaderActivity,
  // TxtReaderActivity): outer-third taps or horizontal swipes turn the page, a centre tap opens
  // the menu. Both helpers gate on SETTINGS.touchReaderControls and hasTouch(), so tap-vs-swipe
  // mode and the global opt-out are inherited and non-touch boards see no change. Read once per
  // tick, before any early return can swallow the gesture.
  // Resolve touch in the orientation the page is actually displayed in. A page or
  // panel whose aspect does not match the screen is drawn rotated, and the render
  // path restores the base orientation before returning -- so without this the
  // zones follow the Reading Orientation setting no matter which way the content
  // is on screen, and the outer thirds land on the wrong edges of a rotated page.
  // Both the tap point (tapToLogical) and the screen dims come from the renderer's
  // current orientation, so setting it around the read gives a consistent frame.
  //
  // Under a Try lock, because render() runs on its own task: flipping the renderer's orientation
  // while a frame is in flight would tear it, and displayedRotated_ is written by that same task.
  // Taking the lock makes both safe, and taking it non-blocking keeps the input loop off the
  // render's critical path -- on a miss this tick simply resolves in the base orientation, which
  // is what it did before.
  ReaderUtils::TouchPageTurn touch{};
  bool touchMenu = false;
  std::string holdText;
  int holdGlyph = -1;
  {
    RenderLock touchLock{RenderLock::Try{}};
    const bool rotateTouch = touchLock.held() && displayedRotated_;
    auto baseOrientation = GfxRenderer::Orientation::Portrait;
    if (rotateTouch) {
      // Read inside the lock too: the render task writes this field while it draws a rotated
      // page, so reading it unguarded would be a race in its own right.
      baseOrientation = renderer.getOrientation();
      renderer.setOrientation(static_cast<GfxRenderer::Orientation>((baseOrientation + 3) % 4));
    }
    // A hold on a speech bubble looks up the word under the finger. Read in the drawn frame like
    // the zones, and first: a hold never also reads as the tap those handle. Only under the lock,
    // since displayedMap_ is the render task's -- a miss simply leaves the hold for the next tick.
    int holdX = 0;
    int holdY = 0;
    if (touchLock.held() && mappedInput.hasTouch() && DictIndex::isAvailable() &&
        mappedInput.wasScreenLongPress(holdX, holdY) && !holdTarget(holdX, holdY, holdText, holdGlyph)) {
      holdGlyph = -1;
    }
    touch = ReaderUtils::detectTouchPageTurn(renderer, mappedInput);
    touchMenu = ReaderUtils::isTouchMenuGesture(renderer, mappedInput);
    if (rotateTouch) {
      renderer.setOrientation(baseOrientation);
    }
  }

  if (holdGlyph >= 0) {
    launchWordLookupAt(std::move(holdText), holdGlyph);
    return;
  }

  if (wordSelect_ && handleWordSelectInput()) return;

  if (showBookmarkMessage && (millis() - bookmarkMessageTime) >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showBookmarkMessage = false;
    requestUpdate();
  }

  if (automaticPageTurnActive) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back) || touchMenu) {
      automaticPageTurnActive = false;
      requestUpdate();
      return;
    }
    if (RenderLock::peek()) {
      lastPageTurnTime = millis();
      return;
    }
    if ((millis() - lastPageTurnTime) >= pageTurnDuration) {
      if (viewMode == ViewMode::PanelZoom) {
        nextPanel();
      } else {
        nextPage();
      }
      lastPageTurnTime = millis();
      return;
    }
    return;
  }

  // Handle short power button press for word lookup (mirrors the EPUB reader's shortcut).
  // Skipped when Down is also released so the screenshot combo doesn't trigger it.
  if ((SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::WORD_LOOKUP &&
       mappedInput.wasReleased(MappedInputManager::Button::Power) && !gpio.wasReleased(HalGPIO::BTN_DOWN)) ||
      ReaderUtils::wordLookupSideToggle(mappedInput)) {
    launchWordLookupCurrentView();
    return;
  }

  if (viewMode == ViewMode::TextOverlay) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      viewMode = ViewMode::PanelZoom;
      requestUpdate();
      return;
    }
    // A tap anywhere on the overlay looks the word up, matching Confirm: the overlay is a
    // full-screen text panel, so there is no page-turn zone here to keep clear.
    int overlayTapX = 0;
    int overlayTapY = 0;
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        (mappedInput.hasTouch() && mappedInput.wasScreenTapped(overlayTapX, overlayTapY))) {
      launchWordLookup();
      return;
    }
    return;
  }

  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    activityManager.goToFileBrowser(book ? book->getFolder() : "");
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    if (viewMode == ViewMode::PanelZoom) {
      if (currentPageHasImage && !panelsOnlyMode) {
        currentPanel = -1;
        viewMode = ViewMode::FullPage;
        requestUpdate();
      } else {
        onGoHome();
      }
      return;
    }
    onGoHome();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || touchMenu) {
    if (ignoreNextConfirmRelease) {
      ignoreNextConfirmRelease = false;
    } else if (viewMode == ViewMode::PanelZoom || viewMode == ViewMode::FullPage) {
      launchMenu();
      return;
    }
  }

  // Resolve against the reader's OWN orientation, not the live one: a panel whose aspect does not
  // match the screen is displayed rotated 90 degrees, and that rotation stays applied while the
  // panel is on screen. Following it inverted the front pair, so the same press stepped forward on
  // an upright panel and back on a rotated one -- observed on device as walking backwards through
  // a page's panels (orient flipping 0 <-> 3 between presses, the front pair swapping with it).
  //
  // With the toggle on, the left button advances -- into the panels and on through the pages --
  // and the right button goes back, matching how manga is read.
  const bool reversedTurn = SETTINGS.reversePageTurn != 0;
  auto [prevTriggered, nextTriggered, fromTilt] =
      ReaderUtils::detectPageTurnForOrientation(mappedInput, reversedTurn, SETTINGS.orientation);
  prevTriggered = prevTriggered || touch.prev;
  nextTriggered = nextTriggered || touch.next;
  if (!prevTriggered && !nextTriggered) {
    // Idle tick: after a dwell, warm the pixel cache the user is most likely to need next so the
    // render hits it instead of a fresh JPEG decode. Dwell gates and ordering rationale live with
    // the PREFETCH_DWELL_MS / FIRST_PANEL_PREFETCH_DWELL_MS constants above. In short: on a paneled
    // full page warm the first panel first (shorter dwell), then the next page; pages without crops
    // go straight to the next page; inside panel-zoom warm the next panel.
    if (viewMode == ViewMode::FullPage) {
      const unsigned long dwell = millis() - fullPageRenderedMs;
      if (pageHasPanelCrops && !firstPanelPrefetched && dwell > FIRST_PANEL_PREFETCH_DWELL_MS) {
        prefetchPanelCache(firstPanelWithCrop);
      } else if (!nextPagePrefetched && dwell > PREFETCH_DWELL_MS) {
        prefetchNextPageCache();
      }
    } else if (viewMode == ViewMode::PanelZoom && (millis() - panelRenderedMs) > PREFETCH_DWELL_MS) {
      if (panelGrayPending) {
        // Dwelled on a panel still showing the fast BW-only image: upgrade it to full 4-level gray.
        // Clear panelGrayPending as we hand off so this issues requestUpdate() exactly once per
        // dwell instead of every idle tick until the render task starts the upgrade. Takes priority
        // over the speculative next-panel prefetch below -- this is the panel the reader is actually
        // looking at. (Shares the PREFETCH_DWELL_MS gate; tunable.)
        panelGrayPending = false;
        panelGrayUpgrade = true;
        requestUpdate();
      } else if (!nextPanelPrefetched) {
        prefetchPanelCache(currentPanel + 1);
      }
    }
    return;
  }

  if (handleEndOfBookPageTurn(prevTriggered, nextTriggered)) return;

  if (viewMode == ViewMode::PanelZoom) {
    if (nextTriggered) nextPanel();
    if (prevTriggered) prevPanel();
  } else {
    if (nextTriggered) {
      // Only enter panel-zoom if a real crop file exists for the first panel.
      // Full-page panels (cover, splash pages) have no crop -- they'd just
      // render the same full image in panel-zoom, requiring an extra click.
      // Probed once per page in loadCurrentPagePanels(); the old per-press
      // Storage.exists() here was an SD transaction inside the input path.
      if (pageHasPanelCrops) {
        // Fresh panel-zoom entry: clear any deferred gray flags left over from a prior panel-zoom
        // session (see nextPanel()), so the first panel renders BW then defers rather than being
        // upgraded before its BW pass runs.
        panelGrayPending = false;
        panelGrayUpgrade = false;
        currentPanel = firstPanelWithCrop;
        viewMode = ViewMode::PanelZoom;
        requestUpdate();
      } else {
        nextPage();
      }
    }
    if (prevTriggered) prevPage();
  }
}

void MangaReaderActivity::render(RenderLock&&) {
  if (!book) return;

  if (outlineOnlyUpdate_) {
    outlineOnlyUpdate_ = false;
    if (updateOutlineInPlace()) return;
  }
  // Any other render repaints the screen; a page render re-arms both in drawWordOutline().
  pageInFramebuffer_ = false;
  drawnOutline_.clear();

  if (renderEndOfBook()) return;

  switch (viewMode) {
    case ViewMode::FullPage:
      renderFullPage();
      break;
    case ViewMode::PanelZoom:
      renderPanelZoom();
      break;
    case ViewMode::TextOverlay:
      renderTextOverlay();
      break;
  }

  saveProgress();

  if (pendingScreenshot) {
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }

  if (showBookmarkMessage) {
    GUI.drawPopup(renderer, bookmarkRemoved ? tr(STR_BOOKMARK_REMOVED) : tr(STR_BOOKMARK_ADDED));
  }
}

MangaReaderActivity::FullPageGeom MangaReaderActivity::computeFullPageGeom(const int imgWidth, const int imgHeight,
                                                                           int screenW, int screenH) {
  FullPageGeom g;

  // Rotate when the page's aspect doesn't match the screen's -- same
  // fill-the-screen behavior as panel-zoom (renderPanelZoom): this lets a
  // portrait manga page fill a landscape-oriented screen edge-to-edge
  // instead of shrinking to a small centered box. The user tilts the
  // device to read a rotated page. Pure math (no renderer access, so the
  // lock-free prefetch worker can call it): a 90-degree orientation change
  // is exactly a screen-dim swap, which is what the applying wrapper's
  // setOrientation((o+3)%4) produces via getScreenWidth/Height.
  const bool screenIsPortrait = screenH > screenW;
  const bool pageIsLandscape = imgWidth > imgHeight;
  g.rotated = screenIsPortrait == pageIsLandscape;
  if (g.rotated) {
    std::swap(screenW, screenH);
  }
  g.screenW = screenW;
  g.screenH = screenH;

  g.destWidth = imgWidth;
  g.destHeight = imgHeight;
  const float ratio = static_cast<float>(imgWidth) / static_cast<float>(imgHeight);
  const float screenRatio = static_cast<float>(g.screenW) / static_cast<float>(g.screenH);
  if (imgWidth > g.screenW || imgHeight > g.screenH) {
    // Derive the fitted size from the aspect ratio, then centre with whatever is left over.
    // Doing it the other way round -- truncating the margin and calling "screen minus twice the
    // margin" the size -- made the box up to 2px LARGER than the real fit, because the truncated
    // margin is counted twice. The decoder fits properly, so it wrote a pixel cache the reader
    // then rejected on every read (device log: "Cache dimension mismatch: 480x682 vs 480x684"),
    // and every page decoded from scratch three times per turn instead of once ever.
    if (ratio > screenRatio) {
      g.destWidth = g.screenW;
      g.destHeight = std::min(g.screenH, static_cast<int>(g.screenW / ratio + 0.5f));
      g.y = (g.screenH - g.destHeight) / 2;
    } else {
      g.destHeight = g.screenH;
      g.destWidth = std::min(g.screenW, static_cast<int>(g.screenH * ratio + 0.5f));
      g.x = (g.screenW - g.destWidth) / 2;
    }
  } else {
    g.x = (g.screenW - imgWidth) / 2;
    g.y = (g.screenH - imgHeight) / 2;
  }
  return g;
}

MangaReaderActivity::FullPageGeom MangaReaderActivity::adoptFullPageGeom(FullPageGeom g) {
  g.savedOrientation = renderer.getOrientation();
  if (g.rotated) {
    renderer.setOrientation(static_cast<GfxRenderer::Orientation>((g.savedOrientation + 3) % 4));
  }
  return g;
}

MangaReaderActivity::FullPageGeom MangaReaderActivity::applyFullPageGeometry(const int imgWidth, const int imgHeight) {
  return adoptFullPageGeom(
      computeFullPageGeom(imgWidth, imgHeight, renderer.getScreenWidth(), renderer.getScreenHeight()));
}

bool MangaReaderActivity::fullPageGeomFromCache(const PixelCacheIO::Reader& cache, FullPageGeom& out) {
  if (!cache.isOpen()) return false;
  const int cw = cache.width(), ch = cache.height();

  // The header holds the fitted on-screen size, not the source size -- so the geometry has to be
  // re-derived from it, and only accepted when the derivation is provably the same one the
  // decode made. Two properties make that possible:
  //
  //  * The fit preserves aspect, and rounding is monotonic, so destWidth > destHeight exactly
  //    when imgWidth > imgHeight -- which is the only thing computeFullPageGeom needs the source
  //    for (its rotate test). A square cache is the one case that loses the answer (a rotated
  //    landscape page can fit to a square), so bail there.
  //  * Feeding the fitted size back through computeFullPageGeom is a fixed point when the cache
  //    belongs to this screen: it already fits, so it comes back unscaled and centred the same
  //    way. A cache fitted to different screen dimensions generally is not.
  //
  // The fixed point alone would also accept a cache that is merely smaller than the screen (a
  // stale one written under a different fit rule would sit centred and undersized forever), so
  // require it to touch a screen edge as well -- i.e. to actually be a fill. A page whose source
  // is smaller than the screen legitimately does not, and simply falls back to the probe.
  if (cw == ch) return false;

  const FullPageGeom g = computeFullPageGeom(cw, ch, renderer.getScreenWidth(), renderer.getScreenHeight());
  if (g.destWidth != cw || g.destHeight != ch) return false;
  if (cw != g.screenW && ch != g.screenH) return false;

  LOG_DBG("MANGA", "Geometry from cache header: %dx%d at %d,%d (rot %d) -- source header not read", cw, ch, g.x, g.y,
          (int)g.rotated);
  out = adoptFullPageGeom(g);
  return true;
}

void MangaReaderActivity::renderFullPage() {
  renderer.clearScreen();
  // Cleared up front, not just assigned once the geometry works out: the paths below display a
  // placeholder and return early (no image, no decoder, an unreadable header), and a stale true
  // from the last rotated page would have touch resolving rotated over an unrotated screen. The
  // geometry sets the real value further down.
  displayedRotated_ = false;
  displayedMap_.valid = false;

  std::string imgPath = book->getPageImagePath(currentPage);
  if (imgPath.empty()) {
    renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() / 2, tr(STR_PAGE_LOAD_ERROR), true);
    renderer.displayBuffer();
    return;
  }

  // Use the image decoder (supports JPG/PNG/BMP) for proper grayscale rendering.
  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(imgPath);
  if (!decoder) {
    renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() / 2, tr(STR_PAGE_LOAD_ERROR), true);
    renderer.displayBuffer();
    return;
  }

  // Only JPEG/PNG pages stream a .2bp pixel cache; the BMP converter renders straight to the
  // framebuffer and never writes one, so for a BMP page the cache would be a guaranteed miss on
  // every plane -- an SD open attempt for nothing. Skip it entirely for BMP (mirrors the
  // panelCropIsBmp guard in renderPanelZoom). Note the 1-bit BMP fast path below returns before
  // any cache use anyway; this covers the >=8-bit BMP page case.
  const bool useCache = !FsHelpers::hasBmpExtension(imgPath);
  const std::string cachePath =
      useCache ? book->getCachePath() + "/page_" + std::to_string(currentPage) + ".2bp" : std::string();

  // One open serves this whole page turn: the header below, then all three render passes. On
  // device an open costs ~85ms (SdFat walks a FAT directory holding a chapter's worth of cache
  // entries), so the open-per-pass this replaces was the single largest cost of a page turn --
  // larger than any pass's own read. Kept alive until the last pass has run.
  PixelCacheIO::Reader cache;
  if (useCache) cache.open(cachePath);

  // A warm cache (a revisited page, or the next page warmed by the idle prefetch) already knows
  // the on-screen size, so the source image never has to be opened for its dimensions -- that
  // probe is an SD open plus a JPEG marker walk, measured at 86ms of every page turn, spent to
  // recompute a number the passes below then go on to supply themselves for free. Falls through
  // to the probe whenever the cache can't prove its geometry (see fullPageGeomFromCache).
  FullPageGeom g;
  if (!fullPageGeomFromCache(cache, g)) {
    ImageDimensions dims = {static_cast<int16_t>(currentPageBmpWidth), static_cast<int16_t>(currentPageBmpHeight)};
    if ((dims.width <= 0 || dims.height <= 0) &&
        (!decoder->getDimensions(imgPath, dims) || dims.width <= 0 || dims.height <= 0)) {
      renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() / 2, tr(STR_PAGE_LOAD_ERROR), true);
      renderer.displayBuffer();
      return;
    }
    g = applyFullPageGeometry(dims.width, dims.height);
  }

  const auto savedOrientation = static_cast<GfxRenderer::Orientation>(g.savedOrientation);
  const bool rotatePage = g.rotated;
  displayedRotated_ = rotatePage;
  displayedMap_ = HoldMap{true,
                          g.x,
                          g.y,
                          g.destWidth,
                          g.destHeight,
                          0,
                          0,
                          book ? book->getPageImgWidth(currentPage) : 0,
                          book ? book->getPageImgHeight(currentPage) : 0};
  displayedMap_.valid = displayedMap_.sw > 0 && displayedMap_.sh > 0;
  const int x = g.x, y = g.y;
  const int destWidth = g.destWidth, destHeight = g.destHeight;
  const int screenW = g.screenW, screenH = g.screenH;

  RenderConfig config;
  config.x = x;
  config.y = y;
  config.maxWidth = screenW;
  config.maxHeight = screenH;
  config.useGrayscale = true;
  config.useDithering = true;
  config.cachePath = cachePath;

  // 1-bit BMP fast path: pure black/white content needs no 4-level gray refresh, so render a
  // single BW pass and one FAST wave -- no grayscale planes, no displayGrayBuffer, no pixel
  // cache. Roughly halves the page render for line-art manga (the whole point of BMP support).
  if (currentPageBwOnly) {
    renderer.setRenderMode(GfxRenderer::BW);
    decoder->decodeToFramebuffer(imgPath, renderer, config);

    drawWordOutline();

    char bwStatus[32];
    snprintf(bwStatus, sizeof(bwStatus), "%u/%u", currentPage + 1, book->getPageCount());
    const int bwStatusW = renderer.getTextWidth(SMALL_FONT_ID, bwStatus);
    const int bwStatusX = screenW - bwStatusW - 4;
    const int bwStatusY = screenH - renderer.getLineHeight(SMALL_FONT_ID) - 2;
    renderer.fillRect(bwStatusX - 2, bwStatusY - 1, bwStatusW + 4, renderer.getLineHeight(SMALL_FONT_ID) + 2, false);
    renderer.drawText(SMALL_FONT_ID, bwStatusX, bwStatusY, bwStatus, true);

    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    if (rotatePage) renderer.setOrientation(savedOrientation);
    // Arm the next-page prefetch like the grayscale path (prefetch itself skips BMP -- see there).
    nextPagePrefetched = false;
    fullPageRenderedMs = millis();
    return;
  }

  // BW pass — a page visited before (or warmed by the idle prefetch) has its decoded pixels
  // cached on SD; render those directly and skip the JPEG decode entirely. Otherwise decode to
  // framebuffer AND stream the pixel cache to disk (config.cachePath) so the two grayscale passes
  // below can read the already-decoded pixels back instead of re-running the full JPEG decode.
  // Confirmed on a real device: without the cache, every manga page turn ran the decode 3 times
  // (BW + LSB + MSB), each a full JPEG parse/IDCT/scale -- the dominant cost of "turning pages in
  // manga is slow". ImageBlock (regular EPUB images) already had this same cache-read
  // optimization; manga bypassed ImageBlock entirely and never got it.
  if (!cache.render(renderer, x, y, destWidth, destHeight)) {
    // Cold page (or a cache that failed to open or read): decode, which also streams the cache to
    // disk. Open it afterwards so the two grayscale passes still read pixels instead of decoding
    // the JPEG twice more -- that hand-off is the entire reason the cache exists. Any handle from
    // a failed read is stale by now (the decode just rewrote the file), hence the close first.
    cache.close();
    decoder->decodeToFramebuffer(imgPath, renderer, config);
    if (useCache) cache.open(cachePath);
  }

  drawWordOutline();

  // Status bar: page number
  char statusBuf[32];
  snprintf(statusBuf, sizeof(statusBuf), "%u/%u", currentPage + 1, book->getPageCount());
  int statusW = renderer.getTextWidth(SMALL_FONT_ID, statusBuf);
  int statusX = screenW - statusW - 4;
  int statusY = screenH - renderer.getLineHeight(SMALL_FONT_ID) - 2;
  renderer.fillRect(statusX - 2, statusY - 1, statusW + 4, renderer.getLineHeight(SMALL_FONT_ID) + 2, false);
  renderer.drawText(SMALL_FONT_ID, statusX, statusY, statusBuf, true);

  // Display with grayscale: BW first, then LSB/MSB planes for 4-level gray.
  renderer.storeBwBuffer();
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);

  // Read the pixels the BW pass just cached instead of re-decoding the JPEG. Falls back to a
  // real decode if the cache write failed (e.g. under memory pressure) or wasn't enabled.
  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  if (!cache.render(renderer, x, y, destWidth, destHeight)) {
    decoder->decodeToFramebuffer(imgPath, renderer, config);
  }
  renderer.copyGrayscaleLsbBuffers();

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  if (!cache.render(renderer, x, y, destWidth, destHeight)) {
    decoder->decodeToFramebuffer(imgPath, renderer, config);
  }
  renderer.copyGrayscaleMsbBuffers();

  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.restoreBwBuffer();

  if (rotatePage) {
    renderer.setOrientation(savedOrientation);
  }

  // Arm the idle prefetch of the next page's pixel cache (see loop()).
  nextPagePrefetched = false;
  fullPageRenderedMs = millis();
}

void MangaReaderActivity::prefetchNextPageCache() {
  // Don't even build a job while a render is active/queued: the worker would only defer via its
  // cancel probe, so posting now is pure churn -- a task wake-up plus fresh path strings every
  // idle tick for the render's whole duration (heap-fragmentation smell). Retry next tick.
  if (RenderLock::peek()) return;
  if (!book) {
    nextPagePrefetched = true;
    return;
  }
  const uint32_t upcomingPage = currentPage + 1;
  if (upcomingPage >= book->getPageCount()) {
    nextPagePrefetched = true;
    return;
  }
  // BMP pages don't stream a .2bp cache (the BMP converter renders straight to the framebuffer),
  // so there's nothing to warm. BMP is uncompressed, so the page-turn decode is cheap anyway.
  const std::string imgPath = book->getPageImagePath(upcomingPage);
  if (imgPath.empty() || FsHelpers::hasBmpExtension(imgPath)) {
    nextPagePrefetched = true;
    return;
  }
  // Everything that touches the SD or decodes runs on the prefetch worker -- this used to decode
  // synchronously right here on the input-polling task, which starved gpio.update() and dropped
  // button presses for the whole ~1s decode (see the worker doc block in the header).
  PrefetchJob job;
  job.isPanel = false;
  job.gen = pageGeneration;
  job.imgPath = imgPath;
  job.cachePath = book->getCachePath() + "/page_" + std::to_string(upcomingPage) + ".2bp";
  postPrefetchJob(std::move(job));  // screen dims for the geometry math are captured in the post
}

void MangaReaderActivity::renderPanelZoom() {
  displayedRotated_ = false;  // see renderFullPage(): every early return below falls back to it
  displayedMap_.valid = false;
  // Deferred-grayscale phase (see the panelGray* flags in the header). true means the BW image is
  // already displayed on the e-ink from the initial entry, so this pass skips the BW refresh wave
  // and only adds the 4-level gray wave; false is a fresh entry that shows BW and re-defers the gray
  // wave. Both phases still repopulate the BW *framebuffer* below (a cheap warm-cache read, not an
  // e-ink wave): the upgrade needs it as the base for displayGrayBuffer, and rebuilding it keeps the
  // stored snapshot correct independent of any transient overlay (e.g. a bookmark popup) drawn into
  // the framebuffer after the fresh entry. Consume the request up front. Clear panelGrayPending on
  // every entry: the fresh-entry branch below re-arms it only on success, so any fallback path
  // leaves nothing pending for loop() to upgrade.
  const bool grayUpgrade = panelGrayUpgrade.exchange(false);
  panelGrayPending = false;

  renderer.clearScreen();

  if (currentPanel < 0 || currentPanel >= static_cast<int>(panels.size())) {
    renderFullPage();
    return;
  }

  // Crop known missing/invalid from an earlier probe: fall back without touching the SD.
  if (panelDims[currentPanel].w < 0) {
    renderFullPage();
    return;
  }

  const std::string panelImgPath = panelCropPath(currentPanel);
  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(panelImgPath);
  if (!decoder) {
    // Defensive: getDecoder returns null only for an unsupported extension, and crop paths are
    // always .jpg/.bmp -- shouldn't happen. Fall back to the full page without poisoning the
    // dims slot.
    renderFullPage();
    return;
  }

  // Crop dimensions are static per file: probe the JPEG header only the first time; a failure
  // is cached as -1 so a missing/corrupt crop doesn't re-probe on every entry. Prefetch fills
  // this slot too, so a prefetched panel enters without any header parse.
  if (panelDims[currentPanel].w == 0) {
    ImageDimensions dims = {0, 0};
    if (!decoder->getDimensions(panelImgPath, dims) || dims.width <= 0 || dims.height <= 0) {
      panelDims[currentPanel] = {-1, -1};
      renderFullPage();
      return;
    }
    panelDims[currentPanel] = {dims.width, dims.height};
  }

  // Panel-zoom is genuinely in use (a crop resolved and will render): arm the idle prefetches
  // (see loop()). Armed only here so books whose crops never render don't trigger speculative
  // prefetch work.
  panelPrefetchArmed = true;

  const PanelGeom g = applyPanelGeometry(panelDims[currentPanel].w, panelDims[currentPanel].h);
  const bool rotatePanel = g.rotated;
  displayedRotated_ = rotatePanel;
  {
    const auto& p = panels[currentPanel];
    displayedMap_ = HoldMap{p.cropW > 0 && p.cropH > 0, g.x, g.y, g.fitW, g.fitH, p.cropX, p.cropY, p.cropW, p.cropH};
  }
  const auto savedOrientation = static_cast<GfxRenderer::Orientation>(g.savedOrientation);
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const int fitW = g.fitW, fitH = g.fitH;
  const int x = g.x, y = g.y;

  // 1-bit BMP panels render BW-only (single fast wave, no gray planes, no .2bp cache) -- the same
  // fast path a mono full page uses. Grayscale (JPEG or >=8-bit BMP) panels take the cached
  // BW+LSB+MSB path below.
  const bool bwOnly = panelsBwOnly;
  // Only JPEG panels stream a .2bp pixel cache; the BMP converter renders straight to the
  // framebuffer and never writes one, so for any BMP crop the cache would be a guaranteed miss --
  // an SD open attempt per plane for nothing. Skip the cache entirely for BMP.
  const bool useCache = !panelCropIsBmp;
  const std::string cachePath =
      useCache ? book->getCachePath() + "/p" + std::to_string(currentPage) + "_" + std::to_string(currentPanel) + ".2bp"
               : std::string();
  RenderConfig config;
  config.x = x;
  config.y = y;
  config.maxWidth = fitW;
  config.maxHeight = fitH;
  config.useExactDimensions = true;
  config.allowUpscale = true;  // panel zoom: a small mono crop should fill the screen
  config.useGrayscale = !bwOnly;
  config.useDithering = !bwOnly;
  config.cachePath = cachePath;

  // One open for every pass this render performs -- the BW rebuild here plus, on a deferred gray
  // upgrade, both plane passes below. See the Reader comment in renderFullPage(): an open is
  // ~85ms, so an upgrade under the old open-per-pass API spent ~255ms of its ~400ms budget just
  // reopening the same crop. Not opened for a BW-only panel, which never touches the cache.
  PixelCacheIO::Reader cache;
  if (useCache && !bwOnly) cache.open(cachePath);

  // Populate the BW framebuffer. BW-only decodes the 1-bit BMP straight to BW; the grayscale path
  // renders from the .2bp pixel cache when warm (revisited/prefetched JPEG panel) and only decodes
  // on a cold pass (or always, for a cacheless BMP crop), same as renderFullPage().
  if (bwOnly) {
    renderer.setRenderMode(GfxRenderer::BW);
    decoder->decodeToFramebuffer(panelImgPath, renderer, config);
  } else if (!cache.render(renderer, x, y, fitW, fitH)) {
    // Cold crop: the decode streams the cache, so reopen it for the plane passes below (same
    // hand-off as renderFullPage()).
    cache.close();
    decoder->decodeToFramebuffer(panelImgPath, renderer, config);
    if (useCache) cache.open(cachePath);
  }

  drawWordOutline();

  // Panel indicator and status
  char statusBuf[48];
  snprintf(statusBuf, sizeof(statusBuf), "%d/%d  %u/%u", currentPanel + 1, (int)panels.size(), currentPage + 1,
           book->getPageCount());
  int statusW = renderer.getTextWidth(SMALL_FONT_ID, statusBuf);
  int statusX = screenW - statusW - 4;
  int statusY = screenH - renderer.getLineHeight(SMALL_FONT_ID) - 2;
  renderer.fillRect(statusX - 2, statusY - 1, statusW + 4, renderer.getLineHeight(SMALL_FONT_ID) + 2, false);
  renderer.drawText(SMALL_FONT_ID, statusX, statusY, statusBuf, true);

  // "Panels" hint — always shown in panel-zoom, indicates this mode and
  // that Confirm opens the full reader menu (with Word Lookup, Translate, etc.)
  {
    const char* hint = tr(STR_PANELS_MODE_HINT);
    renderer.fillRect(2, statusY - 1, renderer.getTextWidth(SMALL_FONT_ID, hint) + 4,
                      renderer.getLineHeight(SMALL_FONT_ID) + 2, false);
    renderer.drawText(SMALL_FONT_ID, 4, statusY, hint, true);
  }

  if (bwOnly) {
    // Single black-and-white wave; no grayscale planes, nothing to defer.
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  } else if (!grayUpgrade) {
    // Fresh entry: show the BW image with one FAST wave and defer the slower 4-level gray wave to a
    // dwell (loop() requests it once the reader stops stepping). Rapid panel-to-panel navigation
    // thus pays a single wave per panel instead of two. The BW pass above also streamed the .2bp
    // cache (for JPEG crops), so the deferred upgrade reads those pixels back instead of re-decoding.
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    panelGrayPending = true;
  } else {
    // Deferred upgrade: the BW image is already on screen (initial entry showed it), so skip the BW
    // wave entirely. Store the BW framebuffer, rebuild the LSB/MSB planes from the now-warm pixel
    // cache, and show the combined 4-level gray in one wave. Identical plane-build to the old
    // non-deferred path, minus that first BW wave.
    if (!renderer.storeBwBuffer()) {
      // OOM saving the BW framebuffer. Unlike the non-deferred path we can degrade cleanly here: the
      // fast BW panel is already on screen, so just skip the gray upgrade (no clearScreen, no plane
      // build, no restoreBwBuffer of chunks that were never stored) and leave the BW image up.
      LOG_ERR("MRA", "storeBwBuffer OOM; keeping on-screen BW panel, skipping gray upgrade");
    } else {
      // Read back the pixels the BW pass cached instead of re-decoding the JPEG for each grayscale
      // plane -- same approach as renderFullPage(), see the comment there for the full rationale.
      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
      if (!cache.render(renderer, x, y, fitW, fitH)) {
        decoder->decodeToFramebuffer(panelImgPath, renderer, config);
      }
      renderer.copyGrayscaleLsbBuffers();

      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
      if (!cache.render(renderer, x, y, fitW, fitH)) {
        decoder->decodeToFramebuffer(panelImgPath, renderer, config);
      }
      renderer.copyGrayscaleMsbBuffers();

      renderer.displayGrayBuffer();
      renderer.setRenderMode(GfxRenderer::BW);
      renderer.restoreBwBuffer();
    }
  }

  if (rotatePanel) {
    renderer.setOrientation(savedOrientation);
  }

  // Arm the idle prefetch/upgrade dwell for the panel the user steps to next (see loop()) -- but
  // only from a fresh render. Re-arming on the deferred gray upgrade would restart the 400ms window
  // and needlessly delay the next-panel prefetch, which is gated on the same panelRenderedMs;
  // leaving it lets prefetch start as soon as the upgrade finishes.
  if (!grayUpgrade) {
    panelRenderedMs = millis();
    nextPanelPrefetched = (currentPanel + 1 >= static_cast<int>(panels.size()));
  }
}

MangaReaderActivity::PanelGeom MangaReaderActivity::computePanelGeom(const int imgWidth, const int imgHeight,
                                                                     int screenW, int screenH,
                                                                     const bool rotatePanels) {
  PanelGeom g;

  // With Rotate Panels enabled, rotate when the crop's aspect does not match the screen. This
  // maximizes readable panel size on the small display; users rotate the physical device to read
  // it. When disabled, fit every crop inside the reader's configured orientation.
  const bool screenIsPortrait = screenH > screenW;
  const bool panelIsLandscape = imgWidth > imgHeight;
  g.rotated = rotatePanels && screenIsPortrait == panelIsLandscape;
  if (g.rotated) {
    std::swap(screenW, screenH);
  }

  // Panel crops always upscale to use as much of the available screen as their aspect allows.
  // Exact dimensions make the decoder bypass its normal upscale-disabled fit-or-shrink logic.
  const float scale = std::min(static_cast<float>(screenW) / imgWidth, static_cast<float>(screenH) / imgHeight);
  g.fitW = std::max(1, static_cast<int>(imgWidth * scale + 0.5f));
  g.fitH = std::max(1, static_cast<int>(imgHeight * scale + 0.5f));
  g.x = (screenW - g.fitW) / 2;
  g.y = (screenH - g.fitH) / 2;
  return g;
}

MangaReaderActivity::PanelGeom MangaReaderActivity::applyPanelGeometry(const int imgWidth, const int imgHeight) {
  const int savedOrientation = renderer.getOrientation();
  PanelGeom g = computePanelGeom(imgWidth, imgHeight, renderer.getScreenWidth(), renderer.getScreenHeight(),
                                 SETTINGS.rotateMangaPanels != 0);
  g.savedOrientation = savedOrientation;
  if (g.rotated) {
    renderer.setOrientation(static_cast<GfxRenderer::Orientation>((savedOrientation + 3) % 4));
  }
  return g;
}

// Idle-time twin of renderPanelZoom's decode: warms the panel's .2bp pixel cache (and its
// dimension slot) so entering the panel costs a cache read instead of a JPEG decode. This is
// only the loop-side gate; the SD probes and the decode itself run on the prefetch worker.
void MangaReaderActivity::prefetchPanelCache(const int panelIdx) {
  // See prefetchNextPageCache: no job building while a render is active/queued.
  if (RenderLock::peek()) return;
  std::atomic<bool>& doneFlag = (viewMode == ViewMode::FullPage) ? firstPanelPrefetched : nextPanelPrefetched;
  if (!book || panelIdx < 0 || panelIdx >= static_cast<int>(panels.size())) {
    doneFlag = true;
    return;
  }
  // BMP panels don't stream a .2bp cache (the BMP converter renders straight to the framebuffer),
  // and mono panels render BW-only on entry anyway -- nothing to warm, and probing the (never
  // written) .2bp would re-run this every idle tick. BMP decode is cheap, so skip the prefetch.
  if (panelCropIsBmp) {
    doneFlag = true;
    return;
  }
  PrefetchJob job;
  job.isPanel = true;
  job.isFirstPanel = (viewMode == ViewMode::FullPage);
  job.panelIdx = panelIdx;
  job.gen = pageGeneration;
  // Paths are built HERE, on the loop task that owns currentPage/panelCropIsBmp -- the worker
  // must never call panelCropPath() itself, since a page turn mid-job would make it read
  // half-updated state. A stale captured path at worst writes a correct cache for the old page.
  job.imgPath = panelCropPath(panelIdx);
  job.cachePath = book->getCachePath() + "/p" + std::to_string(currentPage) + "_" + std::to_string(panelIdx) + ".2bp";
  postPrefetchJob(std::move(job));
}

// ---- Background prefetch worker (see the doc block in the header for the design invariants) ----

void MangaReaderActivity::prefetchTaskTrampoline(void* param) {
  static_cast<MangaReaderActivity*>(param)->prefetchTaskLoop();
}

void MangaReaderActivity::prefetchTaskLoop() {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (prefetchExitRequested) break;
    if (!prefetchBusy) continue;  // spurious/coalesced notification with no job posted
    if (prefetchJob.isPanel) {
      workerWarmPanel();
    } else {
      workerWarmNextPage();
    }
    // Release the job slot LAST: the loop task treats busy==false as "job+result are mine".
    prefetchBusy = false;
  }
  // Self-terminate. The exited flag is the join signal for onExit(); nothing on this task may
  // touch the activity after setting it.
  prefetchTaskExited = true;
  vTaskDelete(nullptr);
}

bool MangaReaderActivity::prefetchShouldCancel(const void* selfPtr) {
  const auto* self = static_cast<const MangaReaderActivity*>(selfPtr);
  // Exit: the activity is tearing down. peek: a real render holds (or has just taken) the
  // rendering mutex -- the foreground work the background warm must never compete with.
  return self->prefetchExitRequested || RenderLock::peek();
}

void MangaReaderActivity::postPrefetchJob(PrefetchJob&& job) {
  // Single-slot queue: refuse while the worker owns the slot (busy) or a finished result hasn't
  // been applied yet (pending) -- overwriting either would break the ownership ping-pong. The
  // caller's done-flag stays false, so the dwell logic simply retries on a later idle tick.
  // No worker (task creation failed at onEnter): prefetching is disabled; mark nothing.
  if (!prefetchTaskHandle || prefetchBusy || prefetchResult.pending) return;
  prefetchJob = std::move(job);
  // Base dims captured once at onEnter -- no renderer read here, so posting can never race a
  // render task's transient orientation change (see the header note on baseScreenW/baseScreenH).
  prefetchJob.screenW = baseScreenW;
  prefetchJob.screenH = baseScreenH;
  prefetchJob.rotatePanels = SETTINGS.rotateMangaPanels != 0;
  prefetchResult = PrefetchResult{};
  prefetchBusy = true;
  xTaskNotifyGive(prefetchTaskHandle);
}

void MangaReaderActivity::applyPrefetchResult() {
  // Loop task only. Worker results are applied here -- never by the worker itself -- so every
  // panelDims write keeps the established "loop task writes under RenderLock" discipline, and
  // the worker can stay lock-free (see the header: onExit() joins it while HOLDING RenderLock).
  if (prefetchBusy || !prefetchResult.pending) return;
  const bool genOk = (prefetchJob.gen == pageGeneration);
  if (prefetchResult.dimsValid && genOk) {
    // NEVER block the input task on the rendering mutex: a blocking acquire here stalls loop()
    // -- and button polling with it -- for a render's whole duration (observed on-device: a
    // 2.7s loop stall when a cancelled warm's dims were applied during a 3.3s panel decode).
    // A peek()-then-lock guard still had a TOCTOU window (the render task can take the mutex
    // between the check and the constructor), so use the non-blocking acquire: either we hold
    // the lock right now, or the whole application defers to a later tick -- the result stays
    // pending, and postPrefetchJob refuses new jobs while it is, so nothing is lost.
    RenderLock lock{RenderLock::Try{}};
    if (!lock.held()) return;
    if (prefetchJob.panelIdx >= 0 && prefetchJob.panelIdx < static_cast<int>(panelDims.size())) {
      panelDims[prefetchJob.panelIdx] = prefetchResult.dims;
    }
  }
  if (prefetchResult.completed && genOk) {
    std::atomic<bool>& doneFlag = prefetchJob.isPanel
                                      ? (prefetchJob.isFirstPanel ? firstPanelPrefetched : nextPanelPrefetched)
                                      : nextPagePrefetched;
    doneFlag = true;
  }
  // A stale-generation or deferred (cancelled) job marks nothing: the flags for the current page
  // are still false, so the dwell logic naturally re-posts a fresh job.
  prefetchResult.pending = false;
}

void MangaReaderActivity::workerWarmNextPage() {
  prefetchResult.pending = true;  // every exit path below hands a result (possibly a deferral) back
  // Defer, don't consume, while a render is active or teardown started: completed stays false,
  // so the loop retries after the next dwell.
  if (prefetchShouldCancel(this)) return;
  if (Storage.exists(prefetchJob.cachePath.c_str())) {
    prefetchResult.completed = true;  // already warm (e.g. persisted from a prior session)
    return;
  }
  if (ESP.getMaxAllocHeap() < PREFETCH_HEAP_FLOOR) {
    // Under memory pressure skip permanently for this page (the page-turn decode handles it as
    // before) -- same policy as the old in-loop prefetch.
    prefetchResult.completed = true;
    return;
  }
  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(prefetchJob.imgPath);
  ImageDimensions dims = {0, 0};
  if (!decoder || !decoder->getDimensions(prefetchJob.imgPath, dims) || dims.width <= 0 || dims.height <= 0) {
    prefetchResult.completed = true;  // undecodable page: give up like the old code did
    return;
  }
  LOG_DBG("MRA", "Prefetch worker: warming page cache %s", prefetchJob.cachePath.c_str());
  const FullPageGeom g = computeFullPageGeom(dims.width, dims.height, prefetchJob.screenW, prefetchJob.screenH);
  RenderConfig config;
  config.x = g.x;
  config.y = g.y;
  config.maxWidth = g.screenW;
  config.maxHeight = g.screenH;
  config.useGrayscale = true;
  config.useDithering = true;
  config.cacheOnly = true;
  config.shouldCancel = &prefetchShouldCancel;
  config.cancelCtx = this;
  // Decode into a private tmp and publish by rename -- see the header doc block for why writing
  // the real path directly could collide with a foreground render caching the same asset.
  const std::string tmpPath = prefetchJob.cachePath + ".tmp";
  config.cachePath = tmpPath;
  if (decoder->decodeToFramebuffer(prefetchJob.imgPath, renderer, config)) {
    // SdFat rename fails (O_CREAT|O_EXCL) if the destination exists -- exactly what we want when
    // a foreground render published the real cache first. Losing the race just drops our tmp.
    // Any OTHER rename failure (I/O error, card full) leaves no cache; the job is still marked
    // completed rather than retried: the done flags mean "stop warming this asset", NOT "cache
    // guaranteed present" -- every render path probes the cache file itself and falls back to an
    // on-demand decode, and retrying on a failing/full card would churn the worker forever.
    // Same give-up-once policy as a decode failure below; log it so it isn't silent.
    if (!Storage.rename(tmpPath.c_str(), prefetchJob.cachePath.c_str())) {
      if (!Storage.exists(prefetchJob.cachePath.c_str())) {
        LOG_ERR("MRA", "Prefetch cache publish failed: %s", prefetchJob.cachePath.c_str());
      }
      Storage.remove(tmpPath.c_str());
    }
    prefetchResult.completed = true;
  } else if (!prefetchShouldCancel(this)) {
    // Genuine decode failure (not a cancellation): give up for this page, old-code policy.
    // The converter already dropped its partial tmp.
    prefetchResult.completed = true;
  }
  // Cancelled: completed stays false -> the loop re-posts after the next dwell.
}

void MangaReaderActivity::workerWarmPanel() {
  prefetchResult.pending = true;  // every exit path below hands a result (possibly a deferral) back
  if (prefetchShouldCancel(this)) return;

  if (Storage.exists(prefetchJob.cachePath.c_str())) {
    // Pixel cache already warm (commonly persisted from a prior session), but the panelDims slot
    // may still be unprobed -- so the eventual panel entry would parse the crop header on the
    // render hot path even though the pixels are cached. Probe the header once here and hand the
    // dims back for the loop task to publish, so entry is a pure cache read. (Reading the slot to
    // skip an already-done probe would need RenderLock, which this task must never take -- so we
    // probe unconditionally: at worst one redundant header parse per panel per page, off-hot-path.)
    const ImageToFramebufferDecoder* warmDecoder = ImageDecoderFactory::getDecoder(prefetchJob.imgPath);
    // Defensive: getDecoder returns null only for an unsupported extension (decoder instances
    // are static, no allocation involved) -- leave the slot unprobed. Only a non-null decoder
    // whose header parse fails is a genuinely bad crop worth caching as -1.
    if (warmDecoder) {
      ImageDimensions warmDims = {0, 0};
      const bool warmOk =
          warmDecoder->getDimensions(prefetchJob.imgPath, warmDims) && warmDims.width > 0 && warmDims.height > 0;
      prefetchResult.dims = warmOk ? PanelCropDims{warmDims.width, warmDims.height} : PanelCropDims{-1, -1};
      prefetchResult.dimsValid = true;
    }
    prefetchResult.completed = true;
    return;
  }
  if (!Storage.exists(prefetchJob.imgPath.c_str())) {
    // Confirmed missing crop file (full-page panel like a cover/splash): cache -1 so a later
    // render entry falls back without re-probing the SD.
    prefetchResult.dims = {-1, -1};
    prefetchResult.dimsValid = true;
    prefetchResult.completed = true;
    return;
  }
  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(prefetchJob.imgPath);  // non-const: decodes
  if (!decoder) {
    // Defensive: getDecoder returns null only for an unsupported extension. Finish the job
    // without poisoning the dims slot; the panel-entry render re-probes if ever hit.
    prefetchResult.completed = true;
    return;
  }
  if (ESP.getMaxAllocHeap() < PREFETCH_HEAP_FLOOR) {
    prefetchResult.completed = true;  // memory pressure: skip for this slot, old-code policy
    return;
  }
  ImageDimensions dims = {0, 0};
  const bool dimsOk = decoder->getDimensions(prefetchJob.imgPath, dims) && dims.width > 0 && dims.height > 0;
  prefetchResult.dims = dimsOk ? PanelCropDims{dims.width, dims.height} : PanelCropDims{-1, -1};
  prefetchResult.dimsValid = true;
  if (!dimsOk) {
    prefetchResult.completed = true;
    return;
  }
  LOG_DBG("MRA", "Prefetch worker: warming panel cache %s", prefetchJob.cachePath.c_str());
  const PanelGeom g =
      computePanelGeom(dims.width, dims.height, prefetchJob.screenW, prefetchJob.screenH, prefetchJob.rotatePanels);
  RenderConfig config;
  config.x = g.x;
  config.y = g.y;
  config.maxWidth = g.fitW;
  config.maxHeight = g.fitH;
  config.useExactDimensions = true;
  config.allowUpscale = true;  // same panel geometry as the on-screen render above
  config.useGrayscale = true;
  config.useDithering = true;
  config.cacheOnly = true;
  config.shouldCancel = &prefetchShouldCancel;
  config.cancelCtx = this;
  const std::string tmpPath = prefetchJob.cachePath + ".tmp";
  config.cachePath = tmpPath;
  if (decoder->decodeToFramebuffer(prefetchJob.imgPath, renderer, config)) {
    // See workerWarmNextPage: rename-fails-because-destination-exists is the by-design lost
    // race; any other failure still completes the job (done flags mean "stop warming", not
    // "cache present" -- renders probe the file and fall back to decoding) but gets logged.
    if (!Storage.rename(tmpPath.c_str(), prefetchJob.cachePath.c_str())) {
      if (!Storage.exists(prefetchJob.cachePath.c_str())) {
        LOG_ERR("MRA", "Prefetch cache publish failed: %s", prefetchJob.cachePath.c_str());
      }
      Storage.remove(tmpPath.c_str());
    }
    prefetchResult.completed = true;
  } else if (!prefetchShouldCancel(this)) {
    prefetchResult.completed = true;  // genuine failure: give up for this slot, old-code policy
  }
  // Cancelled: completed stays false (dims, if probed, still get published) -> retried later.
}

void MangaReaderActivity::renderTextOverlay() {
  renderer.clearScreen();
  displayedRotated_ = false;  // see renderFullPage(): text is laid out in the base orientation
  displayedMap_.valid = false;

  if (currentPanel < 0 || currentPanel >= static_cast<int>(panels.size())) {
    viewMode = ViewMode::PanelZoom;
    renderPanelZoom();
    return;
  }

  const auto& panel = panels[currentPanel];
  auto& theme = UITheme::getInstance();
  auto metrics = theme.getMetrics();
  Rect screen = theme.getScreenSafeArea(renderer, true, false);

  const int jaFont = SETTINGS.getReaderFontId();
  const int lineH = renderer.getLineHeight(jaFont);
  int textY = screen.y + metrics.topPadding;

  // Header
  char headerBuf[32];
  snprintf(headerBuf, sizeof(headerBuf), tr(STR_PANEL_NUMBER_FORMAT), currentPanel + 1, (int)panels.size());
  renderer.drawText(UI_12_FONT_ID, screen.x + metrics.contentSidePadding, textY, headerBuf, true, EpdFontFamily::BOLD);
  textY += renderer.getLineHeight(UI_12_FONT_ID) + metrics.verticalSpacing;

  // Draw text blocks
  int maxWidth = screen.width - metrics.contentSidePadding * 2;
  int textX = screen.x + metrics.contentSidePadding;
  int maxY = screen.y + screen.height - renderer.getLineHeight(SMALL_FONT_ID) - 4;

  for (const auto& tb : panel.textBlocks) {
    if (textY + lineH > maxY) break;
    if (tb.text.empty()) continue;

    // Word-wrap the text block
    std::string remaining = tb.text;
    while (!remaining.empty() && textY + lineH <= maxY) {
      if (renderer.getTextWidth(jaFont, remaining.c_str()) <= maxWidth) {
        renderer.drawText(jaFont, textX, textY, remaining.c_str(), true);
        textY += lineH;
        break;
      }

      // Find break point
      std::string accum;
      const char* p = remaining.c_str();
      while (*p) {
        size_t charLen = 1;
        auto c0 = static_cast<unsigned char>(*p);
        if (c0 >= 0xF0)
          charLen = 4;
        else if (c0 >= 0xE0)
          charLen = 3;
        else if (c0 >= 0xC0)
          charLen = 2;
        std::string test = accum + std::string(p, charLen);
        if (renderer.getTextWidth(jaFont, test.c_str()) > maxWidth) break;
        accum = test;
        p += charLen;
      }

      if (accum.empty()) {
        auto c0 = static_cast<unsigned char>(remaining[0]);
        size_t cl = 1;
        if (c0 >= 0xF0)
          cl = 4;
        else if (c0 >= 0xE0)
          cl = 3;
        else if (c0 >= 0xC0)
          cl = 2;
        accum = remaining.substr(0, cl);
        remaining = remaining.substr(cl);
      } else {
        remaining = remaining.substr(accum.size());
      }

      renderer.drawText(jaFont, textX, textY, accum.c_str(), true);
      textY += lineH;
    }

    textY += lineH / 2;
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), DictIndex::isAvailable() ? tr(STR_WORD_LOOKUP) : "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void MangaReaderActivity::launchWordLookupCurrentView() {
  // In panel zoom, look up just that panel's text. In full-page view,
  // combine every panel's text on the page so lookup still works
  // without having to zoom into each panel individually.
  if (!book) return;
  if (!DictIndex::isAvailable()) {
    LOG_ERR("MANGA", "Word lookup: no Japanese dictionary (%s / %s)", DictIndex::vocabIdxPath(),
            DictIndex::vocabDatPath());
    return;
  }
  LOG_DBG("MANGA", "Word lookup: page %u panel %d", static_cast<unsigned>(currentPage), currentPanel);
  // With line geometry the page itself becomes the selector; the text-only panel remains for
  // volumes converted before line boxes existed.
  if (enterWordSelect()) return;
  std::string combined;
  for (const auto* tb : viewTextBlocks()) {
    if (!combined.empty()) combined += '\n';
    combined += tb->text;
  }
  launchWordLookupAt(std::move(combined), -1);
}

std::vector<const manga::TextBlock*> MangaReaderActivity::viewTextBlocks() const {
  // The text a lookup from this view covers, in reading order: the zoomed panel's, or every
  // panel's on a full page. launchWordLookupCurrentView() and the hold's hit test both go through
  // here, so the character a hold resolves to indexes the same text the lookup scans.
  std::vector<const manga::TextBlock*> blocks;
  const auto addBlocks = [&blocks](const manga::Panel& panel) {
    std::transform(panel.textBlocks.begin(), panel.textBlocks.end(), std::back_inserter(blocks),
                   [](const manga::TextBlock& tb) { return &tb; });
  };
  if (currentPanel >= 0 && currentPanel < static_cast<int>(panels.size())) {
    addBlocks(panels[currentPanel]);
  } else {
    std::for_each(panels.begin(), panels.end(), addBlocks);
  }
  return blocks;
}

namespace {
// Codepoints of a UTF-8 string, line breaks included.
std::vector<uint32_t> decodeUtf8(const std::string& s) {
  std::vector<uint32_t> out;
  out.reserve(s.size() / 2);
  for (size_t b = 0; b < s.size();) {
    const auto c0 = static_cast<unsigned char>(s[b]);
    size_t len = c0 < 0x80 ? 1 : (c0 & 0xE0) == 0xC0 ? 2 : (c0 & 0xF0) == 0xE0 ? 3 : 4;
    if (b + len > s.size()) break;
    uint32_t cp = len == 1 ? c0 : len == 2 ? (c0 & 0x1F) : len == 3 ? (c0 & 0x0F) : (c0 & 0x07);
    for (size_t k = 1; k < len; k++) cp = (cp << 6) | (static_cast<unsigned char>(s[b + k]) & 0x3F);
    out.push_back(cp);
    b += len;
  }
  return out;
}

bool isUprightAlnum(const uint32_t cp) {
  return (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z');
}
bool isBang(const uint32_t cp) { return cp == '!' || cp == '?' || cp == 0xFF01 || cp == 0xFF1F; }

// Start offsets (within `line`) of the cells manga lettering puts the line in: one per character,
// except that a short run of Latin letters/digits ("360") or a doubled !/? ("!!", "!?") is set
// upright in a single cell. Counting those as several cells would push every later character of
// the line one or more cells off.
std::vector<size_t> lineCells(const std::vector<uint32_t>& line) {
  std::vector<size_t> cells;
  for (size_t i = 0; i < line.size();) {
    cells.push_back(i);
    if (isUprightAlnum(line[i])) {
      size_t j = i;
      while (j < line.size() && j - i < 4 && isUprightAlnum(line[j])) j++;
      i = j;
    } else if (isBang(line[i]) && i + 1 < line.size() && isBang(line[i + 1])) {
      i += 2;
    } else {
      i++;
    }
  }
  return cells;
}
}  // namespace

bool MangaReaderActivity::holdTarget(const int x, const int y, std::string& text, int& glyph) const {
  const HoldMap& m = displayedMap_;
  if (!m.valid || m.dw <= 0 || m.dh <= 0) return false;
  if (x < m.dx || y < m.dy || x >= m.dx + m.dw || y >= m.dy + m.dh) return false;
  // Onto the page image the text boxes are measured in.
  const int px = m.sx + static_cast<int>(static_cast<int64_t>(x - m.dx) * m.sw / m.dw);
  const int py = m.sy + static_cast<int>(static_cast<int64_t>(y - m.dy) * m.sh / m.dh);

  const auto blocks = viewTextBlocks();
  int bestBlock = -1;
  int bestLine = -1;
  int bestDistance = INT_MAX;
  for (size_t b = 0; b < blocks.size(); b++) {
    const auto& lines = blocks[b]->lines;
    for (size_t l = 0; l < lines.size(); l++) {
      const auto& lb = lines[l];
      const int dx = px < lb.x ? lb.x - px : (px >= lb.x + lb.w ? px - (lb.x + lb.w - 1) : 0);
      const int dy = py < lb.y ? lb.y - py : (py >= lb.y + lb.h ? py - (lb.y + lb.h - 1) : 0);
      const int distance = std::max(dx, dy);
      if (distance < bestDistance) {
        bestDistance = distance;
        bestBlock = static_cast<int>(b);
        bestLine = static_cast<int>(l);
      }
    }
  }
  if (bestBlock < 0) return false;
  const auto& block = *blocks[bestBlock];
  const auto& lb = block.lines[bestLine];
  // Within about a character of a line the intent is unambiguous; further out (the artwork, an
  // empty corner of the bubble) is not a word at all.
  const int cellSize = block.vertical ? lb.w : lb.h;
  if (bestDistance > std::max(8, cellSize)) return false;

  // Text of the whole view, and where this line starts in it. Line breaks are not glyphs to the
  // lookup's scan, so they are not counted.
  int base = 0;
  for (int b = 0; b < bestBlock; b++) {
    const auto cps = decodeUtf8(blocks[b]->text);
    base += static_cast<int>(std::count_if(cps.begin(), cps.end(), [](const uint32_t cp) { return cp != '\n'; }));
  }
  const auto cps = decodeUtf8(block.text);
  std::vector<uint32_t> line;
  int lineIndex = 0;
  for (const uint32_t cp : cps) {
    if (cp == '\n') {
      if (lineIndex == bestLine) break;
      lineIndex++;
      continue;
    }
    if (lineIndex == bestLine) {
      line.push_back(cp);
    } else {
      base++;
    }
  }
  if (line.empty()) return false;

  // The cell under the point: the line's length split evenly into its cells. Clamped, so a hold
  // just past either end lands on the first or last character.
  const auto cells = lineCells(line);
  const int along = block.vertical ? py - lb.y : px - lb.x;
  const int length = std::max(1, block.vertical ? static_cast<int>(lb.h) : static_cast<int>(lb.w));
  const int cell =
      std::clamp(static_cast<int>(static_cast<int64_t>(along) * static_cast<int64_t>(cells.size()) / length), 0,
                 static_cast<int>(cells.size()) - 1);

  text.clear();
  for (const auto* tb : blocks) {
    if (!text.empty()) text += '\n';
    text += tb->text;
  }
  glyph = base + static_cast<int>(cells[cell]);
  return true;
}

void MangaReaderActivity::launchWordLookupAt(std::string combined, const int glyph) {
  if (!book || !DictIndex::isAvailable()) return;
  const ViewMode returnMode = viewMode;
  if (combined.empty()) return;
  // Dictionary text needs the Japanese SD fallback. Restore it only for the child activity, then
  // return its memory to the page decoder before the manga redraws.
  {
    RenderLock lock;
    sdFontSystem.ensureLoaded(renderer);
    sdFontSystem.setJpFallbackNeeded(renderer, true);
  }
  startActivityForResult(std::make_unique<MangaWordLookupActivity>(
                             renderer, mappedInput, std::move(combined), book->getCachePath() + "/wlscan.bin",
                             static_cast<uint16_t>(currentPage), static_cast<uint16_t>(currentPanel + 1), glyph),
                         [this, returnMode](const ActivityResult&) {
                           {
                             RenderLock lock;
                             sdFontSystem.releaseAllResidentFonts(renderer);
                             sdFontSystem.setJpFallbackNeeded(renderer, false);
                             viewMode = returnMode;
                           }
                           requestUpdate();
                         });
}

void MangaReaderActivity::launchWordLookup() {
  if (currentPanel < 0 || currentPanel >= static_cast<int>(panels.size())) return;
  if (!DictIndex::isAvailable()) return;

  const auto& panel = panels[currentPanel];
  if (panel.textBlocks.empty()) return;

  // Build a combined text string from all text blocks in this panel
  std::string combined;
  for (const auto& tb : panel.textBlocks) {
    if (!combined.empty()) combined += '\n';
    combined += tb.text;
  }

  if (combined.empty()) return;

  {
    RenderLock lock;
    sdFontSystem.ensureLoaded(renderer);
    sdFontSystem.setJpFallbackNeeded(renderer, true);
  }

  // Use the MangaWordLookup sub-activity with raw text. The scan cache makes a re-open of the
  // same panel/page text instant (validated by content hash, so the key is just a hint).
  startActivityForResult(std::make_unique<MangaWordLookupActivity>(
                             renderer, mappedInput, std::move(combined), book->getCachePath() + "/wlscan.bin",
                             static_cast<uint16_t>(currentPage), static_cast<uint16_t>(currentPanel + 1)),
                         [this](const ActivityResult&) {
                           {
                             RenderLock lock;
                             sdFontSystem.releaseAllResidentFonts(renderer);
                             sdFontSystem.setJpFallbackNeeded(renderer, false);
                             viewMode = ViewMode::PanelZoom;
                           }
                           requestUpdate();
                         });
}

void MangaReaderActivity::saveProgress() const {
  if (!book) return;
  std::string cachePath = book->getCachePath();

  if (!Storage.exists(cachePath.c_str())) {
    Storage.mkdir(cachePath.c_str());
  }

  const uint32_t savedPage = book->getPageCount() > 0 ? std::min(currentPage, book->getPageCount() - 1) : 0;
  uint8_t data[7];
  data[0] = savedPage & 0xFF;
  data[1] = (savedPage >> 8) & 0xFF;
  data[2] = (savedPage >> 16) & 0xFF;
  data[3] = (savedPage >> 24) & 0xFF;
  int16_t panelVal = static_cast<int16_t>(currentPanel);
  memcpy(data + 4, &panelVal, 2);
  data[6] = panelsOnlyMode ? 1 : 0;

  ProgressFile::writeAtomic(cachePath, data, sizeof(data));
}

void MangaReaderActivity::loadProgress() {
  if (!book) return;
  std::string cachePath = book->getCachePath();

  HalFile f;
  if (Storage.openFileForRead("MNG", cachePath + "/progress.bin", f)) {
    uint8_t data[7] = {};
    const int bytesRead = f.read(data, sizeof(data));
    if (bytesRead >= 6) {
      currentPage = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
      int16_t panelVal;
      memcpy(&panelVal, data + 4, 2);
      currentPanel = panelVal;
      // Six-byte progress files predate this setting and default to normal full-page mode.
      panelsOnlyMode = bytesRead >= 7 && data[6] != 0;

      if (currentPage >= book->getPageCount()) {
        currentPage = 0;
        currentPanel = -1;
      }

      viewMode = (currentPanel >= 0) ? ViewMode::PanelZoom : ViewMode::FullPage;
    }
  }
}

void MangaReaderActivity::loadCachedBookmarks() {
  cachedBookmarks.clear();
  if (!book) {
    currentPageBookmarked = false;
    return;
  }

  BookmarkFile::load(book->getFolder(), cachedBookmarks);
}

// Manga has a flat page index (no chapters/xpath like Epub), so bookmarks
// reuse BookmarkEntry with computedSpineIndex fixed at 0 and
// computedChapterPageCount/computedChapterProgress holding the book's total
// page count / bookmarked page directly.
void MangaReaderActivity::updateBookmarkFlag() {
  if (!book || cachedBookmarks.empty()) {
    currentPageBookmarked = false;
    return;
  }
  const uint32_t pageCount = book->getPageCount();
  currentPageBookmarked = std::any_of(cachedBookmarks.begin(), cachedBookmarks.end(), [&](const BookmarkEntry& b) {
    return b.computedSpineIndex == 0 && b.computedChapterPageCount == pageCount &&
           b.computedChapterProgress == currentPage;
  });
}

void MangaReaderActivity::addBookmark() {
  if (!book) return;
  const uint32_t pageCount = book->getPageCount();
  if (pageCount == 0) return;

  const size_t countBefore = cachedBookmarks.size();
  cachedBookmarks.erase(std::remove_if(cachedBookmarks.begin(), cachedBookmarks.end(),
                                       [&](const BookmarkEntry& b) {
                                         return b.computedSpineIndex == 0 && b.computedChapterPageCount == pageCount &&
                                                b.computedChapterProgress == currentPage;
                                       }),
                        cachedBookmarks.end());
  if (cachedBookmarks.size() != countBefore) {
    bookmarkRemoved = true;
    currentPageBookmarked = false;
  } else {
    std::string pageText;
    for (const auto& panel : panels) {
      for (const auto& tb : panel.textBlocks) {
        if (!pageText.empty()) pageText += '\n';
        pageText += tb.text;
      }
    }
    BookmarkEntry entry;
    entry.percentage = static_cast<float>(currentPage) / static_cast<float>(pageCount);
    if (pageText.empty()) {
      char buf[32];
      snprintf(buf, sizeof(buf), tr(STR_PAGE_NUMBER_FORMAT), currentPage + 1);
      entry.summary = buf;
    } else {
      entry.summary = BookmarkUtil::sanitizeBookmarkSummary(pageText);
    }
    entry.computedSpineIndex = 0;
    entry.computedChapterPageCount = static_cast<uint16_t>(std::min<uint32_t>(pageCount, 0xFFFF));
    entry.computedChapterProgress = static_cast<uint16_t>(std::min<uint32_t>(currentPage, 0xFFFF));
    cachedBookmarks.insert(cachedBookmarks.begin(), entry);
    bookmarkRemoved = false;
    currentPageBookmarked = true;
  }

  if (!BookmarkFile::save(book->getFolder(), cachedBookmarks)) {
    LOG_ERR("MNG", "Failed to save bookmarks for: %s", book->getFolder().c_str());
  }
}

void MangaReaderActivity::launchMenu() {
  if (!book) return;

  const int totalPages = static_cast<int>(book->getPageCount());
  const int curPage = static_cast<int>(currentPage) + 1;
  const int bookProgressPercent = totalPages > 0 ? static_cast<int>((currentPage + 1) * 100 / totalPages) : 0;

  // hasWordLookup gates whether the item appears at all -- stable for the
  // whole book (dictionary installed), so it never shifts other items.
  // hasPageText reflects THIS page/panel specifically and only dims
  // Word Lookup/Translate/QR rather than hiding them, since OCR'd text
  // availability varies panel-to-panel.
  const bool hasWordLookup = DictIndex::isAvailable();
  bool hasPageText = false;
  if (panelsLoaded) {
    if (currentPanel >= 0 && currentPanel < static_cast<int>(panels.size())) {
      hasPageText = !panels[currentPanel].textBlocks.empty();
    } else {
      hasPageText = std::any_of(panels.begin(), panels.end(), [](const auto& p) { return !p.textBlocks.empty(); });
    }
  }

  // hasFootnotes=false (no footnotes in manga); mangaMode=true hides Look Up (Word Lookup
  // covers OCR'd text) and routes READER_SETTINGS below to a filtered Settings screen -- see
  // its case and SettingsActivity's mangaMode.
  startActivityForResult(
      std::make_unique<EpubReaderMenuActivity>(renderer, mappedInput, book->getTitle(), curPage, totalPages,
                                               bookProgressPercent, SETTINGS.orientation,
                                               /*hasFootnotes=*/false, /*hasBookmarks=*/!cachedBookmarks.empty(),
                                               /*hasWordLookup=*/hasWordLookup, /*verticalEnabled=*/false,
                                               /*furiganaEnabled=*/true, /*hasPageText=*/hasPageText,
                                               /*imageReaderMinimal=*/false, /*mangaMode=*/true,
                                               /*hideGenericLookup=*/false,
                                               /*showPanelsOnlyToggle=*/bookHasPanelCropCapability,
                                               /*panelsOnlyEnabled=*/panelsOnlyMode,
                                               /*scrubOnEnter=*/true),
      [this](const ActivityResult& result) {
        const auto& menu = std::get<MenuResult>(result.data);
        // Apply orientation change
        if (SETTINGS.orientation != menu.orientation) {
          SETTINGS.orientation = menu.orientation;
          SETTINGS.saveToFile();
          ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
        }
        if (!result.isCancelled) {
          const auto action = static_cast<EpubReaderMenuActivity::MenuAction>(menu.action);
          if (action == EpubReaderMenuActivity::MenuAction::AUTO_PAGE_TURN) {
            toggleAutoPageTurn(menu.pageTurnOption);
          } else {
            onReaderMenuConfirm(action);
          }
        }
        requestUpdate();
      });
}

void MangaReaderActivity::onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action) {
  // Reusable lambda for jumping to a page by percent (shared by SELECT_CHAPTER and GO_TO_PERCENT)
  auto launchPercentJump = [this]() {
    if (!book || book->getPageCount() == 0) return;
    const int totalPages = static_cast<int>(book->getPageCount());
    const int initialPercent = totalPages > 0 ? static_cast<int>((currentPage + 1) * 100 / totalPages) : 0;
    startActivityForResult(std::make_unique<EpubReaderPercentSelectionActivity>(renderer, mappedInput, initialPercent,
                                                                                /*scrubOnEnter=*/true),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled && book) {
                               const int percent = std::get<PercentResult>(result.data).percent;
                               const uint32_t totalPages = book->getPageCount();
                               uint32_t targetPage = static_cast<uint32_t>(static_cast<float>(percent) / 100.0f *
                                                                           static_cast<float>(totalPages));
                               if (targetPage >= totalPages && totalPages > 0) targetPage = totalPages - 1;
                               currentPage = targetPage;
                               currentPanel = -1;
                               viewMode = ViewMode::FullPage;
                               loadCurrentPagePanels();
                               requestUpdate();
                             }
                           });
  };

  switch (action) {
    case EpubReaderMenuActivity::MenuAction::READER_SETTINGS: {
      // Manga keeps Rotate Panels and the other applicable Reader settings, while hiding
      // EPUB-only text and image-rendering settings through SettingsActivity's mangaMode.
      // Restore SD fonts while Settings is open, then release that memory before manga redraws.
      {
        RenderLock lock;
        sdFontSystem.ensureLoaded(renderer);
      }
      startActivityForResult(std::make_unique<SettingsActivity>(renderer, mappedInput, /*initialCategory=*/1,
                                                                /*finishOnBack=*/true, /*japaneseBook=*/true,
                                                                /*dictionaryLanguage=*/std::string{},
                                                                /*showReaderToggles=*/false,
                                                                /*verticalTextEnabled=*/false,
                                                                /*furiganaEnabled=*/false,
                                                                /*mangaMode=*/true,
                                                                /*hideMangaOnlySettings=*/false),
                             [this](const ActivityResult&) {
                               ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
                               baseScreenW = renderer.getScreenWidth();
                               baseScreenH = renderer.getScreenHeight();
                               {
                                 RenderLock lock;
                                 sdFontSystem.releaseAllResidentFonts(renderer);
                               }
                               launchMenu();
                             });
      return;
    }
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER:
      if (book && book->hasToc()) {
        startActivityForResult(
            std::make_unique<MangaChapterSelectionActivity>(renderer, mappedInput, book->getToc(), currentPage),
            [this](const ActivityResult& result) {
              if (!result.isCancelled && book) {
                const uint32_t targetPage = std::get<PageResult>(result.data).page;
                if (targetPage < book->getPageCount()) {
                  currentPage = targetPage;
                  currentPanel = -1;
                  viewMode = ViewMode::FullPage;
                  loadCurrentPagePanels();
                }
              }
              requestUpdate();
            });
        return;
      }
      // No TOC available — fall back to percent-based page jump
      launchPercentJump();
      break;
    case EpubReaderMenuActivity::MenuAction::WORD_LOOKUP: {
      launchWordLookupCurrentView();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TRANSLATE_PAGE: {
      // Same full-page fallback as WORD_LOOKUP above. Prefer a translation
      // already extracted offline during manga conversion (instant, no
      // network) over a live Gemini call.
      std::string combined;
      std::string preTranslated;
      if (currentPanel >= 0 && currentPanel < static_cast<int>(panels.size())) {
        const auto& panel = panels[currentPanel];
        for (const auto& tb : panel.textBlocks) {
          if (!combined.empty()) combined += '\n';
          combined += tb.text;
        }
        preTranslated = panel.translation;
      } else {
        for (const auto& panel : panels) {
          for (const auto& tb : panel.textBlocks) {
            if (!combined.empty()) combined += '\n';
            combined += tb.text;
          }
          if (!panel.translation.empty()) {
            if (!preTranslated.empty()) preTranslated += '\n';
            preTranslated += panel.translation;
          }
        }
      }
      if (!combined.empty()) {
        startActivityForResult(std::make_unique<EpubReaderTranslationActivity>(
                                   renderer, mappedInput, std::move(combined), std::move(preTranslated)),
                               [this](const ActivityResult&) { requestUpdate(); });
        return;
      }
      break;
    }
    case EpubReaderMenuActivity::MenuAction::BOOKMARKS: {
      if (!book) break;
      startActivityForResult(
          std::make_unique<MangaBookmarksActivity>(renderer, mappedInput, book->getFolder(), book->getToc()),
          [this](const ActivityResult& result) {
            if (!result.isCancelled && book) {
              const uint32_t targetPage = std::get<PageResult>(result.data).page;
              if (targetPage < book->getPageCount()) {
                currentPage = targetPage;
                currentPanel = -1;
                viewMode = ViewMode::FullPage;
                loadCurrentPagePanels();
              }
            }
            requestUpdate();
          });
      return;
    }
    case EpubReaderMenuActivity::MenuAction::TOGGLE_BOOKMARK: {
      addBookmark();
      showBookmarkMessage = true;
      bookmarkMessageTime = millis();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::ROTATE_SCREEN:
      // Orientation already applied in the callback above
      break;
    case EpubReaderMenuActivity::MenuAction::TOGGLE_PANELS_ONLY:
      panelsOnlyMode = !panelsOnlyMode;
      if (panelsOnlyMode && pageHasPanelCrops) {
        currentPanel = firstPanelWithCrop;
        viewMode = ViewMode::PanelZoom;
      } else if (!panelsOnlyMode && currentPageHasImage) {
        currentPanel = -1;
        viewMode = ViewMode::FullPage;
      }
      saveProgress();
      break;
    case EpubReaderMenuActivity::MenuAction::GO_TO_PERCENT:
      launchPercentJump();
      break;
    case EpubReaderMenuActivity::MenuAction::SCREENSHOT: {
      pendingScreenshot = true;
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_HOME: {
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_CACHE: {
      if (book) {
        std::string cachePath = book->getCachePath();
        if (Storage.exists(cachePath.c_str())) {
          Storage.removeDir(cachePath.c_str());
        }
      }
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::DISPLAY_QR: {
      // Show the panel's (or, in full-page view, the whole page's) text as
      // a QR code -- same full-page fallback as WORD_LOOKUP/TRANSLATE_PAGE.
      std::string combined;
      if (currentPanel >= 0 && currentPanel < static_cast<int>(panels.size())) {
        for (const auto& tb : panels[currentPanel].textBlocks) {
          if (!combined.empty()) combined += '\n';
          combined += tb.text;
        }
      } else {
        for (const auto& panel : panels) {
          for (const auto& tb : panel.textBlocks) {
            if (!combined.empty()) combined += '\n';
            combined += tb.text;
          }
        }
      }
      if (!combined.empty()) {
        startActivityForResult(std::make_unique<QrDisplayActivity>(renderer, mappedInput, std::move(combined)),
                               [this](const ActivityResult&) { requestUpdate(); });
        return;
      }
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SYNC:
      // KOReader sync matches progress against a document hash computed from
      // an actual ebook file. A manga folder of images has no equivalent
      // document on the KOReader server side, so sync is not applicable.
      break;
    default:
      // AUTO_PAGE_TURN handled above in launchMenu(); FOOTNOTES,
      // TOGGLE_VERTICAL, TOGGLE_FURIGANA are not applicable for manga.
      break;
  }
}

ScreenshotInfo MangaReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Xtc;  // reuse XTC type for now
  if (book) {
    snprintf(info.title, sizeof(info.title), "%s", book->getTitle().c_str());
    info.totalPages = static_cast<int>(book->getPageCount());
    info.currentPage = static_cast<int>(currentPage) + 1;
    info.progressPercent =
        book->getPageCount() > 0 ? static_cast<int>((currentPage + 1) * 100 / book->getPageCount()) : 0;
  }
  return info;
}

bool MangaReaderActivity::enterWordSelect() {
  if (!displayedMap_.valid) return false;
  const auto blocks = viewTextBlocks();

  // Every character of the view text gets the cell it is set in: a line's length split evenly into
  // its cells (see lineCells()), each cell covering one character or one upright run.
  std::string text;
  std::vector<GlyphCell> cells;
  bool anyGeometry = false;
  for (size_t b = 0; b < blocks.size(); b++) {
    const auto& block = *blocks[b];
    if (!text.empty()) text += '\n';
    text += block.text;
    std::vector<uint32_t> line;
    int lineIndex = 0;
    auto flushLine = [&] {
      if (line.empty()) return;
      const bool hasBox = lineIndex < static_cast<int>(block.lines.size());
      const auto lineCellStarts = lineCells(line);
      const int n = static_cast<int>(lineCellStarts.size());
      for (int k = 0; k < n; k++) {
        const size_t end = k + 1 < n ? lineCellStarts[k + 1] : line.size();
        GlyphCell cell;
        if (hasBox) {
          const auto& lb = block.lines[lineIndex];
          cell.block = static_cast<int16_t>(b);
          cell.line = static_cast<int16_t>(lineIndex);
          if (block.vertical) {
            cell.x = lb.x;
            cell.w = lb.w;
            cell.y = static_cast<uint16_t>(lb.y + k * lb.h / n);
            cell.h = static_cast<uint16_t>(std::max(1, (k + 1) * lb.h / n - k * lb.h / n));
          } else {
            cell.y = lb.y;
            cell.h = lb.h;
            cell.x = static_cast<uint16_t>(lb.x + k * lb.w / n);
            cell.w = static_cast<uint16_t>(std::max(1, (k + 1) * lb.w / n - k * lb.w / n));
          }
          anyGeometry = true;
        }
        for (size_t g = lineCellStarts[k]; g < end; g++) cells.push_back(cell);
      }
      line.clear();
    };
    for (const uint32_t cp : decodeUtf8(block.text)) {
      if (cp == '\n') {
        flushLine();
        lineIndex++;
      } else {
        line.push_back(cp);
      }
    }
    flushLine();
  }
  if (!anyGeometry) return false;

  // The words themselves come from the same segmentation the lookup uses, so what is outlined is
  // exactly what Confirm will look up. A view's text is a few bubbles: scanning it all is quick.
  WordSelectionScan scan;
  scan.initFromUtf8Text(text);
  while (!scan.isDone()) scan.step(50);
  std::vector<SelectWord> words;
  for (size_t i = 0; i < scan.selectToAllIdx.size(); i++) {
    const size_t start = scan.selectToAllIdx[i];
    const size_t len = std::max<size_t>(scan.selectableGlyphs[i].matchLen, 1);
    if (start >= cells.size() || cells[start].block < 0) continue;  // nothing to outline
    words.push_back(SelectWord{static_cast<uint16_t>(start), static_cast<uint8_t>(std::min<size_t>(len, 255))});
  }
  if (words.empty()) return false;

  {
    RenderLock lock(*this);
    selectText_ = std::move(text);
    selectCells_ = std::move(cells);
    selectWords_ = std::move(words);
    selectCursor_ = 0;
    selectPage_ = currentPage;
    selectPanel_ = currentPanel;
    wordSelect_ = true;
    outlineOnlyUpdate_ = true;
  }
  requestUpdate();
  return true;
}

void MangaReaderActivity::exitWordSelect(const bool inPlace) {
  {
    RenderLock lock(*this);
    wordSelect_ = false;
    outlineOnlyUpdate_ = inPlace;
    selectWords_.clear();
    selectCells_.clear();
    selectText_.clear();
  }
  requestUpdate();
}

bool MangaReaderActivity::handleWordSelectInput() {
  if (!selectionIsCurrent()) {
    // The page or panel changed under it (a gesture page turn, the menu): drop it quietly. That
    // change already asked for a full render, which an outline-only update must not replace.
    exitWordSelect(/*inPlace=*/false);
    return false;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    exitWordSelect(/*inPlace=*/true);
    return true;
  }
  // Confirm, or the same shortcut that opened the selection (power click, a Word Lookup side
  // button): look the outlined word up. Two presses in, as the book panel works.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || ReaderUtils::wordLookupPowerClick(mappedInput) ||
      ReaderUtils::wordLookupSideToggle(mappedInput)) {
    lookUpSelectedWord();
    return true;
  }
  // The keys that would turn the page move the outline instead -- exactly those keys, on exactly
  // the same edge (press or release, per the long-press setting), and in the same direction the
  // reader gives them, Reversed Page Turn included. Asking the reader's own detector is what makes
  // that hold: a hand-picked button list reacted on release, while the page turn had already
  // fired on the press and moved on to the next panel.
  const auto turn =
      ReaderUtils::detectPageTurnForOrientation(mappedInput, SETTINGS.reversePageTurn != 0, SETTINGS.orientation);
  int step = turn.next ? 1 : (turn.prev ? -1 : 0);
  if (step == 0) {
    // The other axis steps too, so the pair that is not the page-turn pair is not a dead end.
    using B = MappedInputManager::Button;
    if (mappedInput.wasPressed(B::ScreenDown) || mappedInput.wasPressed(B::NavNext)) {
      step = 1;
    } else if (mappedInput.wasPressed(B::ScreenUp) || mappedInput.wasPressed(B::NavPrevious)) {
      step = -1;
    }
  }
  if (step != 0) {
    const int count = static_cast<int>(selectWords_.size());
    const int next = std::clamp(selectCursor_ + step, 0, count - 1);
    if (next != selectCursor_) {
      {
        RenderLock lock(*this);
        selectCursor_ = next;
        outlineOnlyUpdate_ = true;
      }
      requestUpdate();
    }
    return true;
  }
  // Every other input -- touch, the menu, page turns by gesture -- belongs to the reader as usual,
  // except that turning the page or panel ends the selection (its words are no longer on screen).
  return false;
}

void MangaReaderActivity::lookUpSelectedWord() {
  // The lookup opens on this word; closing it returns here, still selecting, so the next word is
  // one press away. launchWordLookupAt takes the text by value -- keep ours for the next pick.
  const auto& word = selectWords_[static_cast<size_t>(selectCursor_)];
  launchWordLookupAt(selectText_, word.glyph);
}

bool MangaReaderActivity::handleHomeGesture() {
  if (!selectionIsCurrent()) return false;
  lookUpSelectedWord();
  return true;
}

void MangaReaderActivity::drawWordOutline() {
  pageInFramebuffer_ = true;
  drawnOutline_.clear();
  if (!selectionIsCurrent() || selectWords_.empty()) return;
  outlineBoxes(std::clamp(selectCursor_, 0, static_cast<int>(selectWords_.size()) - 1), drawnOutline_);
  invertBoxes(drawnOutline_);
}

void MangaReaderActivity::outlineBoxes(const int cursor, std::vector<OutlineBox>& out) const {
  const HoldMap& m = displayedMap_;
  if (!m.valid || m.sw <= 0 || m.sh <= 0 || cursor < 0 || cursor >= static_cast<int>(selectWords_.size())) return;
  const auto& word = selectWords_[static_cast<size_t>(cursor)];

  // One box per line the word is set on (a word can wrap to the next column), each the union of
  // its characters' cells, mapped from the page image into the frame being drawn.
  auto toX = [&](int px) { return m.dx + static_cast<int>(static_cast<int64_t>(px - m.sx) * m.dw / m.sw); };
  auto toY = [&](int py) { return m.dy + static_cast<int>(static_cast<int64_t>(py - m.sy) * m.dh / m.sh); };
  constexpr int kPad = 3;
  size_t g = word.glyph;
  const size_t end = std::min(selectCells_.size(), static_cast<size_t>(word.glyph) + word.len);
  while (g < end) {
    const GlyphCell& first = selectCells_[g];
    if (first.block < 0) {
      g++;
      continue;
    }
    int x1 = first.x, y1 = first.y, x2 = first.x + first.w, y2 = first.y + first.h;
    size_t h = g + 1;
    while (h < end && selectCells_[h].block == first.block && selectCells_[h].line == first.line) {
      const GlyphCell& c = selectCells_[h];
      x1 = std::min<int>(x1, c.x);
      y1 = std::min<int>(y1, c.y);
      x2 = std::max<int>(x2, c.x + c.w);
      y2 = std::max<int>(y2, c.y + c.h);
      h++;
    }
    const int sx1 = toX(x1) - kPad, sy1 = toY(y1) - kPad;
    out.push_back({static_cast<int16_t>(sx1), static_cast<int16_t>(sy1), static_cast<int16_t>(toX(x2) + kPad - sx1),
                   static_cast<int16_t>(toY(y2) + kPad - sy1)});
    g = h;
  }
}

void MangaReaderActivity::invertBoxes(const std::vector<OutlineBox>& boxes) const {
  constexpr int kLine = 2;
  for (const auto& b : boxes) {
    // Four strips that never overlap: a corner inverted twice would cancel out.
    renderer.invertRect(b.x, b.y, b.w, kLine);
    renderer.invertRect(b.x, b.y + b.h - kLine, b.w, kLine);
    renderer.invertRect(b.x, b.y + kLine, kLine, b.h - 2 * kLine);
    renderer.invertRect(b.x + b.w - kLine, b.y + kLine, kLine, b.h - 2 * kLine);
  }
}

bool MangaReaderActivity::updateOutlineInPlace() {
  if (!pageInFramebuffer_) return false;
  std::vector<OutlineBox> next;
  if (selectionIsCurrent() && !selectWords_.empty()) {
    outlineBoxes(std::clamp(selectCursor_, 0, static_cast<int>(selectWords_.size()) - 1), next);
  }
  if (drawnOutline_.empty() && next.empty()) return true;
  // The boxes are in the page's frame, which a rotated page sets up for its whole render.
  const auto saved = renderer.getOrientation();
  if (displayedRotated_) renderer.setOrientation(static_cast<GfxRenderer::Orientation>((saved + 3) % 4));
  invertBoxes(drawnOutline_);  // XOR: restores the page under the old outline exactly
  invertBoxes(next);
  drawnOutline_ = std::move(next);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  if (displayedRotated_) renderer.setOrientation(saved);
  return true;
}
