#pragma once
#include <I18n.h>

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
    std::string coverBookPath;  // EPUB path used to generate the shelf thumb
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
  // Cancel hook for the background thumbnail generation: a cover conversion takes seconds and
  // runs on the UI task, so it polls the buttons directly (the debounced state is stale while
  // no update() tick happens) and gives way to a press. The abandoned thumb is retried on the
  // next visit. Static so it can be passed as a plain function pointer.
  static bool thumbGenShouldCancel(void* ctx);
  uint32_t thumbGenStartedMs = 0;   // start of the running conversion, for the slice budget
  uint32_t thumbGenBudgetMs = 400;  // grows when a conversion keeps hitting the budget
  // Cover height of one grid cell, recorded while drawing so the background thumb passes
  // generate at exactly that size. Drawing a theme-sized thumb (300px, 400px in Classic) into
  // a ~207px cell costs seconds per cover in software scaling -- a 1:1 draw is a few ms.
  // 0 until the first grid render; the passes fall back to the theme's cover height.
  mutable int gridCoverHeight_ = 0;

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
  // instantly; loop() re-walks the SD card one directory per slice and applies/saves changes
  // when the pass completes. The full walk used to run synchronously on every Library open --
  // every folder on the card plus per-book cover repair -- which dominated the open time.
  struct LibraryScanState {
    bool active = false;
    bool walkDone = false;
    std::vector<std::string> dirStack;
    std::vector<RecentBook> results;
    size_t thumbIndex = 0;  // EPUB/XTC cover-thumb pass cursor over results
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
    uint8_t flags = 0;           // bit0: verified thumbnail present
  };
  static constexpr uint8_t INDEX_FLAG_HAS_THUMB = 1 << 0;
  std::vector<LibraryIndexEntry> libraryIndex_;
  bool libraryIndexDirty_ = false;
  void loadLibraryIndex();
  void saveLibraryIndex();
  const LibraryIndexEntry* findIndexEntry(uint32_t pathHash) const;
  void recordIndexEntry(const std::string& path, uint32_t fileSize, uint32_t modifiedStamp, int thumbHeight,
                        bool hasThumb);
  void startLibraryScan();
  bool stepLibraryScan();  // one slice; returns true when the whole pass is done
  void applyLibraryScan();
  void finishLibraryScan();
  uint32_t lastInputMs = 0;  // idle gate for the heavy thumb/indexing slices
  void scanOneDirectory(const std::string& dirPath);
  // Progress percentages fill progressively from loop() (PROGRESS_PENDING sentinel) instead of
  // ~5 file reads per book up front.
  static constexpr int PROGRESS_PENDING = -2;
  void markAllProgressPending();
  bool fillPendingProgress(int maxCount);

  void promptRemoveBook(const std::string& path, const std::string& title);

 public:
  explicit RecentBooksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("RecentBooks", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
