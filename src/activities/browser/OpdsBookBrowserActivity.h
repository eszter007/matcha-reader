#pragma once
#include <OpdsParser.h>

#include <string>
#include <utility>
#include <vector>

#include "OpdsServerStore.h"
#include "activities/CatalogActivity.h"
#include "components/LibraryTabs.h"

/**
 * Activity for browsing and downloading books from an OPDS server.
 * Supports navigation through catalog hierarchy and downloading EPUBs.
 */
class OpdsBookBrowserActivity final : public CatalogActivity {
 public:
  explicit OpdsBookBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, OpdsServer server);

  void onEnter() override;
  void onExit() override;

 private:
  std::vector<OpdsEntry> entries;
  // Row buffer, built whenever entries changes (fetchFeed()/releaseEntries())
  // so buildBrowsingScreen() reuses it on every repaint instead of rebuilding
  // a ListItem vector per render.
  std::vector<freeink::ui::ListItem> rowItems;
  void rebuildRowItems();
  std::vector<std::string> navigationHistory;
  std::string currentPath;
  std::string searchTemplate;
  // Synthetic pager rows fetchFeed() bracketed the entries with; their footer
  // hint is Fetch (a server round-trip), not Open.
  bool prevRowPresent = false;
  bool nextRowPresent = false;
  OpdsServer server;  // Copied at construction — safe even if the store changes during browsing

  int listCount() const override { return state == State::BROWSING ? static_cast<int>(entries.size()) : 0; }
  bool hasSearch() const override { return !searchTemplate.empty(); }
  void activateIndex(int index) override;
  void buildScreen(UiScreen& screen) override;
  void drawFooter() override;
  void buildBrowsingScreen(UiScreen& screen);
  void startBrowse() override;
  void downloadFinished(bool) override { startBrowse(); }
  void fetchFeed(const std::string& path);
  void releaseEntries();
  void navigateToEntry(const OpdsEntry& entry);
  void onBackButton() override;

  // Cover Grid theme: the catalog is drawn as the Library's OPDS tab, under the Library band and
  // above the bottom bar, instead of full screen. Its top tabs are the Library's, and leaving for
  // another tab goes through goToLibraryTab / goToHomeTab, which record where the restart lands.
  HomeTab tabBarTab() const override { return HomeTab::Library; }
  int ringTopTabCount() const override { return hasTabBar() ? LibraryTabs::count() : 0; }
  int ringActiveTopTab() const override { return LibraryTabs::Opds; }
  void ringSelectTopTab(int index) override { goToLibraryTab(index); }
  void ringActivateBottomTab(HomeTab tab) override { goToHomeTab(static_cast<int>(tab)); }
  bool ringHasContent() const override { return state == State::BROWSING && !entries.empty(); }
  // Left on the catalog opens search; Up/Down reach the bar.
  bool ringContentLeftRightToBar() const override { return false; }
  void buildTopBand(UiScreen& screen) override;
  void restartAfterWifi() override;
  // Out of the browser: to the Library's OPDS tab in the Cover Grid theme, Home elsewhere.
  void leaveBrowser();
  void goToLibraryTab(int tab);
  void goToHomeTab(int tab);
  // Where onExit()'s heap-defrag restart lands: a HomeTab, LIBRARY_TAB_BASE plus a LibraryTabs
  // value, or -1 for Home.
  static constexpr int LIBRARY_TAB_BASE = 16;
  int exitTarget = -1;
  // Free heap under which a feed fetch first drops the SD-font caches: 38 KB failed the TLS
  // handshake, 66 KB succeeded.
  static constexpr uint32_t FETCH_FREE_HEAP_FLOOR = 60000;
  void downloadBook(const OpdsEntry& book);
  void performSearch(const std::string& query) override;
};
