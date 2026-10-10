#pragma once
#include <HalStorage.h>
#include <I18n.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <array>
#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "activities/Activity.h"
#include "components/CoverWorker.h"
#include "components/OptionPopup.h"
#include "components/TabRing.h"
#include "components/UITheme.h"  // TabInfo, Rect
#include "components/UiTabBand.h"
#include "util/ButtonNavigator.h"

class CoverLibraryActivity final : public Activity, public TabRing::Host {
 private:
  ButtonNavigator buttonNavigator;
  // The Library's tab band, the same FreeInkUI band the list screens build.
  UiTabBand::Host tabBand_;
  // Long-press menu on a cover (stats / read / unread / delete), shared with the Home grid.
  OptionPopup optionPopup;
  // Bottom tab bar cursor for button boards; -1 when nothing in the band is focused.
  int tabFocus = -1;

  int selectedTab = 0;
  // Tab to open on, from the tab band of whichever screen switched here.
  int requestedTab = 0;
  int contentIndex = 0;
  // contentIndex and tabFocus when the current Next/Previous press began; the index is -1 once a
  // hold has restored them.
  int holdStartContentIndex_ = -1;
  int holdStartTabFocus_ = -1;
  // A Confirm release acts on the tab band only when its press began on this screen: the release
  // of the click that opened the Library must not step a tab.
  bool confirmPressSeen_ = false;

  // TabRing::Host. The top tabs are the Library's (Books, Shelves, and in Cover Grid OPDS and
  // Files, which are other screens); an open shelf has no tabs, only its grid and the bar.
  int ringTopTabCount() const override;
  int ringActiveTopTab() const override { return selectedTab; }
  void ringSelectTopTab(int index) override;
  HomeTab ringBottomTab() const override { return HomeTab::Library; }
  // As on every list with tabs (UiTabListActivity): Left/Right step the tabs only beside the
  // bottom bar; elsewhere they are Previous/Next, as the hints say, and walk on into the covers.
  bool ringTopTabsLeftRightStep() const override { return TabRing::hasBottomBar(*this); }
  bool ringHasContent() const override;
  TabRing::Focus ringFocus() const override;
  void ringSetFocus(TabRing::Focus focus, bool atEnd) override;
  int& ringBarSlot() override { return tabFocus; }
  void ringChanged() override { requestUpdate(); }
  int scrollRow = 0;      // Books tab: first visible grid row
  int shelvesScroll = 0;  // Shelves tab: first visible list row

  bool longPressFired = false;
  // A shelf opens on the Confirm PRESS, so the release of that same physical click arrives with
  // the shelf's book list already on screen -- where it read as "open the focused book", opening
  // one the moment a shelf was entered. Cleared on entry and set by a FRESH press, so only a
  // click that both started and ended inside the shelf view can act. Same guard as
  // EpubReaderWordLookupActivity::confirmPressSeen, which is entered mid-press the same way.
  bool shelfConfirmPressSeen = false;

  // Books tab
  std::vector<RecentBook> recentBooks;

  struct BookProgress {
    int percent = -1;
  };
  std::vector<BookProgress> bookProgress;

  // Shelves tab
  struct ShelfInfo {
    std::string folderPath;
    std::string folderName;
    std::string coverBmpPath;
    // Resolved path to a small (shelf-height) thumbnail that renders 1:1.
    std::string shelfThumbPath;
    int bookCount = 0;
  };
  std::vector<ShelfInfo> shelves;
  bool shelvesLoaded = false;

  // Shelf detail view
  struct ShelfBook {
    std::string path;
    std::string title;
    std::string coverBmpPath;
  };
  std::vector<ShelfBook> shelfBooks;
  std::vector<BookProgress> shelfBookProgress;
  int openShelfIndex = -1;
  int shelfContentIndex = 0;
  int shelfScrollRow = 0;

  static constexpr int TAB_COUNT = 2;
  static constexpr int GRID_COLS = 3;
  static constexpr int GRID_ROW_GAP = 16;
  static constexpr int COVER_PADDING = 4;
  static constexpr int CELL_TEXT_GAP = 4;
  static constexpr int SELECTION_RADIUS = 6;

