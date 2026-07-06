#pragma once
#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/Section.h>
#include <Epub/VerticalSection.h>

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
