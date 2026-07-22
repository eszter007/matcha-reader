#pragma once
#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/Section.h>
#include <Epub/VerticalSection.h>

#include <atomic>
#include <optional>

#include "BookmarkEntry.h"
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
    bool operator==(const LayoutSig& o) const {
      return fontId == o.fontId && viewportWidth == o.viewportWidth && viewportHeight == o.viewportHeight &&
             lineCompression == o.lineCompression && paragraphAlignment == o.paragraphAlignment &&
             extraParagraphSpacing == o.extraParagraphSpacing && hyphenationEnabled == o.hyphenationEnabled &&
             embeddedStyle == o.embeddedStyle && imageRendering == o.imageRendering &&
             focusReadingEnabled == o.focusReadingEnabled && bookCssMargins == o.bookCssMargins;
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
    uint8_t fontSize = 0;
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
  unsigned long lastPageTurnTime = 0UL;
  unsigned long pageTurnDuration = 0UL;
  // Signals that the next render should reposition within the newly loaded section
  // based on a cross-book percentage jump.
  bool pendingPercentJump = false;
  // Normalized 0.0-1.0 progress within the target spine item, computed from book percentage.
  float pendingSpineProgress = 0.0f;
  bool pendingScreenshot = false;
  bool pendingSyncSaveError = false;
  bool skipNextButtonCheck = false;  // Skip button processing for one frame after subactivity exit
  bool automaticPageTurnActive = false;
  bool showBookmarkMessage = false;
  bool ignoreNextConfirmRelease = false;
  bool currentPageBookmarked = false;
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
  void warmNextPageImageCache(uint16_t viewportWidth, uint16_t viewportHeight);
  static bool imageWarmShouldCancel(const void* ctx);

  void renderContents(std::unique_ptr<Page> page, int orientedMarginTop, int orientedMarginRight,
                      int orientedMarginBottom, int orientedMarginLeft);
  void renderStatusBar() const;
  void silentIndexNextChapterIfNeeded(uint16_t viewportWidth, uint16_t viewportHeight);
  bool saveProgress(int spineIndex, int currentPage, int pageCount, int8_t vertOverride, int8_t furiOverride);
  // Jump to a percentage of the book (0-100), mapping it to spine and page.
  void jumpToPercent(int percent);
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
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
  void openReaderMenu();
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
  bool useFurigana() const;
  bool isJapaneseBook() const;

 public:
  explicit EpubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Epub> epub)
      : Activity("EpubReader", renderer, mappedInput), epub(std::move(epub)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  bool isReaderActivity() const override { return true; }
  ScreenshotInfo getScreenshotInfo() const override;
  CrossPointPosition getCurrentPosition() const;
};
