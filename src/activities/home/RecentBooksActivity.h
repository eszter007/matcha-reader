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

  void promptRemoveBook(const std::string& path, const std::string& title);

 public:
  explicit RecentBooksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("RecentBooks", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