  int getVisibleRows(int cellHeight, int contentHeight) const;
  int getCellHeight(int cellWidth) const;
  // The grid's viewport height, below the tab bar and above the button hints.
  [[nodiscard]] int gridContentHeight() const;
  // Highest scrollRow that still fills the viewport, for the swipe that scrolls it.
  // itemCount defaults to the tabbed view's; the shelf detail view passes its own,
  // since it scrolls its own grid of shelfBooks rather than the Library's.
  [[nodiscard]] int maxScrollRow(int contentHeight, int itemCount = -1) const;
  // Absolute grid item index under a screen point, or -1 for a miss. Derives the cell grid the
  // same way renderBooksTab()/renderShelvesTab()/renderShelfBooksView() do, so the hit targets
  // are exactly the drawn cells. Deliberately stops at visibleRows: those renderers draw one
  // EXTRA "peek" row as a more-below hint, and it sits half-behind the button hints -- letting a
  // tap land there would open a book the reader cannot actually see. contentTop differs between
  // the tabbed views and the shelf detail view (no tab bar), so the caller passes it in.
  int gridIndexAtPoint(int x, int y, int contentTop, int contentHeight, int scrollRowIn, int itemCount) const;
  // Cover height of one grid cell, published by the render task and read by the scan task.
  // The scan also derives the same geometry before the first render, so it never generates a
  // theme-sized thumb that the grid cannot draw.
  std::atomic<int> gridCoverHeight_{0};

  void loadRecentBooks();
  void loadBookProgress();
  // A pointer-style selector belongs to key navigation. Touch users act on what they
  // touch, so a highlight sitting on some other cover is just noise -- and worse, it
  // implies the swipe moved it. Set by the nav keys, cleared by any touch.
  bool selectorVisible = false;
  // Clearing the flag is not enough: the frame still showing the selector has to be replaced,
  // and a touch that changes nothing else (a swipe against the end stop, a tap on the current
  // cover, a tap on the active tab) requests no redraw of its own.
  // The content highlight is drawn only while the content holds the cursor: with it in the
  // bottom bar, a second highlight on the last cover would read as two selections.
  bool contentCursorShown() const { return selectorVisible && tabFocus < 0; }
  void hideSelector() {
    if (!selectorVisible) return;
    selectorVisible = false;
    requestUpdate();
  }
  // Where the tab band sits, so a touch on it never falls through to the grid.
  [[nodiscard]] Rect tabBarRect() const;
  // Same idea for the Shelves list, which is rows rather than the cover grid: the renderer and
  // the hit test below share this geometry instead of each deriving its own.
  [[nodiscard]] int shelvesVisibleItems(int contentHeight) const;
  [[nodiscard]] int shelvesScrollOffset(int visibleItems) const;
  // Shelf index under a screen point in the Shelves list, or -1 for a miss.
  [[nodiscard]] int shelfRowAtPoint(int x, int y, int contentTop, int contentHeight) const;
  void loadShelves();
  void loadShelfBooks(const std::string& folderPath);
  int readProgressPercent(const std::string& bookPath) const;
  void fillPageProgressNow(std::vector<BookProgress>& progress, const std::vector<RecentBook>* books,
                           const std::vector<ShelfBook>* sBooks, int firstIdx, int lastIdx);

  int getContentItemCount() const;
  void renderBooksTab(int contentTop, int contentHeight);
  void renderShelvesTab(int contentTop, int contentHeight);
  void renderShelfBooksView(int contentTop, int contentHeight);

  // Shared cell/row painters, used by both the full renders above and the partial fast path.
  void drawGridCell(int cellX, int cellY, int cellWidth, int cellHeight, const std::string& coverBmpPath,
                    const std::string& title, int progressPercent, bool selected, bool drawTitle = true);
  void drawShelfRow(int shelfIdx, int itemY, bool selected);

  // Grid selection indicator: a 2px border ring just OUTSIDE the cover box, entirely within the
  // cell's padding margin. Because it never overlaps the cover, moving the selection is two of
  // these calls (erase old with on=false, draw new) -- no cover re-decode, a few ms total.
  void drawGridSelectionBorder(int cellX, int cellY, int cellWidth, int cellHeight, bool on);

  // Selection-only fast path: when the previous full render is still in the framebuffer and ONLY
  // the selection moved within the same scroll window, repaint the two affected cells/rows over
  // the existing frame instead of re-rendering the whole screen (covers, header, tabs). This is
  // the difference between ~585ms and tens of ms per cursor move. Returns false when a full
  // render is required (scroll, tab switch, data reload, first render).
  bool tryPartialSelectionRedraw();

  // What the framebuffer currently shows; compared by tryPartialSelectionRedraw() and refreshed
  // after every render. valid=false whenever the frame may not match this state anymore (data
  // reloads, sub-activity overlays like the delete confirmation).
  struct RenderedState {
    bool valid = false;
    int openShelf = -1;
    int tab = -1;
    int contentIndex = -1;
    int scrollRow = -1;
    int shelvesScroll = -1;
    int shelfContentIndex = -1;
    int shelfScrollRow = -1;
    int tabFocus = -1;
  };
  RenderedState lastRendered;
  // The one place that snapshots what the frame on screen shows. Both render paths call it, so a
  // new piece of view state cannot be added to the renderer and forgotten here -- which is exactly
  // how the partial redraw came to read the Shelves list at an offset it was no longer drawn at.
  void rememberRendered() {
    lastRendered.valid = true;
    lastRendered.openShelf = openShelfIndex;
    lastRendered.tab = selectedTab;
    lastRendered.contentIndex = contentIndex;
    lastRendered.scrollRow = scrollRow;
    lastRendered.shelvesScroll = shelvesScroll;
    lastRendered.shelfContentIndex = shelfContentIndex;
    lastRendered.shelfScrollRow = shelfScrollRow;
    lastRendered.tabFocus = tabFocus;
  }

