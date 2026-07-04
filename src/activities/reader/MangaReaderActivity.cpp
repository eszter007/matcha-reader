#include "MangaReaderActivity.h"

#include <Bitmap.h>
#include <DictIndex.h>
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

#include <HalClock.h>

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
    for (const auto& r : RECENT_BOOKS.getBooks()) {
      if (r.path == book->getFolder()) { bookAuthor = r.author; break; }
    }
  }
  RECENT_BOOKS.addBook(book->getFolder(), book->getTitle(), bookAuthor, book->getPageImagePath(0));

  readingSessionStartMs = millis();

  loadCurrentPagePanels();
  requestUpdate();
}

void MangaReaderActivity::onExit() {
  uint16_t minutes = 0;
  if (readingSessionStartMs > 0) {
    unsigned long elapsed = millis() - readingSessionStartMs;
    minutes = static_cast<uint16_t>(elapsed / 60000);
  }

  // On the last page (currentPage never advances past pageCount-1 -- see
  // nextPage()) regardless of how long this particular session lasted.
  // Previously this was nested inside `minutes > 0` AND compared against
  // pageCount instead of pageCount-1, so it could never fire -- manga never
  // got marked finished even at 100% progress, unlike Epub/Txt readers.
  const bool atLastPage = book && book->getPageCount() > 0 && currentPage >= book->getPageCount() - 1;

  if (minutes > 0 || atLastPage) {
    READING_STATS.loadFromFile();
    if (minutes > 0) {
      // Local-midnight day boundary -- see the matching call in EpubReaderActivity.
      time_t now = HalClock::localEpoch(SETTINGS.clockUtcOffsetQ);
      struct tm* t = gmtime(&now);
      READING_STATS.addMinutes(static_cast<uint16_t>(t->tm_year + 1900), static_cast<uint8_t>(t->tm_mon + 1),
                               static_cast<uint8_t>(t->tm_mday), minutes);
    }
    if (atLastPage) {
      READING_STATS.markBookFinished(book->getFolder());
    }
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
  panels.clear();
  panelsLoaded = false;
  if (book && currentPage < book->getPageCount()) {
    panelsLoaded = book->loadPagePanels(currentPage, panels);
  }
  updateBookmarkFlag();
}

void MangaReaderActivity::nextPanel() {
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

namespace {
constexpr int PAGE_TURN_RATES[] = {1, 1, 3, 6, 12};
}  // namespace

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

  if (mappedInput.isPressed(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
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
    // Idle tick: after a short dwell on a full page, warm the NEXT page's pixel cache so the
    // upcoming forward page turn renders from cache instead of running its JPEG decode. The
    // dwell gate keeps rapid back-to-back turns from queueing a blocking decode between them.
    if (viewMode == ViewMode::FullPage && !nextPagePrefetched && (millis() - fullPageRenderedMs) > 400) {
      prefetchNextPageCache();
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
      bool hasPanelCrops = false;
      if (!panels.empty() && book) {
        char cropName[64];
        snprintf(cropName, sizeof(cropName), "p%u_0.jpg", currentPage);
        std::string cropPath = book->getFolder();
        if (cropPath.back() != '/') cropPath += '/';
        cropPath += cropName;
        hasPanelCrops = Storage.exists(cropPath.c_str());
      }
      if (hasPanelCrops) {
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
    renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() / 2,
                              tr(STR_END_OF_BOOK), true, EpdFontFamily::BOLD);
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

MangaReaderActivity::FullPageGeom MangaReaderActivity::applyFullPageGeometry(const int imgWidth,
                                                                             const int imgHeight) {
  FullPageGeom g;
  g.savedOrientation = renderer.getOrientation();
  g.screenW = renderer.getScreenWidth();
  g.screenH = renderer.getScreenHeight();

  // Rotate when the page's aspect doesn't match the screen's -- same
  // fill-the-screen behavior as panel-zoom (renderPanelZoom): this lets a
  // portrait manga page fill a landscape-oriented screen edge-to-edge
  // instead of shrinking to a small centered box. The user tilts the
  // device to read a rotated page.
  const bool screenIsPortrait = g.screenH > g.screenW;
  const bool pageIsLandscape = imgWidth > imgHeight;
  g.rotated = screenIsPortrait == pageIsLandscape;
  if (g.rotated) {
    const auto rotatedOrientation = static_cast<GfxRenderer::Orientation>((g.savedOrientation + 3) % 4);
    renderer.setOrientation(rotatedOrientation);
    g.screenW = renderer.getScreenWidth();
    g.screenH = renderer.getScreenHeight();
  }

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

void MangaReaderActivity::renderFullPage() {
  renderer.clearScreen();

  std::string imgPath = book->getPageImagePath(currentPage);
  if (imgPath.empty()) {
    renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() / 2,
                              tr(STR_PAGE_LOAD_ERROR), true);
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

  // Cache path for decoded pixel data (avoids re-decoding JPG on grayscale passes)
  std::string cachePath = book->getCachePath() + "/page_" + std::to_string(currentPage) + ".2bp";

  RenderConfig config;
  config.x = x;
  config.y = y;
  config.maxWidth = screenW;
  config.maxHeight = screenH;
  config.useGrayscale = true;
  config.useDithering = true;
  config.cachePath = cachePath;

  // BW pass — a page visited before (or warmed by the idle prefetch) has its decoded pixels
  // cached on SD; render those directly and skip the JPEG decode entirely. Otherwise decode to
  // framebuffer AND stream the pixel cache to disk (config.cachePath) so the two grayscale passes
  // below can read the already-decoded pixels back instead of re-running the full JPEG decode.
  // Confirmed on a real device: without the cache, every manga page turn ran the decode 3 times
  // (BW + LSB + MSB), each a full JPEG parse/IDCT/scale -- the dominant cost of "turning pages in
  // manga is slow". ImageBlock (regular EPUB images) already had this same cache-read
  // optimization; manga bypassed ImageBlock entirely and never got it.
  if (!PixelCacheIO::renderFromCache(renderer, cachePath, x, y, destWidth, destHeight)) {
    decoder->decodeToFramebuffer(imgPath, renderer, config);
  }

  // Status bar: page number
  char statusBuf[32];
  snprintf(statusBuf, sizeof(statusBuf), "%u/%u", currentPage + 1, book->getPageCount());
  int statusW = renderer.getTextWidth(SMALL_FONT_ID, statusBuf);
  int statusX = screenW - statusW - 4;
  int statusY = screenH - renderer.getLineHeight(SMALL_FONT_ID) - 2;
  renderer.fillRect(statusX - 2, statusY - 1, statusW + 4,
                    renderer.getLineHeight(SMALL_FONT_ID) + 2, false);
  renderer.drawText(SMALL_FONT_ID, statusX, statusY, statusBuf, true);

  // Display with grayscale: BW first, then LSB/MSB planes for 4-level gray.
  renderer.storeBwBuffer();
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);

  // Read the pixels the BW pass just cached instead of re-decoding the JPEG. Falls back to a
  // real decode if the cache write failed (e.g. under memory pressure) or wasn't enabled.
  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  if (!PixelCacheIO::renderFromCache(renderer, cachePath, x, y, destWidth, destHeight)) {
    decoder->decodeToFramebuffer(imgPath, renderer, config);
  }
  renderer.copyGrayscaleLsbBuffers();

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  if (!PixelCacheIO::renderFromCache(renderer, cachePath, x, y, destWidth, destHeight)) {
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
  const uint32_t nextPage = currentPage + 1;
  if (nextPage >= book->getPageCount()) {
    nextPagePrefetched = true;
    return;
  }
  const std::string cachePath = book->getCachePath() + "/page_" + std::to_string(nextPage) + ".2bp";
  if (Storage.exists(cachePath.c_str())) {
    nextPagePrefetched = true;
    return;
  }
  // The decode needs working buffers plus the 48KB framebuffer snapshot below; under pressure,
  // skip permanently for this page (the page-turn decode handles it as before).
  if (ESP.getMaxAllocHeap() < 60000) {
    nextPagePrefetched = true;
    return;
  }
  // The renderer belongs to the render task; only touch it under the rendering mutex, and don't
  // stall a queued render -- retry on a later idle tick instead.
  if (RenderLock::peek()) return;

  const std::string imgPath = book->getPageImagePath(nextPage);
  ImageToFramebufferDecoder* decoder = imgPath.empty() ? nullptr : ImageDecoderFactory::getDecoder(imgPath);
  ImageDimensions dims = {0, 0};
  if (!decoder || !decoder->getDimensions(imgPath, dims) || dims.width <= 0 || dims.height <= 0) {
    nextPagePrefetched = true;
    return;
  }

  RenderLock lock;
  // The decode scribbles the framebuffer (RAM only -- nothing is displayed here); snapshot and
  // restore it so the on-screen frame survives for later partial redraws and overlays.
  if (!renderer.storeBwBuffer()) {
    nextPagePrefetched = true;
    return;
  }
  LOG_DBG("MRA", "Prefetching pixel cache for page %u", static_cast<unsigned>(nextPage));
  const FullPageGeom g = applyFullPageGeometry(dims.width, dims.height);
  RenderConfig config;
  config.x = g.x;
  config.y = g.y;
  config.maxWidth = g.screenW;
  config.maxHeight = g.screenH;
  config.useGrayscale = true;
  config.useDithering = true;
  config.cachePath = cachePath;
  decoder->decodeToFramebuffer(imgPath, renderer, config);
  if (g.rotated) {
    renderer.setOrientation(static_cast<GfxRenderer::Orientation>(g.savedOrientation));
  }
  renderer.restoreBwBuffer();
  nextPagePrefetched = true;
}

void MangaReaderActivity::renderPanelZoom() {
  renderer.clearScreen();

  if (currentPanel < 0 || currentPanel >= static_cast<int>(panels.size())) {
    renderFullPage();
    return;
  }

  const auto& panel = panels[currentPanel];

  std::string imgPath = book->getPageImagePath(currentPage);
  if (imgPath.empty()) {
    renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() / 2,
                              tr(STR_PAGE_LOAD_ERROR), true);
    renderer.displayBuffer();
    return;
  }

  int screenW = renderer.getScreenWidth();
  int screenH = renderer.getScreenHeight();

  // Try to load a pre-cropped panel image (p<page>_<panel>.jpg)
  char panelFileName[64];
  snprintf(panelFileName, sizeof(panelFileName), "p%d_%d.jpg", currentPage, currentPanel);
  std::string panelImgPath = book->getFolder();
  if (panelImgPath.back() != '/') panelImgPath += '/';
  panelImgPath += panelFileName;

  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(panelImgPath);
  if (!decoder || !Storage.exists(panelImgPath.c_str())) {
    // No pre-cropped panel image — fall back to full page view
    renderFullPage();
    return;
  }

  ImageDimensions panelDims = {0, 0};
  if (!decoder->getDimensions(panelImgPath, panelDims) || panelDims.width <= 0 || panelDims.height <= 0) {
    renderFullPage();
    return;
  }

  // Rotate when the panel's aspect doesn't match the screen's, same as EPUB
  // full-page images: this lets a wide (landscape) panel fill a portrait
  // screen edge-to-edge instead of shrinking to fit within its width, and
  // vice versa. The user tilts the device to view a rotated panel.
  const auto savedOrientation = renderer.getOrientation();
  const bool screenIsPortrait = screenH > screenW;
  const bool panelIsLandscape = panelDims.width > panelDims.height;
  const bool rotatePanel = screenIsPortrait == panelIsLandscape;
  if (rotatePanel) {
    const auto rotatedOrientation = static_cast<GfxRenderer::Orientation>((savedOrientation + 3) % 4);
    renderer.setOrientation(rotatedOrientation);
    screenW = renderer.getScreenWidth();
    screenH = renderer.getScreenHeight();
  }

  // Fit panel image to screen preserving aspect ratio. Unlike inline EPUB
  // images (which deliberately never upscale), a panel crop should always
  // be blown up to fill as much of the screen as possible -- that's the
  // whole point of zooming into it. Compute the exact target size here and
  // pass useExactDimensions so the decoder skips its own upscale-disabled
  // fit-or-shrink logic.
  float scale = std::min(static_cast<float>(screenW) / panelDims.width,
                         static_cast<float>(screenH) / panelDims.height);
  int fitW = std::max(1, static_cast<int>(panelDims.width * scale + 0.5f));
  int fitH = std::max(1, static_cast<int>(panelDims.height * scale + 0.5f));
  int x = (screenW - fitW) / 2;
  int y = (screenH - fitH) / 2;

  std::string cachePath = book->getCachePath() + "/p" + std::to_string(currentPage)
                          + "_" + std::to_string(currentPanel) + ".2bp";
  RenderConfig config;
  config.x = x;
  config.y = y;
  config.maxWidth = fitW;
  config.maxHeight = fitH;
  config.useExactDimensions = true;
  config.useGrayscale = true;
  config.useDithering = true;
  config.cachePath = cachePath;

  // BW pass -- a revisited panel renders from its pixel cache and skips the crop-JPEG decode,
  // same as renderFullPage().
  if (!PixelCacheIO::renderFromCache(renderer, cachePath, x, y, fitW, fitH)) {
    decoder->decodeToFramebuffer(panelImgPath, renderer, config);
  }

  // Panel indicator and status
  char statusBuf[48];
  snprintf(statusBuf, sizeof(statusBuf), "%d/%d  %u/%u",
           currentPanel + 1, (int)panels.size(),
           currentPage + 1, book->getPageCount());
  int statusW = renderer.getTextWidth(SMALL_FONT_ID, statusBuf);
  int statusX = screenW - statusW - 4;
  int statusY = screenH - renderer.getLineHeight(SMALL_FONT_ID) - 2;
  renderer.fillRect(statusX - 2, statusY - 1, statusW + 4,
                    renderer.getLineHeight(SMALL_FONT_ID) + 2, false);
  renderer.drawText(SMALL_FONT_ID, statusX, statusY, statusBuf, true);

  // "Panels" hint — always shown in panel-zoom, indicates this mode and
  // that Confirm opens the full reader menu (with Word Lookup, Translate, etc.)
  {
    const char* hint = tr(STR_PANELS_MODE_HINT);
    renderer.fillRect(2, statusY - 1, renderer.getTextWidth(SMALL_FONT_ID, hint) + 4,
                      renderer.getLineHeight(SMALL_FONT_ID) + 2, false);
    renderer.drawText(SMALL_FONT_ID, 4, statusY, hint, true);
  }

  // Display with grayscale
  renderer.storeBwBuffer();
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);

  // Read back the pixels the BW pass cached instead of re-decoding the JPEG for each grayscale
  // plane -- same fix as renderFullPage(), see the comment there for the full rationale.
  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  if (!PixelCacheIO::renderFromCache(renderer, cachePath, x, y, fitW, fitH)) {
    decoder->decodeToFramebuffer(panelImgPath, renderer, config);
  }
  renderer.copyGrayscaleLsbBuffers();

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  if (!PixelCacheIO::renderFromCache(renderer, cachePath, x, y, fitW, fitH)) {
    decoder->decodeToFramebuffer(panelImgPath, renderer, config);
  }
  renderer.copyGrayscaleMsbBuffers();

  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.restoreBwBuffer();

  if (rotatePanel) {
    renderer.setOrientation(savedOrientation);
  }
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
  renderer.drawText(UI_12_FONT_ID, screen.x + metrics.contentSidePadding, textY, headerBuf, true,
                    EpdFontFamily::BOLD);
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
        if (c0 >= 0xF0) charLen = 4;
        else if (c0 >= 0xE0) charLen = 3;
        else if (c0 >= 0xC0) charLen = 2;
        std::string test = accum + std::string(p, charLen);
        if (renderer.getTextWidth(jaFont, test.c_str()) > maxWidth) break;
        accum = test;
        p += charLen;
      }

      if (accum.empty()) {
        auto c0 = static_cast<unsigned char>(remaining[0]);
        size_t cl = 1;
        if (c0 >= 0xF0) cl = 4;
        else if (c0 >= 0xE0) cl = 3;
        else if (c0 >= 0xC0) cl = 2;
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

  const auto labels = mappedInput.mapLabels(tr(STR_BACK),
                                            DictIndex::isAvailable() ? tr(STR_WORD_LOOKUP) : "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
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
  startActivityForResult(
      std::make_unique<MangaWordLookupActivity>(renderer, mappedInput, std::move(combined),
                                                book->getCachePath() + "/wlscan.bin",
                                                static_cast<uint16_t>(currentPage),
                                                static_cast<uint16_t>(currentPanel + 1)),
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
  currentPageBookmarked =
      std::any_of(cachedBookmarks.begin(), cachedBookmarks.end(), [&](const BookmarkEntry& b) {
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
  const int bookProgressPercent =
      totalPages > 0 ? static_cast<int>((currentPage + 1) * 100 / totalPages) : 0;

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
      hasPageText = std::any_of(panels.begin(), panels.end(),
                                [](const auto& p) { return !p.textBlocks.empty(); });
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
    const int initialPercent =
        totalPages > 0 ? static_cast<int>((currentPage + 1) * 100 / totalPages) : 0;
    startActivityForResult(
        std::make_unique<EpubReaderPercentSelectionActivity>(renderer, mappedInput, initialPercent),
        [this](const ActivityResult& result) {
          if (!result.isCancelled && book) {
            const int percent = std::get<PercentResult>(result.data).percent;
            const uint32_t totalPages = book->getPageCount();
            uint32_t targetPage = static_cast<uint32_t>(
                static_cast<float>(percent) / 100.0f * static_cast<float>(totalPages));
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
      // In panel zoom, look up just that panel's text. In full-page view,
      // combine every panel's text on the page so lookup still works
      // without having to zoom into each panel individually.
      ViewMode returnMode = viewMode;
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
      if (!combined.empty() && DictIndex::isAvailable()) {
        startActivityForResult(
            std::make_unique<MangaWordLookupActivity>(renderer, mappedInput, std::move(combined),
                                                      book->getCachePath() + "/wlscan.bin",
                                                      static_cast<uint16_t>(currentPage),
                                                      static_cast<uint16_t>(currentPanel + 1)),
            [this, returnMode](const ActivityResult&) {
              viewMode = returnMode;
              requestUpdate();
            });
        return;
      }
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
        startActivityForResult(
            std::make_unique<EpubReaderTranslationActivity>(renderer, mappedInput, std::move(combined),
                                                             std::move(preTranslated)),
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
    info.progressPercent = book->getPageCount() > 0
        ? static_cast<int>((currentPage + 1) * 100 / book->getPageCount())
        : 0;
  }
  return info;
}
