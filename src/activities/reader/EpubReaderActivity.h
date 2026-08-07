#pragma once
#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/Section.h>
#include <Epub/VerticalSection.h>

#include <atomic>
#include <optional>

#include "BookmarkEntry.h"
#include "EndOfBookOptions.h"
#include "EpubReaderMenuActivity.h"
#include "ProgressMapper.h"
#include "activities/Activity.h"

class EpubReaderActivity final : public Activity {
  std::shared_ptr<Epub> epub;
  std::unique_ptr<Section> section = nullptr;
  std::unique_ptr<VerticalSection> verticalSection = nullptr;
  // Spine index whose vertical section last failed to build (e.g. transient low-heap allocation
  // failure). Prevents an immediate automatic retry loop: without this, verticalSection.reset()
  // on failure leaves `!verticalSection` true, so the very next render() call retries the same
  // expensive build (indexing an entire chapter) again -- observed on a real device as an
  // indefinite "Indexing" popup that silently re-failed every ~12 seconds with no visible error.
  int failedVerticalSpineIndex = -1;
  // Same guard as failedVerticalSpineIndex, for horizontal-mode Section builds. Without this,
  // section.reset() on a build failure leaves `!section` true, so the next render() retries the
  // same build immediately -- observed as an indefinite "Indexing" popup on a chapter whose
  // horizontal build fails for the same reason vertical builds can (a hard-to-satisfy contiguous
  // allocation, e.g. the zip inflate window, failing under a tight/fragmented heap).
  int failedSectionSpineIndex = -1;
  int currentSpineIndex = 0;
  int nextPageNumber = 0;
  std::optional<uint16_t> pendingPageJump;
  // Set when navigating to a footnote href with a fragment (e.g. #note1).
  // Cleared on the next render after the new section loads and resolves it to a page.
  std::string pendingAnchor;
  int pagesUntilFullRefresh = 0;
  // Image pages use a dedicated double-FAST refresh path, so retain a manual
  // refresh request until renderContents can issue its clean base pass.
  bool forcedRefreshPending = false;
  int cachedSpineIndex = 0;
  int cachedChapterTotalPageCount = 0;

  // Snapshot of the layout-affecting settings the currently-resident section was
  // built with. The reader menu is pushed on top of this activity, so editing a
  // setting (e.g. screenMargin) never null-resets the section -- the new value
  // moves the draw origin but the cached line layout keeps the old width, so text
  // overflows one side until the book is reopened. render() compares this against
  // the current settings and reflows in place on a mismatch.
  struct LayoutSig {
    int fontId = -1;
    uint16_t viewportWidth = 0;
    uint16_t viewportHeight = 0;
    float lineCompression = 0.0f;
    uint8_t paragraphAlignment = 0;
    bool extraParagraphSpacing = false;
    bool hyphenationEnabled = false;
    bool embeddedStyle = false;
    uint8_t imageRendering = 0;
    bool focusReadingEnabled = false;
    bool bookCssMargins = false;
    // Vertical-only inputs. They key the vertical cache FILE, so leaving them out here meant an
    // in-memory section built with the old values kept being served: changing line spacing mid-book
    // left the columns at their previous 行間 until the book was reopened.
    uint8_t lineSpacing = 0;
    bool furigana = false;
    bool operator==(const LayoutSig& o) const {
      return fontId == o.fontId && viewportWidth == o.viewportWidth && viewportHeight == o.viewportHeight &&
             lineCompression == o.lineCompression && paragraphAlignment == o.paragraphAlignment &&
             extraParagraphSpacing == o.extraParagraphSpacing && hyphenationEnabled == o.hyphenationEnabled &&
             embeddedStyle == o.embeddedStyle && imageRendering == o.imageRendering &&
             focusReadingEnabled == o.focusReadingEnabled && bookCssMargins == o.bookCssMargins &&
             lineSpacing == o.lineSpacing && furigana == o.furigana;
    }
    bool operator!=(const LayoutSig& o) const { return !(*this == o); }
  };
  LayoutSig sectionLayoutSig;