  // Background library scan (stale-while-revalidate): onEnter() shows the persisted book list
  // instantly; loop() re-walks the SD card one directory entry per slice and applies/saves changes
  // when the pass completes. The full walk used to run synchronously on every Library open --
  // every folder on the card plus per-book cover repair -- which dominated the open time.
  struct LibraryScanState {
    bool active = false;
    bool walkDone = false;
    std::vector<std::string> dirStack;
    HalFile activeDir;
    std::string activeDirPath;
    std::array<char, 500> nameBuf{};
    std::vector<RecentBook> results;
    size_t thumbIndex = 0;  // cover-thumb pass cursor over the live catalog
  };
  LibraryScanState scan_;

  // Library index (/.crosspoint/library.idx): one record per book seen by a previous scan.
  // Without it every Library visit re-examined each book on the card -- a file open per EPUB
  // to check its cover thumbnail, a directory listing per manga folder to find its cover page
  // -- which is what made the background scan cost seconds per slice. A record whose size and
  // modification stamp still match means nothing about that book can have changed, so the scan
  // trusts the recorded cover state and skips the I/O. Kept deliberately small (no strings):
  // titles and cover paths already live in the persisted recents list.
  struct LibraryIndexEntry {
    uint32_t pathHash = 0;
    uint32_t fileSize = 0;       // manga folders: size of panels.idx
    uint32_t modifiedStamp = 0;  // packed FAT date/time, 0 when the driver has none
    uint16_t thumbHeight = 0;    // cover height this thumb was verified for (theme-dependent)
    uint8_t flags = 0;           // bit0: verified thumbnail present, bit1: book declares no cover
  };
  static constexpr uint8_t INDEX_FLAG_HAS_THUMB = 1 << 0;
  // The book itself declares no cover image (Epub::hasCoverImage() == false) -- a permanent fact
  // about the file, not the outcome of one conversion, so it is safe to record and skip on later
  // visits. This is what stops a coverless book being re-parsed every time the Library opens,
  // the job the 0-byte sentinel file used to do dishonestly. Cleared whenever the book's size or
  // modification stamp changes, so replacing the file re-examines it. Only set for EPUBs: Xtc
  // and MangaBook expose no equivalent predicate, so they keep retrying.
  static constexpr uint8_t INDEX_FLAG_NO_COVER = 1 << 1;
  std::vector<LibraryIndexEntry> libraryIndex_;
  bool libraryIndexDirty_ = false;

  // Cover conversion off the loop task. The mechanism lives in CoverWorker (shared with Home);
  // what stays here is the policy -- which book to convert next, the set of heights to ask for,
  // and recording the outcome in library.idx.
  CoverWorker coverWorker_;

  // Full CPU while a cover conversion runs, the same way EpubReaderActivity keeps a section
  // build off the low-power clock. The Library sits idle while the worker converts, so the loop
  // would drop to LOW_POWER_FREQ and a thumbnail that takes ~1.5s at 160MHz takes ~24s at 10 --
  // long enough that it used to be cancelled before it could finish. The work is fixed, so
  // finishing sooner spends less time awake, not more.
  bool skipLoopDelay() override { return coverWorker_.busy(); }

  void loadLibraryIndex();
  void saveLibraryIndex();
  const LibraryIndexEntry* findIndexEntry(uint32_t pathHash) const;
  void recordIndexEntry(const std::string& path, uint32_t fileSize, uint32_t modifiedStamp, int thumbHeight,
                        bool hasThumb, bool coverKnownAbsent = false);
  void prewarmBookText();
  void startLibraryScan();
  bool stepLibraryScan();  // one slice; returns true when the whole pass is done
  bool applyLibraryScan();
  void finishLibraryScan();
  uint32_t lastInputMs = 0;  // idle gate for the heavy thumb/indexing slices
  void scanDirectoryEntry();
  // Progress percentages fill progressively from loop() (PROGRESS_PENDING sentinel) instead of
  // ~5 file reads per book up front.
  static constexpr int PROGRESS_PENDING = -2;
  void markAllProgressPending();
  void warmOnePendingProgress();

  // Long-press on a book opens its reading stats.
  void showBookActions(const std::string& path, const std::string& title, int progressPercent);

 public:
  explicit CoverLibraryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const int initialTab = 0)
      : Activity("RecentBooks", renderer, mappedInput), tabBand_(renderer), requestedTab(initialTab) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
