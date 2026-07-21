#pragma once

#include <MangaPanel.h>

#include <memory>
#include <string>
#include <vector>

#include "../../BookmarkEntry.h"
#include "EpubReaderMenuActivity.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class MangaReaderActivity final : public Activity {
 public:
  explicit MangaReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                               std::unique_ptr<manga::MangaBook> book)
      : Activity("MangaReader", renderer, mappedInput), book(std::move(book)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  ScreenshotInfo getScreenshotInfo() const override;

 private:
  std::unique_ptr<manga::MangaBook> book;

  uint32_t currentPage = 0;
  int currentPanel = -1;  // -1 = full page view, 0+ = zoomed panel
  int pagesUntilFullRefresh = 0;

  std::vector<manga::Panel> panels;
  bool panelsLoaded = false;

  bool showTextOverlay = false;
  bool pendingScreenshot = false;
  bool ignoreNextConfirmRelease = false;
  unsigned long readingSessionStartMs = 0;

  bool automaticPageTurnActive = false;
  unsigned long lastPageTurnTime = 0;
  unsigned long pageTurnDuration = 0;

  bool currentPageBookmarked = false;
  bool showBookmarkMessage = false;
  bool bookmarkRemoved = false;  // true when last toggle removed (controls popup text)
  unsigned long bookmarkMessageTime = 0UL;
  std::vector<BookmarkEntry> cachedBookmarks;

  ButtonNavigator buttonNavigator;

  enum class ViewMode { FullPage, PanelZoom, TextOverlay };
  ViewMode viewMode = ViewMode::FullPage;

  // Set when a full page finishes rendering; the idle prefetch below warms the NEXT page's pixel
  // cache once per displayed page, after a short dwell, so forward page turns hit the cache
  // instead of running a fresh JPEG decode.
  bool nextPagePrefetched = true;  // true until the first full-page render arms it
  unsigned long fullPageRenderedMs = 0;

  // Geometry of a full-page image on the (possibly temporarily rotated) screen. Shared by
  // renderFullPage() and prefetchNextPageCache() so the prefetch-written pixel cache has exactly
  // the dimensions the later render expects.
  struct FullPageGeom {
    int x = 0, y = 0;
    int destWidth = 0, destHeight = 0;
    int screenW = 0, screenH = 0;
    bool rotated = false;
    int savedOrientation = 0;  // GfxRenderer::Orientation of the caller, restored after use
  };
  // NOTE: sets the renderer orientation when rotation is needed -- the caller must restore
  // savedOrientation when done.
  FullPageGeom applyFullPageGeometry(int imgWidth, int imgHeight);

  void prefetchNextPageCache();

  // Panel-zoom prefetch. Armed the first time panel-zoom actually renders in this book, so
  // full-page-only readers never pay the speculative decode/SD-write cost. Once armed, idle
  // dwell on a full page warms the first panel's pixel cache, and idle dwell on a panel warms
  // the next panel's -- entering a panel then costs a cache read instead of a JPEG decode.
  bool panelPrefetchArmed = false;
  bool firstPanelPrefetched = true;  // per-page; true until loadCurrentPagePanels() arms it
  bool nextPanelPrefetched = true;   // per-panel; true until a panel render arms it
  unsigned long panelRenderedMs = 0;

  // Per-page facts cached off the hot paths: the input handler used to hit the SD (exists())
  // on every full-page -> panel press, and renderPanelZoom re-parsed the crop's JPEG header on
  // every entry. Both answers are static for a given page.
  bool pageHasPanelCrops = false;
  // True when the current full-page image is a 1-bit monochrome BMP: it renders with a single BW
  // e-ink pass (no 4-level gray refresh), so renderFullPage skips the grayscale planes entirely.
  // Computed once per page in loadCurrentPagePanels(). Panel crops are always JPEG, so panel-zoom
  // is unaffected. Read on the render task, written under RenderLock (see panelDims note below).
  bool currentPageBwOnly = false;
  struct PanelCropDims {
    // Sentinels: 0 = not probed yet (probe on next need); -1 = crop known missing/invalid
    // (never re-probe -- render falls back to full page without touching the SD).
    int w = 0, h = 0;
  };
  // render() (render task) reads `panels` and `panelDims` under RenderLock. Every writer on the
  // loop task -- loadCurrentPagePanels() (whole-vector swap) and prefetchPanelCache() (per-slot)
  // -- must take RenderLock around the write; renderPanelZoom() writes them while already
  // holding the lock render() handed it. Keep the standalone SD probes (the panel-index load,
  // exists(), getDimensions()) OUTSIDE the lock so they don't stall the render task; the decode
  // itself necessarily runs under the lock because it draws into the renderer's framebuffer, and
  // its file read is part of that -- that is expected, not a violation of the rule above.
  std::vector<PanelCropDims> panelDims;

  // Geometry of a zoomed panel on the (possibly temporarily rotated) screen. Shared by
  // renderPanelZoom() and prefetchPanelCache() so the prefetch-written pixel cache has exactly
  // the dimensions the later render expects. Same restore contract as applyFullPageGeometry.
  struct PanelGeom {
    int x = 0, y = 0;
    int fitW = 0, fitH = 0;
    bool rotated = false;
    int savedOrientation = 0;
  };
  PanelGeom applyPanelGeometry(int imgWidth, int imgHeight);

  std::string panelCropPath(int panelIdx) const;
  void prefetchPanelCache(int panelIdx);

  void loadCurrentPagePanels();
  void renderFullPage();
  void renderPanelZoom();
  void renderTextOverlay();

  void nextPanel();
  void prevPanel();
  void nextPage();
  void prevPage();

  void saveProgress() const;
  void loadProgress();

  void loadCachedBookmarks();
  void updateBookmarkFlag();
  void addBookmark();

  void launchWordLookup();
  void launchWordLookupCurrentView();
  void launchMenu();
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  void toggleAutoPageTurn(uint8_t selectedPageTurnOption);
};