  // Per-book reader preferences. The global settings page (opened from home)
  // holds the DEFAULTS: a book with no prefs file opens with them. Once a book
  // has been opened, its reading-relevant settings are pinned to the book
  // (readerprefs.bin in its cache dir) and reapplied on every open; settings
  // edited while reading affect only the book -- the global values captured in
  // globalPrefsSnapshot are restored (RAM and, if a mid-session save leaked
  // book values into the global file, re-saved) on exit. Vertical/furigana
  // overrides already live per-book in progress.bin and are untouched.
  struct ReaderPrefs {
    uint8_t fontFamily = 0;
    char sdFontFamilyName[32] = {};
    // Point size since the 1.5.0 merge (was a size-enum slot); see PREFS_VERSION.
    uint8_t fontPointSize = 0;
    uint8_t lineSpacing = 0;
    uint8_t screenMargin = 0;
    uint8_t bookCssMargins = 0;
    uint8_t paragraphAlignment = 0;
    uint8_t embeddedStyle = 0;
    uint8_t hyphenationEnabled = 0;
    uint8_t focusReadingEnabled = 0;
    uint8_t imageRendering = 0;
    uint8_t orientation = 0;
    bool operator==(const ReaderPrefs&) const = default;
  };
  ReaderPrefs globalPrefsSnapshot;
  static ReaderPrefs capturePrefsFromSettings();
  static void applyPrefsToSettings(const ReaderPrefs& prefs);
  bool loadBookPrefs(ReaderPrefs& out) const;
  void saveBookPrefs(const ReaderPrefs& prefs) const;
  std::optional<uint32_t> cachedVisibleTextOffset;
  // Visible-codepoint offset of the page currently on screen, captured when the page is loaded
  // (Page::visibleTextOffset). Lets saveProgress persist the offset without reopening section.bin.
  std::optional<uint32_t> currentPageVisibleOffset;
  // Explicit "land at this visible-codepoint offset in the target spine" request (bookmark open).
  // Resolved in render() once the section is loaded/built far enough, then cleared. Unlike a
  // settings-change reposition it always resolves by content, so it survives any re-pagination.
  std::optional<uint32_t> pendingOffsetJump;
  unsigned long lastPageTurnTime = 0UL;
  unsigned long pageTurnDuration = 0UL;
  // Signals that the next render should reposition within the newly loaded section
  // based on a cross-book percentage jump.
  bool pendingPercentJump = false;
  // Normalized 0.0-1.0 progress within the target spine item, computed from book percentage.
  float pendingSpineProgress = 0.0f;
  bool pendingScreenshot = false;
  bool pendingSyncSaveError = false;
  // Consecutive page-load failures. Each failure drops the section and rebuilds on the next render,
  // which recovers a transiently corrupt cache; capped so a persistently bad page can't spin forever.
  uint8_t pageLoadRetryCount = 0;
  static constexpr uint8_t MAX_PAGE_LOAD_RETRIES = 3;
  bool skipNextButtonCheck = false;  // Skip button processing for one frame after subactivity exit
  bool automaticPageTurnActive = false;
  bool showBookmarkMessage = false;
  // "No dictionary set" popup, shown when a lookup is triggered without a configured dictionary.
  bool showDictionaryMessage = false;
  unsigned long dictionaryMessageTime = 0UL;
  bool ignoreNextConfirmRelease = false;
  bool currentPageBookmarked = false;
  // Idle-time glyph prewarm: after a page settles, scan the LIKELY next page
  // (scan mode draws nothing) and load its missing glyphs from SD during idle,
  // so the next turn's in-render prewarm is a cache hit instead of ~100 ms of
  // SD reads on the page-turn critical path. One attempt per position.
  int idlePrewarmSpine = -1;
  int idlePrewarmPage = -1;
  unsigned long lastRenderCompleteMs = 0;
  bool bookmarkRemoved = false;  // true when last toggle removed (controls popup text)
  std::vector<BookmarkEntry> cachedBookmarks;
  // Tracks whether this book is currently removed from Recent Books by the
  // removeReadBooksFromRecents feature (set at End-of-Book, cleared if paged back in).
  bool recentsEntryRemoved = false;
  // Per-book vertical text override: -1 = auto (detect from language), 0 = off, 1 = on
  int8_t verticalOverride = -1;
  // Per-book furigana override: -1 = auto (on by default), 0 = off, 1 = on
  int8_t furiganaOverride = -1;
  unsigned long bookmarkMessageTime = 0UL;
  unsigned long readingSessionStartMs = 0UL;
  // Set when the reader is left at end-of-book and SETTINGS.moveFinishedToReadFolder is on.
  // Consumed in onExit() to relocate the finished book into /Read/.
  bool pendingReadFolderMove = false;
  // Next-book suggestion menu for the End-of-Book screen
  EndOfBookOptions endOfBookOptions;

