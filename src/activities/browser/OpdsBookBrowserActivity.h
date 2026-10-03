#pragma once
#include <OpdsParser.h>

#include <string>
#include <utility>
#include <vector>

#include "OpdsServerStore.h"
#include "activities/Activity.h"
#include "components/LibraryTabs.h"
#include "components/TabRing.h"
#include "components/UiAppHost.h"
#include "util/ButtonNavigator.h"

/**
 * Activity for browsing and downloading books from an OPDS server.
 * Supports navigation through catalog hierarchy and downloading EPUBs.
 */
class OpdsBookBrowserActivity final : public Activity, private UiAppHost, public TabRing::Host {
 public:
  enum class BrowserState { CHECK_WIFI, WIFI_SELECTION, LOADING, BROWSING, DOWNLOADING, ERROR, SEARCH_INPUT };

  explicit OpdsBookBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, OpdsServer server);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  BrowserState state = BrowserState::LOADING;
  std::vector<OpdsEntry> entries;
  // Row buffer, built whenever entries changes (fetchFeed()/releaseEntries())
  // so buildBrowsingScreen() reuses it on every repaint instead of rebuilding
  // a ListItem vector per render.
  std::vector<freeink::ui::ListItem> rowItems;
  void rebuildRowItems();
  std::vector<std::string> navigationHistory;
  std::string currentPath;
  std::string searchTemplate;
  int selectorIndex = 0;
  bool leftSearchPending = false;
  std::string errorMessage;
  std::string statusMessage;
  size_t downloadProgress = 0;
  size_t downloadTotal = 0;

  OpdsServer server;  // Copied at construction — safe even if the store changes during browsing

  // Viewport memory (top/visibleRows) for the browsing list; `selected` is
  // mirrored from selectorIndex at build/move time.
  freeink::ui::ListNav listNav;
  // Read by HttpDownloader between chunks; set by the Cancel button handler or
  // a Back press, both pumped from the download's progress callback.
  bool cancelDownload = false;
  // Set when the cancel came from the home gesture (consumed by the download
  // callback's own input pump); exit to home after the abort unwinds.
  bool goHomeAfterCancel = false;

  // Single screen fn dispatching on `state`: every state shares the themed
  // header and gets built through FreeInkUI.
  static void rootScreen(UiScreen& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onSearchEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onCancelEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onBackEvent(const freeink::ui::ActionEvent& event, void* user);
  void screenHeader(UiScreen& screen, bool withSearch);
  void buildBrowsingScreen(UiScreen& screen);
  void buildDownloadScreen(UiScreen& screen);
  void buildStatusScreen(UiScreen& screen);
  void activateSelected();

  void checkAndConnectWifi();
  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
  void fetchFeed(const std::string& path);
  void releaseEntries();
  void navigateToEntry(const OpdsEntry& entry);
  void navigateBack();
  // Out of the browser: to the Library's OPDS tab in the Cover Grid theme, Home elsewhere.
  void leaveBrowser();

  // Cover Grid theme: the catalog is drawn as the Library's OPDS tab, under the Library band and
  // above the bottom bar, instead of full screen. Only the Wi-Fi picker still takes the screen.
  static bool inLibraryTab();
  // Cursor slot in the bottom bar on a button board, -1 while it is on the catalog.
  int tabFocus = -1;
  // Where the next onExit()'s heap-defrag restart lands: a HomeTab, LIBRARY_TAB_BASE plus a
  // LibraryTabs value, or -1 for Home.
  static constexpr int LIBRARY_TAB_BASE = 16;
  int exitTarget = -1;
  void goToLibraryTab(int tab);
  void goToHomeTab(int tab);
  // Library band holds the button cursor (ring: band -> rows -> bottom bar -> band).
  bool bandFocused = false;
  // Band and bottom-bar input; true when it consumed the pass.
  bool handleTabInput();
  // TabRing::Host: the catalog is the Library's OPDS view, so its top tabs are the Library's and
  // leaving for another tab goes through goToLibraryTab / goToHomeTab (they record exitTarget).
  int ringTopTabCount() const override { return inLibraryTab() ? LibraryTabs::count() : 0; }
  int ringActiveTopTab() const override { return LibraryTabs::Opds; }
  void ringSelectTopTab(int index) override { goToLibraryTab(index); }
  HomeTab ringBottomTab() const override { return inLibraryTab() ? HomeTab::Library : HomeTab::Count; }
  void ringActivateBottomTab(HomeTab tab) override { goToHomeTab(static_cast<int>(tab)); }
  bool ringHasContent() const override { return state == BrowserState::BROWSING && !entries.empty(); }
  // Left on the catalog opens search (leftSearchPending); Up/Down reach the bar.
  bool ringContentLeftRightToBar() const override { return false; }
  TabRing::Focus ringFocus() const override;
  void ringSetFocus(TabRing::Focus focus, bool atEnd) override;
  int& ringBarSlot() override { return tabFocus; }
  void ringChanged() override { requestUpdate(); }
  // Free heap under which a feed fetch first drops the SD-font caches: 38 KB failed the TLS
  // handshake, 66 KB succeeded.
  static constexpr uint32_t FETCH_FREE_HEAP_FLOOR = 60000;
  void downloadBook(const OpdsEntry& book);
  void launchSearch();
  void performSearch(const std::string& query);
  bool preventAutoSleep() override;
};
