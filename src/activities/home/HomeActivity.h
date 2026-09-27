#pragma once
#include <functional>
#include <vector>

#include "./FileBrowserActivity.h"
#include "RecentBook.h"
#include "RecentBooksStore.h"
#include "activities/Activity.h"
#include "components/CoverGridHomeUi.h"
#include "components/CoverWorker.h"
#include "components/HomeTabBar.h"
#include "components/OptionPopup.h"
#include "util/ButtonNavigator.h"

struct Rect;

class HomeActivity final : public Activity {
  std::unique_ptr<CoverGridHomeUi> coverGridUi;
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  // True once the cover scan has walked every recent book. Reset when the slot height changes.
  bool recentsLoaded = false;
  // Set after the cover grid's first paint: that pass uses a clean HALF refresh, later ones FAST.
  bool firstRenderDone = false;
  // Partial-redraw state: after a full render, a cursor move between two MENU rows only erases
  // and redraws the menu block over the intact frame, skipping the header, cover tile, and
  // button hints (the bulk of a render). Any other update (recents load completion, battery
  // tick) arrives with selectorIndex unchanged and falls through to the full render naturally.
  bool lastRenderValid = false;
  int lastSelectorIndex = 0;
  bool hasOpdsServers = false;
  bool hasContinueReading = false;
  bool coverRendered = false;      // Track if cover has been rendered once
  bool coverBufferStored = false;  // Track if cover buffer is stored
  uint8_t* coverBuffer = nullptr;  // HomeActivity's own buffer for cover image
  size_t coverBufferSize = 0;      // Bytes allocated to coverBuffer
  // Logical rect last passed to drawRecentBookCover. The cover snapshot only
  // needs to cover this region, not the entire framebuffer, so we cache the
  // tile instead of all 48 KB. Set in render() before the call.
  int coverRectX = 0;
  int coverRectY = 0;
  int coverRectW = 0;
  int coverRectH = 0;
  std::vector<RecentBook> recentBooks;
  // Long-press menu on a cover (stats / read / unread / delete), shared with the Library grid.
  OptionPopup optionPopup;
  int currentBookProgress = -1;
  const HomeMenuItem initialMenuItem;
  const bool cleanInitialRefresh;

  // Convert HomeMenuItem to menu index (used in onEnter)
  static int menuItemToIndex(HomeMenuItem item, bool hasOpdsUrl) {
    int i = 0;
    if (item == HomeMenuItem::LIBRARY) return i;
    ++i;
    if (item == HomeMenuItem::FILE_BROWSER) return i;
    ++i;
    if (item == HomeMenuItem::OPDS_BROWSER) return hasOpdsUrl ? i : 0;
    if (hasOpdsUrl) ++i;
    if (item == HomeMenuItem::FILE_TRANSFER) return i;
    ++i;
    if (item == HomeMenuItem::READING_STATS) return i;
    ++i;
    if (item == HomeMenuItem::SETTINGS_MENU) return i;
    return 0;
  }

  // The cover grid's band is the tab bar, whose order is its own (Home first, no OPDS). Reusing
  // menuItemToIndex there put the cursor on the wrong slot -- coming home from Settings lit up
  // Stats, which shares index 4 with Settings in the classic menu.
  static int tabIndexFor(HomeMenuItem item) {
    switch (item) {
      case HomeMenuItem::FILE_BROWSER:
        // The browser lives inside the Library here, so its bottom-bar home is the Library tab.
        return static_cast<int>(HomeTab::Library);
      case HomeMenuItem::LIBRARY:
        return static_cast<int>(HomeTab::Library);
      case HomeMenuItem::FILE_TRANSFER:
        return static_cast<int>(HomeTab::Transfer);
      case HomeMenuItem::READING_STATS:
        return static_cast<int>(HomeTab::Stats);
      case HomeMenuItem::SETTINGS_MENU:
        return static_cast<int>(HomeTab::Settings);
      default:
        // OPDS has no tab of its own; land on Home rather than on whatever shares its index.
        return static_cast<int>(HomeTab::Home);
    }
  }

  // Convert menu index to HomeMenuItem (used in loop)
  static HomeMenuItem indexToMenuItem(int idx, bool hasOpdsUrl) {
    int i = 0;
    if (idx == i++) return HomeMenuItem::LIBRARY;
    if (idx == i++) return HomeMenuItem::FILE_BROWSER;
    if (hasOpdsUrl && idx == i++) return HomeMenuItem::OPDS_BROWSER;
    if (idx == i++) return HomeMenuItem::FILE_TRANSFER;
    if (idx == i++) return HomeMenuItem::READING_STATS;
    if (idx == i) return HomeMenuItem::SETTINGS_MENU;
    return HomeMenuItem::NONE;
  }
  void onSelectBook(const std::string& path);
  void onFileBrowserOpen();
  void onLibraryOpen();
  void onSettingsOpen();
  void onFileTransferOpen();
  void onStatsOpen();
  void onOpdsBrowserOpen();

  int getMenuItemCount() const;
  bool storeCoverBuffer();    // Store frame buffer for cover image
  bool restoreCoverBuffer();  // Restore frame buffer from stored cover
  void freeCoverBuffer();     // Free the stored cover buffer
  void loadRecentBooks(int maxBooks);
  // Cover thumbnails off the loop task, the same worker the Library uses.
  CoverWorker coverWorker_;
  // How far the cover scan has walked recentBooks. Advances on a completed job; a cancelled one
  // leaves it in place so the book is retried.
  int coverScanIndex_ = 0;
  void postNextCoverJob(int coverHeight);
  void applyCoverResult();
  // The cover height the cards actually ask for. 0 before the grid has measured a slot.
  int coverTargetHeight() const;
  // Full CPU while a conversion runs: Home sits idle meanwhile, so the loop would drop to
  // LOW_POWER_FREQ and a thumbnail that takes ~1.5s at 160MHz would take ~24s at 10. The work is
  // fixed, so finishing sooner spends less time awake, not more.
  bool skipLoopDelay() override { return coverWorker_.busy(); }
  void fillCoverGridFromLibrary();
  void resolveGridCoverPaths();

 public:
  explicit HomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                        HomeMenuItem initialMenuItemValue = HomeMenuItem::NONE, bool cleanInitialRefresh = false)
      : Activity("Home", renderer, mappedInput),
        initialMenuItem(initialMenuItemValue),
        cleanInitialRefresh(cleanInitialRefresh) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isHomeActivity() const override { return true; }
};