  // Footnote support
  std::vector<FootnoteEntry> currentPageFootnotes;
  // Chapter-wide footnote list from the section file's footnote table (v32+): the panel shows
  // ALL of the chapter's notes, opening at the one nearest the current page.
  std::vector<std::pair<uint16_t, FootnoteEntry>> sectionFootnotes;
  // Flattened entries handed to the footnote panel (must outlive the activity, which keeps a
  // reference); rebuilt on each open.
  std::vector<FootnoteEntry> footnotePanelEntries;
  struct SavedPosition {
    int spineIndex;
    int pageNumber;
  };
  static constexpr int MAX_FOOTNOTE_DEPTH = 3;
  SavedPosition savedPositions[MAX_FOOTNOTE_DEPTH] = {};
  int footnoteDepth = 0;

  // --- Background image-cache warm (render-task tail) ---
  // After a page is fully displayed, the render task warms the NEXT page's image .pxc pixel
  // cache in place (ImageBlock::warmCache, cacheOnly decode) so landing on a full-page
  // illustration is a cache read instead of a multi-second decode. No extra task: the warm
  // runs at the tail of render(), still holding the RenderLock, and gets out of the way via
  // cooperative cancellation with two signals polled per decode block:
  //   1. imageWarmInputStamp_ -- bumped by the loop task on ANY button press (and in
  //      pageTurn() for tilt/auto turns) BEFORE any handler can push/pop an activity or take
  //      the RenderLock, so those blocking acquires only ever wait one decode block.
  //   2. the render task's own pending task-notification value -- a queued render (page turn
  //      already requested, requestUpdateAndWait from another task) cancels the warm even
  //      when no new button press is involved.
  std::atomic<uint32_t> imageWarmInputStamp_{0};
  uint32_t imageWarmStampSnapshot_ = 0;  // render task only: stamp value at warm start
  std::string imageWarmFailedPath_;      // render task only: give-up-once decode-failure target
  static constexpr uint32_t NO_IMAGE_REFINE = UINT32_MAX;
  std::atomic<uint32_t> pendingHorizontalImageRefine_{NO_IMAGE_REFINE};
  std::atomic<uint32_t> requestedHorizontalImageRefine_{NO_IMAGE_REFINE};
  void warmNextPageImageCache(uint16_t viewportWidth, uint16_t viewportHeight);
  static bool imageWarmShouldCancel(const void* ctx);
  // Viewport of the last render(), captured so loop()'s lazy partial-extension start
  // builds with IDENTICAL layout parameters to the pages already rendered (a mismatch
  // would paginate differently than the partial being extended). 0 = no render yet.
  uint16_t buildViewportWidth = 0;
  uint16_t buildViewportHeight = 0;
  // Set when the lazy extension start failed, so loop() doesn't retry (and log) every
  // tick; the blocking extension in render() remains the fallback past the watermark.
  bool partialRebuildStartFailed = false;

  // Last position persisted by render()'s saveProgress, used to skip redundant
  // writeAtomic calls on no-op re-renders (menu/bookmark/screenshot).
  int lastSavedSpineIndex = -1;
  int lastSavedPage = -1;
  int lastSavedPageCount = -1;

