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
#include "util/ButtonNavigator.h"

class RecentBooksActivity final : public Activity {
 private:
  ButtonNavigator buttonNavigator;

  int selectedTab = 0;
  int contentIndex = 0;
  int scrollRow = 0;

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
    int shelfContentIndex = -1;
    int shelfScrollRow = -1;
  };
  RenderedState lastRendered;

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

  // One lower-priority cover job at a time. Release/acquire stores on busy publish the job to
  // the worker and its result back to loop(), so the non-atomic structs are never accessed
  // concurrently; the loop task is their only writer while busy=false.
  struct CoverJob {
    RecentBook book;
    int gridHeight = 0;
    // Every thumb height this book still needs, generated in ONE job. Opening the book is what
    // costs (measured on device: 213ms typical, 2347ms worst, against milliseconds to scale the
    // cover once it is open), so a job per height paid that price again for each size. Unused
    // slots are 0. Three: the grid cell, the home cover, and SHELF_THUMB_HEIGHT.
    static constexpr int MAX_TARGET_HEIGHTS = 3;
    int targetHeights[MAX_TARGET_HEIGHTS] = {0, 0, 0};
    uint32_t fileSize = 0;
    uint32_t modifiedStamp = 0;

    void addTargetHeight(const int h) {
      if (h <= 0) return;
      for (int& slot : targetHeights) {
        if (slot == h) return;  // already queued
        if (slot == 0) {
          slot = h;
          return;
        }
      }
    }
  };
  struct CoverResult {
    bool pending = false;
    bool completed = false;  // false means foreground work cancelled it; retry after idle
    bool hasGridThumb = false;
    // The book declares no cover image at all, so no future attempt can succeed. Distinct from
    // hasGridThumb=false, which usually means the conversion did not fit in the heap this time
    // and must be retried -- conflating the two costs a cover forever (see Epub::hasCoverImage).
    bool coverKnownAbsent = false;
    RecentBook book;
    uint32_t fileSize = 0;
    uint32_t modifiedStamp = 0;
  };
  TaskHandle_t coverWorkerTask_ = nullptr;
  std::atomic<bool> coverWorkerExitRequested_{false};
  std::atomic<bool> coverWorkerExited_{false};
  std::atomic<bool> coverWorkerBusy_{false};
  std::atomic<bool> coverWorkerCancelRequested_{false};
  std::atomic<bool> coverWorkerCancelSeen_{false};
  CoverJob coverJob_;
  CoverResult coverResult_;

  static void coverWorkerTrampoline(void* ctx);
  static bool coverWorkerShouldCancel(void* ctx);
  void coverWorkerLoop();
  void runCoverJob();
  bool postCoverJob(CoverJob&& job);
  void startCoverWorker();
  void stopCoverWorker();

  void loadLibraryIndex();
  void saveLibraryIndex();
  const LibraryIndexEntry* findIndexEntry(uint32_t pathHash) const;
  void recordIndexEntry(const std::string& path, uint32_t fileSize, uint32_t modifiedStamp, int thumbHeight,
                        bool hasThumb, bool coverKnownAbsent = false);
  // Full CPU while a cover conversion runs, the same way EpubReaderActivity keeps a section
  // build off the low-power clock. The Library sits idle while the worker converts, so the loop
  // would drop to LOW_POWER_FREQ and a thumbnail that takes ~1.5s at 160MHz takes ~24s at 10 --
  // long enough that it used to be cancelled before it could finish. The work is fixed, so
  // finishing sooner spends less time awake, not more.
  bool skipLoopDelay() override { return coverWorkerBusy_.load(std::memory_order_acquire); }

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
  void showBookStats(const std::string& path, const std::string& title);

 public:
  explicit RecentBooksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("RecentBooks", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
