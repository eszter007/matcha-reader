#pragma once

#include <MangaPanel.h>

#include <memory>
#include <string>
#include <vector>

#include "../../BookmarkEntry.h"
#include "EpubReaderMenuActivity.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class MangaReaderActivity final : public Activity {
 public:
  explicit MangaReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                               std::unique_ptr<manga::MangaBook> book)
      : Activity("MangaReader", renderer, mappedInput), book(std::move(book)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  ScreenshotInfo getScreenshotInfo() const override;

 private:
  std::unique_ptr<manga::MangaBook> book;

  uint32_t currentPage = 0;
  int currentPanel = -1;  // -1 = full page view, 0+ = zoomed panel
  int pagesUntilFullRefresh = 0;

  std::vector<manga::Panel> panels;
  bool panelsLoaded = false;

  bool showTextOverlay = false;
  bool pendingScreenshot = false;
  bool ignoreNextConfirmRelease = false;
  unsigned long readingSessionStartMs = 0;

  bool automaticPageTurnActive = false;
  unsigned long lastPageTurnTime = 0;
  unsigned long pageTurnDuration = 0;

  bool currentPageBookmarked = false;
  bool showBookmarkMessage = false;
  bool bookmarkRemoved = false;  // true when last toggle removed (controls popup text)
  unsigned long bookmarkMessageTime = 0UL;
  std::vector<BookmarkEntry> cachedBookmarks;

  ButtonNavigator buttonNavigator;

  enum class ViewMode { FullPage, PanelZoom, TextOverlay };
  ViewMode viewMode = ViewMode::FullPage;

  // Set when a full page finishes rendering; the idle prefetch below warms the NEXT page's pixel
  // cache once per displayed page, after a short dwell, so forward page turns hit the cache
  // instead of running a fresh JPEG decode.
  bool nextPagePrefetched = true;  // true until the first full-page render arms it
  unsigned long fullPageRenderedMs = 0;

  // Geometry of a full-page image on the (possibly temporarily rotated) screen. Shared by
  // renderFullPage() and prefetchNextPageCache() so the prefetch-written pixel cache has exactly
  // the dimensions the later render expects.
  struct FullPageGeom {
    int x = 0, y = 0;
    int destWidth = 0, destHeight = 0;
    int screenW = 0, screenH = 0;
    bool rotated = false;
    int savedOrientation = 0;  // GfxRenderer::Orientation of the caller, restored after use
  };
  // NOTE: sets the renderer orientation when rotation is needed -- the caller must restore
  // savedOrientation when done.
  FullPageGeom applyFullPageGeometry(int imgWidth, int imgHeight);

  void prefetchNextPageCache();

  void loadCurrentPagePanels();
  void renderFullPage();
  void renderPanelZoom();
  void renderTextOverlay();

  void nextPanel();
  void prevPanel();
  void nextPage();
  void prevPage();

  void saveProgress() const;
  void loadProgress();

  void loadCachedBookmarks();
  void updateBookmarkFlag();
  void addBookmark();

  void launchWordLookup();
  void launchMenu();
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  void toggleAutoPageTurn(uint8_t selectedPageTurnOption);
};