  void renderContents(std::unique_ptr<Page> page, int orientedMarginTop, int orientedMarginRight,
                      int orientedMarginBottom, int orientedMarginLeft, bool glyphsAlreadyWarm = false,
                      bool grayscaleRefineOnly = false);
  // Horizontal analog of prewarmedVPage_: the page index whose glyphs currently sit warm in
  // the font cache from the idle next-page prewarm; -1 = cold/unknown. Written on the render
  // task only.
  int prewarmedHPage_ = -1;
  void renderStatusBar() const;
  // Bulk-loads a vertical page's glyphs into the SD-font mini cache (heap-gated). Returns
  // false when the heap was too tight to prewarm -- rendering still works via the slower
  // per-glyph on-demand path.
  bool prewarmVerticalPageGlyphs(const VerticalPage& vpage);
  // True only while a mid-build early render is running (the build is paused mid-layout and
  // resumes the moment it returns). Selects the conservative prewarm heap floor there, and the
  // reading floor everywhere else -- see prewarmVerticalPageGlyphs().
  bool duringEarlyBuildRender_ = false;
  // Draws one vertical TEXT page into the framebuffer; shared by the normal render path and
  // the early-first-render hook. Does not touch the display. glyphsAlreadyWarm skips the
  // prewarm when the page's glyphs were pre-loaded during idle (see prewarmedVPage_).
  void renderVerticalPageBody(const VerticalPage& vpage, bool glyphsAlreadyWarm = false);
  // Page index whose glyphs currently sit in the SD-font mini cache from the idle next-page
  // warm; -1 = cache cold/unknown. Kindle-class turns: the NEXT page's glyphs are loaded
  // while the reader looks at the current one, so a forward turn renders warm (~200ms)
  // instead of paying the ~500-700ms per-page SD bulk load at button time.
  int prewarmedVPage_ = -1;
  // Evidence for the pre-render font release (see RESUME_HEAP_FLOOR in render()). The release
  // costs a full font-cache rebuild -- kern classes, mini kern, advance table, glyph groups --
  // on the following render. It was written for the resume-into-book path, on the assumption
  // that normal reading stays above the floor; a vertical book sits at maxAlloc 12-32K, so it
  // tripped on 6 of 9 measured page turns and taxed each one ~400ms. These two track whether
  // the previous render actually ran short of glyph memory, which is direct evidence that the
  // heap at this level is inadequate -- a clean render is evidence that it is not.
  uint32_t starvedGlyphsAtLastRender_ = 0;
  bool forceFontReleaseCheck_ = true;  // armed on entry: the resume path this was written for
  // Backoff for the silent next-chapter index when the heap is too tight to build clean:
  // without it the attempt re-fires every tick while the reader sits on a chapter's last two
  // pages, and each attempt releases the font caches (cold glyphs on the next turn) for
  // nothing -- observed on device firing at 1Hz+ with maxAlloc pinned at the same value.
  uint32_t silentIndexBackoffUntilMs_ = 0;
  // Page index the last vertical render actually drew. The idle warm evicts it (the mini font
  // cache holds exactly one page), so running that warm again after a re-render of the SAME
  // page -- status bar tick, closed menu, bookmark toast -- costs two bulk SD loads and buys
  // nothing. Device log: "idle warm page=20" twice around one re-render of page 19.
  int lastRenderedVPage_ = -1;
  // Direction of the most recent page turn; the idle warm follows it (forward turns warm
  // the next page, backward turns the previous one) so sustained paging in EITHER
  // direction hits a warm cache. Written by pageTurn() on the loop() task.
  std::atomic<bool> lastTurnForward_{true};
  // Early-first-render: invoked mid-build by VerticalSection the moment the reader's target
  // page is laid out (and again for every mid-build page-turn request), so text is on screen
  // seconds into a ~17s whole-chapter build and the user can keep turning pages while the
  // rest of the chapter builds.
  static void earlyRenderVerticalPageThunk(void* ctx, const VerticalPage& page, int pageIndex);
  void earlyRenderVerticalPage(const VerticalPage& page, int pageIndex);
  // True while a vertical chapter build runs on the render task. Read by pageTurn() on the
  // loop() task: while building, the section's pageCount is still 0, so the normal turn path
  // would misread every press as "past the last page" and jump to the next spine (observed:
  // a press during the build teleported the reader to the end of the book).
  std::atomic<bool> verticalBuildInProgress_{false};
  // Early target/currently shown page; seeded before the hook so build-time turns work.
  // Written on the render task, read by pageTurn() on the loop() task.
  std::atomic<int> earlyDisplayedPage_{-1};
  bool earlyPageActuallyDisplayed_ = false;
  void silentIndexNextChapterIfNeeded(uint16_t viewportWidth, uint16_t viewportHeight);
  bool saveProgress(int spineIndex, int currentPage, int pageCount, int8_t vertOverride, int8_t furiOverride);
  // Pages laid out per incremental-build pump: on the render path (catching up to the page
  // being shown) and per loop() tick (background build of a large chapter). Kept small so a
  // background build chunk never noticeably delays input or a pending render.
  static constexpr int BUILD_PAGES_PER_CHUNK = 8;
  static constexpr int BACKGROUND_BUILD_PAGES_PER_TICK = 2;

