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
  static constexpr int COVER_PADDING = 4;
  static constexpr int CELL_TEXT_GAP = 4;
  static constexpr int SELECTION_RADIUS = 6;

  int getVisibleRows(int cellHeight, int contentHeight) const;
  int getCellHeight(int cellWidth) const;

  void loadRecentBooks();
  void loadBookProgress();
  void loadShelves();
  void loadShelfBooks(const std::string& folderPath);
  int readProgressPercent(const std::string& bookPath) const;

  int getContentItemCount() const;
  void renderBooksTab(int contentTop, int contentHeight);
  void renderShelvesTab(int contentTop, int contentHeight);
  void renderShelfBooksView(int contentTop, int contentHeight);

  // Shared cell/row painters, used by both the full renders above and the partial fast path.
  void drawGridCell(int cellX, int cellY, int cellWidth, int cellHeight, const std::string& coverBmpPath,
                    const std::string& title, int progressPercent, bool selected);
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

  void promptRemoveBook(const std::string& path, const std::string& title);

 public:
  explicit RecentBooksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("RecentBooks", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
