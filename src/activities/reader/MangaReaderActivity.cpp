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
#include <JsonSettingsIO.h>
#include <Memory.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
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
#include "components/UITheme.h"
#include "fontIds.h"
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

  // Swallow the Confirm release that opened this book from the library,
  // so it doesn't immediately trigger the reader menu.
  ignoreNextConfirmRelease = true;

  if (!book) return;

  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
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
  RECENT_BOOKS.addBook(book->getFolder(), book->getTitle(), bookAuthor, book->getPageImagePath(0));

  readingSessionStartMs = millis();

  // Background prefetch worker (see the header doc block). Same priority as the loop and render
  // tasks so it timeslices instead of starving either; stack mirrors the render task, which runs
  // these same decode paths. Creation failure (OOM) just disables prefetching -- postPrefetchJob
  // no-ops on a null handle and every render path decodes on demand exactly as before.
  if (xTaskCreate(&prefetchTaskTrampoline, "MangaPrefetch", PREFETCH_TASK_STACK, this, 1, &prefetchTaskHandle) !=
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
  ReaderUtils::flushReadingStats(readingSessionStartMs, /*force=*/true);

  // On the last page (currentPage never advances past pageCount-1 -- see
  // nextPage()) regardless of how long this particular session lasted.
  // Previously this was nested inside `minutes > 0` AND compared against
  // pageCount instead of pageCount-1, so it could never fire -- manga never
  // got marked finished even at 100% progress, unlike Epub/Txt readers.
  const bool atLastPage = book && book->getPageCount() > 0 && currentPage >= book->getPageCount() - 1;
  if (atLastPage) {
    READING_STATS.loadFromFile();
    READING_STATS.markBookFinished(book->getFolder());
    READING_STATS.saveToFile();
  }

  saveProgress();
  panels.clear();
  book.reset();

  // Reset orientation back to portrait for the rest of the UI (matches
  // EpubReaderActivity/TxtReaderActivity) -- Home/Library never call
  // applyOrientation themselves, so any rotation left active here (from the
  // reading orientation setting, or a page/panel that triggered the
  // fill-the-screen rotate) would otherwise leak into their layout.
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

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
  if (!loadedPanels.empty() && book) {
    const std::string bmp0 = panelCropPathExt(0, ".bmp");
    if (Storage.exists(bmp0.c_str())) {
      hasCrops = true;
      cropsBmp = true;
      panelsBw = BmpToFramebufferConverter::isMonochromeStatic(bmp0);
    } else if (Storage.exists(panelCropPathExt(0, ".jpg").c_str())) {
      hasCrops = true;
    }
  }
  // A 1-bit BMP full page renders BW-only (single wave, no gray planes); probe it once here
  // (cheap header read) so renderFullPage can pick the fast path. Only .bmp pages can be
  // monochrome -- jpg/png always go the grayscale route.
  bool bwOnly = false;
  if (book) {
    const std::string pageImg = book->getPageImagePath(currentPage);
    if (FsHelpers::hasBmpExtension(pageImg)) {
      bwOnly = BmpToFramebufferConverter::isMonochromeStatic(pageImg);
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
    currentPageBwOnly = bwOnly;
    panelCropIsBmp = cropsBmp;
    panelsBwOnly = panelsBw;
    panelDims.assign(panels.size(), {});
    firstPanelPrefetched = !armFirst;
    nextPanelPrefetched = true;
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
    currentPanel = 0;
    viewMode = ViewMode::PanelZoom;
  } else if (currentPanel < static_cast<int>(panels.size()) - 1) {
    currentPanel++;
  } else {
    nextPage();
    return;
  }
  requestUpdate();
}

void MangaReaderActivity::prevPanel() {
  // See nextPanel(): cancel a deferred gray upgrade queued for the panel we're leaving.
  panelGrayPending = false;
  panelGrayUpgrade = false;

  if (currentPanel > 0) {
    currentPanel--;
    requestUpdate();
  } else if (currentPanel == 0) {
    currentPanel = -1;
    viewMode = ViewMode::FullPage;
    requestUpdate();
  } else {
    prevPage();
  }
}

void MangaReaderActivity::nextPage() {
  if (!book) return;
  if (currentPage + 1 < book->getPageCount()) {
    currentPage++;
    currentPanel = -1;
    viewMode = ViewMode::FullPage;
    loadCurrentPagePanels();
    requestUpdate();
  }
}

void MangaReaderActivity::prevPage() {
  if (currentPage > 0) {
    currentPage--;
    currentPanel = -1;
    viewMode = ViewMode::FullPage;
    loadCurrentPagePanels();
    requestUpdate();
  }
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
  ReaderUtils::flushReadingStats(readingSessionStartMs);

  // Publish any finished prefetch-worker result (panelDims slot + done flags) under the loop
  // task's normal locking rules. Runs before everything else so a completed warm is visible to
  // this very tick's input handling, and so the single job slot frees up for the next post.
  applyPrefetchResult();

  if (showBookmarkMessage && (millis() - bookmarkMessageTime) >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showBookmarkMessage = false;
    requestUpdate();
  }

  if (automaticPageTurnActive) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
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
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::WORD_LOOKUP &&
      mappedInput.wasReleased(MappedInputManager::Button::Power) &&
      !mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    launchWordLookupCurrentView();
    return;
  }

  if (viewMode == ViewMode::TextOverlay) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      viewMode = ViewMode::PanelZoom;
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
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
      currentPanel = -1;
      viewMode = ViewMode::FullPage;
      requestUpdate();
      return;
    }
    onGoHome();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (ignoreNextConfirmRelease) {
      ignoreNextConfirmRelease = false;
    } else if (viewMode == ViewMode::PanelZoom || viewMode == ViewMode::FullPage) {
      launchMenu();
      return;
    }
  }

  const auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  if (!prevTriggered && !nextTriggered) {
    // Idle tick: after a dwell, warm the pixel cache the user is most likely to need next so the
    // render hits it instead of a fresh JPEG decode. Dwell gates and ordering rationale live with
    // the PREFETCH_DWELL_MS / FIRST_PANEL_PREFETCH_DWELL_MS constants above. In short: on a paneled
    // full page warm the first panel first (shorter dwell), then the next page; pages without crops
    // go straight to the next page; inside panel-zoom warm the next panel.
    if (viewMode == ViewMode::FullPage) {
      const unsigned long dwell = millis() - fullPageRenderedMs;
      if (pageHasPanelCrops && !firstPanelPrefetched && dwell > FIRST_PANEL_PREFETCH_DWELL_MS) {
        prefetchPanelCache(0);
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
        currentPanel = 0;
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

  if (currentPage >= book->getPageCount()) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() / 2, tr(STR_END_OF_BOOK), true,
                              EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

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
    if (ratio > screenRatio) {
      g.y = static_cast<int>((g.screenH - g.screenW / ratio) / 2.0f);
      g.destWidth = g.screenW;
      g.destHeight = g.screenH - 2 * g.y;
    } else {
      g.x = static_cast<int>((g.screenW - g.screenH * ratio) / 2.0f);
      g.destWidth = g.screenW - 2 * g.x;
      g.destHeight = g.screenH;
    }
  } else {
    g.x = (g.screenW - imgWidth) / 2;
    g.y = (g.screenH - imgHeight) / 2;
  }
  return g;
}

MangaReaderActivity::FullPageGeom MangaReaderActivity::applyFullPageGeometry(const int imgWidth, const int imgHeight) {
  const int savedOrientation = renderer.getOrientation();
  FullPageGeom g = computeFullPageGeom(imgWidth, imgHeight, renderer.getScreenWidth(), renderer.getScreenHeight());
  g.savedOrientation = savedOrientation;
  if (g.rotated) {
    renderer.setOrientation(static_cast<GfxRenderer::Orientation>((savedOrientation + 3) % 4));
  }
  return g;
}

void MangaReaderActivity::renderFullPage() {
  renderer.clearScreen();

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

  ImageDimensions dims = {0, 0};
  if (!decoder->getDimensions(imgPath, dims) || dims.width <= 0 || dims.height <= 0) {
    renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() / 2, tr(STR_PAGE_LOAD_ERROR), true);
    renderer.displayBuffer();
    return;
  }

  const FullPageGeom g = applyFullPageGeometry(dims.width, dims.height);
  const auto savedOrientation = static_cast<GfxRenderer::Orientation>(g.savedOrientation);
  const bool rotatePage = g.rotated;
  const int x = g.x, y = g.y;
  const int destWidth = g.destWidth, destHeight = g.destHeight;
  const int screenW = g.screenW, screenH = g.screenH;

  // Only JPEG/PNG pages stream a .2bp pixel cache; the BMP converter renders straight to the
  // framebuffer and never writes one, so for a BMP page the cache would be a guaranteed miss on
  // every plane -- an SD open attempt for nothing. Skip it entirely for BMP (mirrors the
  // panelCropIsBmp guard in renderPanelZoom). Note the 1-bit BMP fast path below returns before
  // any cache use anyway; this covers the >=8-bit BMP page case.
  const bool useCache = !FsHelpers::hasBmpExtension(imgPath);
  const std::string cachePath =
      useCache ? book->getCachePath() + "/page_" + std::to_string(currentPage) + ".2bp" : std::string();

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
  if (!useCache || !PixelCacheIO::renderFromCache(renderer, cachePath, x, y, destWidth, destHeight)) {
    decoder->decodeToFramebuffer(imgPath, renderer, config);
  }

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
  if (!useCache || !PixelCacheIO::renderFromCache(renderer, cachePath, x, y, destWidth, destHeight)) {
    decoder->decodeToFramebuffer(imgPath, renderer, config);
  }
  renderer.copyGrayscaleLsbBuffers();

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  if (!useCache || !PixelCacheIO::renderFromCache(renderer, cachePath, x, y, destWidth, destHeight)) {
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

  const auto& panel = panels[currentPanel];

  std::string imgPath = book->getPageImagePath(currentPage);
  if (imgPath.empty()) {
    renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() / 2, tr(STR_PAGE_LOAD_ERROR), true);
    renderer.displayBuffer();
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
    // A null decoder is a transient condition (getDecoder allocates its decoder with nothrow and
    // returns null on OOM), NOT a permanently missing/unsupported crop -- so DON'T poison the
    // dims slot with -1 here. Fall back this frame; a later entry retries once memory recovers.
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
  config.useGrayscale = !bwOnly;
  config.useDithering = !bwOnly;
  config.cachePath = cachePath;

  // Populate the BW framebuffer. BW-only decodes the 1-bit BMP straight to BW; the grayscale path
  // renders from the .2bp pixel cache when warm (revisited/prefetched JPEG panel) and only decodes
  // on a cold pass (or always, for a cacheless BMP crop), same as renderFullPage().
  if (bwOnly) {
    renderer.setRenderMode(GfxRenderer::BW);
    decoder->decodeToFramebuffer(panelImgPath, renderer, config);
  } else if (!useCache || !PixelCacheIO::renderFromCache(renderer, cachePath, x, y, fitW, fitH)) {
    decoder->decodeToFramebuffer(panelImgPath, renderer, config);
  }

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
      if (!useCache || !PixelCacheIO::renderFromCache(renderer, cachePath, x, y, fitW, fitH)) {
        decoder->decodeToFramebuffer(panelImgPath, renderer, config);
      }
      renderer.copyGrayscaleLsbBuffers();

      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
      if (!useCache || !PixelCacheIO::renderFromCache(renderer, cachePath, x, y, fitW, fitH)) {
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
                                                                     int screenW, int screenH) {
  PanelGeom g;

  // Rotate when the panel's aspect doesn't match the screen's, same as EPUB
  // full-page images: this lets a wide (landscape) panel fill a portrait
  // screen edge-to-edge instead of shrinking to fit within its width, and
  // vice versa. The user tilts the device to view a rotated panel. Pure math
  // (no renderer access) -- see computeFullPageGeom for the swap rationale.
  const bool screenIsPortrait = screenH > screenW;
  const bool panelIsLandscape = imgWidth > imgHeight;
  g.rotated = screenIsPortrait == panelIsLandscape;
  if (g.rotated) {
    std::swap(screenW, screenH);
  }

  // Fit panel image to screen preserving aspect ratio. Unlike inline EPUB
  // images (which deliberately never upscale), a panel crop should always
  // be blown up to fill as much of the screen as possible -- that's the
  // whole point of zooming into it. Compute the exact target size here and
  // pass useExactDimensions so the decoder skips its own upscale-disabled
  // fit-or-shrink logic.
  const float scale = std::min(static_cast<float>(screenW) / imgWidth, static_cast<float>(screenH) / imgHeight);
  g.fitW = std::max(1, static_cast<int>(imgWidth * scale + 0.5f));
  g.fitH = std::max(1, static_cast<int>(imgHeight * scale + 0.5f));
  g.x = (screenW - g.fitW) / 2;
  g.y = (screenH - g.fitH) / 2;
  return g;
}

MangaReaderActivity::PanelGeom MangaReaderActivity::applyPanelGeometry(const int imgWidth, const int imgHeight) {
  const int savedOrientation = renderer.getOrientation();
  PanelGeom g = computePanelGeom(imgWidth, imgHeight, renderer.getScreenWidth(), renderer.getScreenHeight());
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
  // Screen dims for the pure geometry math, captured once here. A render running concurrently
  // inside its locked section may have the orientation transiently rotated, which could yield
  // swapped dims -- benign: the resulting cache fails renderFromCache's dimension check and the
  // entry falls back to a normal decode (self-healing miss, never a corrupt render).
  prefetchJob.screenW = renderer.getScreenWidth();
  prefetchJob.screenH = renderer.getScreenHeight();
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
    RenderLock lock;
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
    if (!Storage.rename(tmpPath.c_str(), prefetchJob.cachePath.c_str())) {
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
    // Null decoder = transient OOM (nothrow getDecoder), not a bad crop -- leave the slot
    // unprobed so a later entry retries. Only a non-null decoder whose header parse fails is a
    // genuinely bad crop worth caching as -1.
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
    // Transient (nothrow getDecoder returns null on OOM), not a missing/bad crop -- finish this
    // job without poisoning the slot; the panel-entry render re-probes and retries.
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
  const PanelGeom g = computePanelGeom(dims.width, dims.height, prefetchJob.screenW, prefetchJob.screenH);
  RenderConfig config;
  config.x = g.x;
  config.y = g.y;
  config.maxWidth = g.fitW;
  config.maxHeight = g.fitH;
  config.useExactDimensions = true;
  config.useGrayscale = true;
  config.useDithering = true;
  config.cacheOnly = true;
  config.shouldCancel = &prefetchShouldCancel;
  config.cancelCtx = this;
  const std::string tmpPath = prefetchJob.cachePath + ".tmp";
  config.cachePath = tmpPath;
  if (decoder->decodeToFramebuffer(prefetchJob.imgPath, renderer, config)) {
    if (!Storage.rename(tmpPath.c_str(), prefetchJob.cachePath.c_str())) {
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
  if (!book || !DictIndex::isAvailable()) return;
  const ViewMode returnMode = viewMode;
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
  if (combined.empty()) return;
  startActivityForResult(std::make_unique<MangaWordLookupActivity>(
                             renderer, mappedInput, std::move(combined), book->getCachePath() + "/wlscan.bin",
                             static_cast<uint16_t>(currentPage), static_cast<uint16_t>(currentPanel + 1)),
                         [this, returnMode](const ActivityResult&) {
                           viewMode = returnMode;
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

  // Use the MangaWordLookup sub-activity with raw text. The scan cache makes a re-open of the
  // same panel/page text instant (validated by content hash, so the key is just a hint).
  startActivityForResult(std::make_unique<MangaWordLookupActivity>(
                             renderer, mappedInput, std::move(combined), book->getCachePath() + "/wlscan.bin",
                             static_cast<uint16_t>(currentPage), static_cast<uint16_t>(currentPanel + 1)),
                         [this](const ActivityResult&) {
                           viewMode = ViewMode::PanelZoom;
                           requestUpdate();
                         });
}

void MangaReaderActivity::saveProgress() const {
  if (!book) return;
  std::string cachePath = book->getCachePath();

  if (!Storage.exists(cachePath.c_str())) {
    Storage.mkdir(cachePath.c_str());
  }

  uint8_t data[6];
  data[0] = currentPage & 0xFF;
  data[1] = (currentPage >> 8) & 0xFF;
  data[2] = (currentPage >> 16) & 0xFF;
  data[3] = (currentPage >> 24) & 0xFF;
  int16_t panelVal = static_cast<int16_t>(currentPanel);
  memcpy(data + 4, &panelVal, 2);

  ProgressFile::writeAtomic(cachePath, data, sizeof(data));
}

void MangaReaderActivity::loadProgress() {
  if (!book) return;
  std::string cachePath = book->getCachePath();

  HalFile f;
  if (Storage.openFileForRead("MNG", cachePath + "/progress.bin", f)) {
    uint8_t data[6];
    if (f.read(data, 6) == 6) {
      currentPage = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
      int16_t panelVal;
      memcpy(&panelVal, data + 4, 2);
      currentPanel = panelVal;

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

  const std::string bmPath = BookmarkUtil::getBookmarkPath(book->getFolder());
  if (Storage.exists(bmPath.c_str())) {
    String json = Storage.readFile(bmPath.c_str());
    if (!json.isEmpty()) {
      JsonSettingsIO::loadBookmarks(cachedBookmarks, json.c_str());
    }
  }
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

  const std::string path = BookmarkUtil::getBookmarkPath(book->getFolder());
  Storage.mkdir(BookmarkUtil::getBookmarksDir().c_str());
  if (!JsonSettingsIO::saveBookmarks(cachedBookmarks, path.c_str())) {
    LOG_ERR("MNG", "Failed to save bookmarks to: %s", path.c_str());
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

  // hasFootnotes=false (no footnotes in manga), showVerticalToggle=false
  startActivityForResult(
      std::make_unique<EpubReaderMenuActivity>(renderer, mappedInput, book->getTitle(), curPage, totalPages,
                                               bookProgressPercent, SETTINGS.orientation,
                                               /*hasFootnotes=*/false, /*hasBookmarks=*/!cachedBookmarks.empty(),
                                               /*hasWordLookup=*/hasWordLookup, /*showVerticalToggle=*/false,
                                               /*verticalEnabled=*/false, /*furiganaEnabled=*/true,
                                               /*hasPageText=*/hasPageText),
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
    startActivityForResult(std::make_unique<EpubReaderPercentSelectionActivity>(renderer, mappedInput, initialPercent),
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