  // MEMFIX-PORT: background-build heap floor; portable
  // Skip background build ticks below this free-heap floor. The parse path grows
  // word vectors of heap strings — throwing allocations that abort() on OOM under
  // -fno-exceptions (field crash: bad_alloc in ParsedText::addWord during a
  // background tick under heap pressure). The tick is deferrable work:
  // page-turn transients free up between turns and the build resumes; the render
  // path still builds the page it actually needs regardless of this floor.
  static constexpr size_t BACKGROUND_BUILD_MIN_FREE_HEAP = 32 * 1024;
  // Fragmentation floor for the same gate: a tick passed the free-heap floor at
  // 34.7 KB free but the largest block was ~11 KB, and a parse allocation inside the
  // tick aborted anyway. Free heap says how much memory exists; maxAlloc says whether
  // any single allocation can actually have it. 16 KB also keeps the advance-table
  // batch path (16 KB scratch) viable during builds.
  static constexpr size_t BACKGROUND_BUILD_MIN_MAX_ALLOC = 16 * 1024;
  // Gate for a background build tick: true when the heap can take parse allocations.
  // Updates buildHeapPaused as a side effect.
  bool buildTickHeapGate();
  // True while the background build is gated on the heap floors. Lets skipLoopDelay()
  // return the loop to normal delay/power-saving during the pause: isBuilding() stays
  // true the whole time, and without this the loop would spin at full CPU speed doing
  // no build work — indefinitely, if the build context itself keeps the heap low.
  bool buildHeapPaused = false;
  // Heap floor for optional render-adjacent work (idle prewarm). Page
  // deserialization (TextBlock word vectors/strings) and glyph caching allocate
  // through throwing paths that abort() on OOM; skip deferrable work below it.
  static constexpr size_t RENDER_MIN_FREE_HEAP = 24 * 1024;
  // How many pages to keep laid out ahead of the reader for a still-building section. A page
  // turn is ~1s on e-ink and a page builds in ~30ms, so the reader can't out-click the builder
  // -- a tiny buffer is enough. The background build stops once the watermark is this far
  // ahead and resumes as the reader advances; building unbounded instead locked up input by
  // monopolizing the RenderLock. A giant single-spine book therefore never finalizes its .bin
  // in one sitting -- instant reopen comes from Section::suspendBuild() persisting the pages
  // already laid out as a partial file on exit/sleep.
  static constexpr int BUILD_WINDOW_AHEAD = 5;
  // Reopening a partial does NOT immediately restart its extension build (a whole-chapter
  // re-layout from page 0 -- minutes of background CPU + SD writes on a giant spine, wasted
  // when the reader never crosses the watermark that session). Instead loop() starts it once
  // the reader is within this many pages of the watermark: at ~30s per page read and ~100-300ms
  // per page rebuilt, this margin gives the rebuild ample runway to catch up (and finalize)
  // before the reader arrives.
  static constexpr int PARTIAL_REBUILD_START_MARGIN = BUILD_WINDOW_AHEAD;
  // Show the indexing popup when an initial build must lay out more than this many pages up front
  // (a deep resume/jump into a not-yet-built section), so it isn't a silent wait. Kept independent
  // of the small look-ahead window so ordinary landings stay popup-free.
  static constexpr int BUILD_POPUP_PAGE_THRESHOLD = 20;
  // Also show the popup when first building a spine larger than this (uncompressed bytes): its
  // whole HTML must be inflated before page 1 can lay out (the giant single-spine case), which is
  // a multi-second wait. Normal chapters are well under this and stay popup-free.
  static constexpr size_t BUILD_POPUP_BYTE_THRESHOLD = 96 * 1024;
  // Deadline backstop for the predictive gates above: if the blocking build-to-target still
  // hasn't produced the landing page this long after the build started, surface the popup
  // mid-build. Builds that finish under the deadline stay popup-free.
  static constexpr unsigned long BUILD_POPUP_DEADLINE_MS = 1000;
  // True only during onEnter's blocking build-to-target phase, until the popup has been
  // drawn. Gates showBuildPopup() so the parser's popup callback (which persists into
  // background buildSomeMore chunks) can never draw over a displayed page.
  bool buildPopupPending = false;
  // Draw the indexing popup mid-build (parser image-probe callback and deadline backstop).
  void showBuildPopup();
  // Map the cached content position into the rebuilt section (used after a
  // settings change re-paginates a chapter). Returns true if currentPage moved.
  // No-op while the section is still building or when the pagination is unchanged (plain resume).
  bool applyDeferredReposition();
  void rememberCurrentContentOffset();
  // Jump to a percentage of the book (0-100), mapping it to spine and page.
  void jumpToPercent(int percent);
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  // Opens the reader menu for the current position (short-press Confirm)
  void openReaderMenu();
  void openDictionaryWordSelect();
  // Returns true if sync acted (launched, or surfaced a save error); false if it was a no-op
  // because no KOReader credentials are stored.
  bool launchKOReaderSync();
  void applyOrientation(uint8_t orientation);
  void toggleAutoPageTurn(uint8_t selectedPageTurnOption);
  void pageTurn(bool isForwardTurn);
  void loadCachedBookmarks();
  void addBookmark();
  void updateBookmarkFlag();

