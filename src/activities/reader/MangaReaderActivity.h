#pragma once

#include <MangaPanel.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
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

  // Prefetch state flags below are written from the render task (renderFullPage/renderPanelZoom)
  // and read/updated from the loop task (loop/loadCurrentPagePanels). They're word-sized and only
  // advisory (a stale read at worst skips or delays one prefetch, never corrupts state), and on
  // this single-core target an aligned load/store can't tear -- but std::atomic makes the
  // cross-task access well-defined per the C++ memory model at zero cost (native 32-bit atomics).

  // Set when a full page finishes rendering; the idle prefetch below warms the NEXT page's pixel
  // cache once per displayed page, after a short dwell, so forward page turns hit the cache
  // instead of running a fresh JPEG decode.
  std::atomic<bool> nextPagePrefetched{true};  // true until the first full-page render arms it
  std::atomic<unsigned long> fullPageRenderedMs{0};

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
  // Pure fit/rotate math shared by applyFullPageGeometry (render path) and the prefetch worker.
  // Touches NO renderer state: rotation is just a screen-dim swap here, which matches what
  // setOrientation((o+3)%4) does to getScreenWidth/Height. savedOrientation is left at 0.
  static FullPageGeom computeFullPageGeom(int imgWidth, int imgHeight, int screenW, int screenH);

  void prefetchNextPageCache();

  // Panel-zoom prefetch. Armed the first time panel-zoom actually renders in this book, so
  // full-page-only readers never pay the speculative decode/SD-write cost. Once armed, idle
  // dwell on a full page warms the first panel's pixel cache, and idle dwell on a panel warms
  // the next panel's -- entering a panel then costs a cache read instead of a JPEG decode.
  std::atomic<bool> panelPrefetchArmed{false};
  std::atomic<bool> firstPanelPrefetched{true};  // per-page; true until loadCurrentPagePanels() arms it
  std::atomic<bool> nextPanelPrefetched{true};   // per-panel; true until a panel render arms it
  std::atomic<unsigned long> panelRenderedMs{0};

  // Deferred grayscale upgrade for panel-zoom. A fresh grayscale (JPEG / >=8-bit BMP) panel entry
  // shows only the fast single BW wave; the slower 4-level gray wave is deferred until the reader
  // dwells on the panel, so stepping panel->panel->panel pays one wave each instead of two. 1-bit
  // (bwOnly) panels are already single-wave and never defer. Both flags are atomic for the same
  // reason as the prefetch flags above (written/read across the loop and render tasks).
  //   panelGrayPending -- set to true ONLY by the render task, after a successful deferred BW-only
  //     panel render (so a fallback such as a missing crop leaves nothing pending). Cleared to false
  //     by both tasks: the render task clears it at every renderPanelZoom() entry, and the loop task
  //     clears it when it hands the panel off for its upgrade (once per dwell) and when panel
  //     navigation/fresh entry cancels a pending upgrade. Only one writer ever sets true, and false
  //     is idempotent, so the multiple clearers don't race destructively.
  //   panelGrayUpgrade -- set to true by the loop task (dwell elapsed) to ask renderPanelZoom() to
  //     build the gray planes over the on-screen BW image; the render task consumes it via
  //     exchange(). Panel navigation clears it so an already-elapsed dwell can't upgrade a panel the
  //     reader has just stepped away from.
  std::atomic<bool> panelGrayPending{false};
  std::atomic<bool> panelGrayUpgrade{false};

  // Per-page facts cached off the hot paths: the input handler used to hit the SD (exists())
  // on every full-page -> panel press, and renderPanelZoom re-parsed the crop's JPEG header on
  // every entry. Both answers are static for a given page.
  bool pageHasPanelCrops = false;
  // True when the current full-page image is a 1-bit monochrome BMP: it renders with a single BW
  // e-ink pass (no 4-level gray refresh), so renderFullPage skips the grayscale planes entirely.
  // Computed once per page in loadCurrentPagePanels(). Read on the render task, written under
  // RenderLock (see panelDims note below).
  bool currentPageBwOnly = false;
  // Panel crop format for this book (uniform per book): the converter writes p<page>_<panel>.jpg
  // normally, or .bmp with --mono. panelCropPath() picks the extension from this.
  bool panelCropIsBmp = false;
  // True when this page's panel crops are 1-bit monochrome BMP -> renderPanelZoom takes the same
  // single-BW-wave fast path as a mono full page. Detected once per page in loadCurrentPagePanels.
  bool panelsBwOnly = false;
  struct PanelCropDims {
    // Sentinels: 0 = not probed yet (probe on next need); -1 = crop known missing/invalid
    // (never re-probe -- render falls back to full page without touching the SD).
    int w = 0, h = 0;
  };
  // render() (render task) reads `panels` and `panelDims` under RenderLock. Every writer on the
  // loop task -- loadCurrentPagePanels() (whole-vector swap) and applyPrefetchResult() (per-slot
  // publication of worker probes) -- must take RenderLock around the write; renderPanelZoom()
  // writes them while already holding the lock render() handed it. The prefetch WORKER task never
  // writes these (or takes the lock) -- it hands results to applyPrefetchResult instead; see the
  // worker doc block below. Keep standalone SD probes (the panel-index load, exists(),
  // getDimensions()) OUTSIDE the lock so they don't stall the render task; the render decode
  // itself necessarily runs under the lock because it draws into the renderer's framebuffer, and
  // its file read is part of that -- that is expected, not a violation of the rule above.
  std::vector<PanelCropDims> panelDims;

  // ---- Background prefetch worker ----
  // The speculative cache warms (next page / panels) used to decode JPEGs synchronously inside
  // loop() -- on the SAME task that polls the buttons. A ~1s decode meant gpio.update() stopped
  // being called for that whole window; button edges are events, not queued state, so presses
  // AND releases landing inside the window simply evaporated ("takes multiple clicks to
  // register" on JPEG books). The warms now run on this dedicated worker task instead.
  //
  // Design rules (each one is a hard invariant, not a preference):
  //  * The worker is LOCK-FREE: it never takes RenderLock. ActivityManager's pop path calls
  //    onExit() while HOLDING RenderLock, and onExit() joins this worker -- a worker blocked on
  //    that same lock would deadlock the join. Consequently the worker also never touches the
  //    renderer: decodes run cacheOnly (see RenderConfig), and geometry comes from the pure
  //    computeFullPageGeom/computePanelGeom helpers with screen dims captured at post time.
  //  * Results are handed BACK to the loop task (applyPrefetchResult), which owns the existing
  //    locked-write discipline for panelDims and the done-flags. The worker never writes them.
  //  * The worker decodes into cachePath + ".tmp" and publishes by rename. A real render may
  //    decode-and-cache the SAME asset concurrently (it streams to the real path); two write
  //    handles on one FAT file corrupt the volume. SdFat's rename opens the destination
  //    O_CREAT|O_EXCL (verified, SdFat 2.3.1 FatFile::rename), so publish fails cleanly if the
  //    render got there first -- the worker then just deletes its tmp.
  //  * Cancellation: the decode's per-block cancel probe checks prefetchExitRequested and
  //    RenderLock::peek(). The moment a real render starts (or the activity exits), the decode
  //    aborts within one MCU block/scanline and its partial tmp is dropped -- so a background
  //    warm never competes with a foreground render for CPU/SD beyond a few ms.
  //  * Single-slot job queue with strict ownership ping-pong: loop() writes prefetchJob and
  //    prefetchResult only while prefetchBusy is false and no result is pending; the worker
  //    touches them only between the notify that follows busy=true and its own busy=false.
  //    The strings/struct members therefore need no locking of their own.
  //  * pageGeneration stamps jobs; loadCurrentPagePanels() bumps it (under RenderLock) on every
  //    page change, so results of a stale in-flight warm are dropped instead of poisoning the
  //    new page's panelDims/flags.
  TaskHandle_t prefetchTaskHandle = nullptr;
  std::atomic<bool> prefetchExitRequested{false};
  std::atomic<bool> prefetchTaskExited{false};
  std::atomic<bool> prefetchBusy{false};
  std::atomic<uint32_t> pageGeneration{0};

  struct PrefetchJob {
    bool isPanel = false;
    bool isFirstPanel = false;  // which done-flag a panel job resolves
    int panelIdx = -1;
    uint32_t gen = 0;       // pageGeneration at post time
    int screenW = 0;        // base (unrotated) screen dims captured at post time --
    int screenH = 0;        //   inputs to the pure geometry helpers
    std::string imgPath;    // page image or panel crop
    std::string cachePath;  // real .2bp target; worker writes cachePath + ".tmp", publishes by rename
  };
  PrefetchJob prefetchJob;

  struct PrefetchResult {
    bool pending = false;    // worker sets before clearing busy; loop clears after applying
    bool completed = false;  // job reached a terminal state (vs deferred/cancelled -> retry later)
    bool dimsValid = false;  // panel jobs: publish dims into panelDims[job.panelIdx]
    PanelCropDims dims{};
  };
  PrefetchResult prefetchResult;

  static void prefetchTaskTrampoline(void* param);
  void prefetchTaskLoop();
  static bool prefetchShouldCancel(void* selfPtr);
  void postPrefetchJob(PrefetchJob&& job);
  void applyPrefetchResult();  // loop task: publish worker results under the normal lock rules
  void workerWarmNextPage();
  void workerWarmPanel();

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
  // Pure twin of applyPanelGeometry, same contract as computeFullPageGeom (no renderer access).
  static PanelGeom computePanelGeom(int imgWidth, int imgHeight, int screenW, int screenH);

  std::string panelCropPath(int panelIdx) const;                      // uses panelCropIsBmp for the extension
  std::string panelCropPathExt(int panelIdx, const char* ext) const;  // explicit extension (format detection)
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