  // Footnote navigation
  void navigateToHref(const std::string& href, bool savePosition = false);
  void openFootnotesPanel();
  void openWordLookupPanel();
  static constexpr uint16_t kSpineProbeFailed = 0xFFFF;  // session marker: cache probe failed, don't retry
  // Page numbering across the logical ToC chapter: spine files without their own ToC entry
  // (inline illustration files etc.) inherit the previous entry's tocIndex, so the "page X/Y"
  // counter runs to the next REAL chapter instead of resetting at every spine-file boundary.
  // Sibling counts come from a cheap header-only cache peek; unbuilt siblings are estimated
  // from byte size. Cached per (spine, live page count, mode); mutable so the const render
  // path can refresh it.
  void updateChapterPageSpan(uint16_t viewportWidth, uint16_t viewportHeight) const;
  // Page-based book progress (pages read / total pages, like Apple Books) instead of the
  // byte-weighted estimate: furigana markup inflates ruby-dense chapters' byte share, so the
  // byte model lags several percent behind the rendered-page position on Japanese books.
  // Real counts come from section-cache headers; unindexed chapters are estimated from their
  // byte share of the already-indexed ones and refine as sections get built.
  int pageBasedPercent(int spineIndex, int sectionPage) const;  // sectionPage is 1-based
  mutable int chapterSpanSpine = -1;
  mutable int chapterSpanLivePages = -1;
  mutable bool chapterSpanVertical = false;
  mutable int chapterPagesBefore = 0;
  mutable int chapterPagesTotal = 0;
  mutable std::vector<uint16_t> spinePagesReal;       // 0 = not indexed yet
  mutable std::vector<uint16_t> spinePagesEffective;  // real or byte-estimated, never 0
  mutable int bookPagesBefore = 0;
  mutable int bookPagesTotal = 0;
  mutable uint16_t lastViewportWidth = 0;
  mutable uint16_t lastViewportHeight = 0;
  // The font the book is actually laid out and rendered in. Normally the user's selection;
  // when that font can't carry the book's PRIMARY script (built-in or Latin font with a
  // Japanese book, CJK-only font with a Latin book), the loaded companion font substitutes so
  // measurement and the vertical engine's font-adaptive positioning all derive from one font
  // that really contains the glyphs -- per-glyph fallback stays only for rare stragglers.
  int effectiveReaderFontId() const;
  void restoreSavedPosition();
  bool useVerticalText() const;
  // Space kept clear below the text, in addition to the panel's own bezel.
  //
  // Horizontal follows upstream: the status bar and the reader's margin describe the same strip,
  // so the larger of the two wins. Vertical ADDS them, because tategaki puts ink past its last
  // row by design -- the row's own ink hang, and 。/、 set past the line end (burasage). Measured
  // at 6px on a 29px cell, against a default margin that max() collapses to nothing whenever the
  // status bar is taller (~24px with a clock), which would drop that ink onto the clock.
  uint8_t readerBottomReserve(bool verticalMode) const;

  bool useFurigana() const;
  bool isJapaneseBook() const;
  bool showVerticalToggle() const;
  void applyVerticalFuriganaOverride(int8_t verticalOverrideIn, int8_t furiganaOverrideIn);

 public:
  explicit EpubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Epub> epub,
                              int initialRefreshCountdown)
      : Activity("EpubReader", renderer, mappedInput),
        epub(std::move(epub)),
        pagesUntilFullRefresh(initialRefreshCountdown) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  // Full CPU speed + fast loop ticks while a section build runs: at the low-power
  // frequency a giant chapter's background rebuild stretches from ~40s to many
  // minutes, so the reader exits before it can finalize and the next open restarts
  // it from page 0. Reverts to normal power behavior the moment the build finishes,
  // and while the build is heap-paused (no work is happening, so spinning at full
  // speed would only burn battery; the paused gate still retries every loop pass).
  bool skipLoopDelay() override { return section && section->isBuilding() && !buildHeapPaused; }
  bool isReaderActivity() const override { return true; }
  bool handleForcedRefresh() override {
    {
      RenderLock lock(*this);
      pagesUntilFullRefresh = 1;
      forcedRefreshPending = true;
    }
    requestUpdate();
    return true;
  }
  ScreenshotInfo getScreenshotInfo() const override;
  CrossPointPosition getCurrentPosition() const;
};
