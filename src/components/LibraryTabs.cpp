#include "LibraryTabs.h"

#include <I18n.h>

#include "activities/Activity.h"
#include "components/HomeTabBar.h"
#include "components/UITheme.h"

namespace LibraryTabs {

int count() { return HomeTabBar::enabled() ? 4 : 2; }

std::vector<TabInfo> build(const int active) {
  std::vector<TabInfo> tabs;
  tabs.reserve(count());
  tabs.push_back({tr(STR_TAB_BOOKS), active == Books});
  tabs.push_back({tr(STR_TAB_SHELVES), active == Shelves});
  if (count() > 2) {
    tabs.push_back({tr(STR_TAB_OPDS), active == Opds});
    tabs.push_back({tr(STR_TAB_FILES), active == Files});
  }
  return tabs;
}

Rect barRect(const GfxRenderer& renderer, const MappedInputManager& input) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return Rect{0, static_cast<int16_t>(metrics.topPadding + metrics.headerHeight),
              static_cast<int16_t>(renderer.getScreenWidth()),
              static_cast<int16_t>(tabBandHeight(metrics, input.hasTouch()))};
}

int height(const MappedInputManager& input) {
  return tabBandHeight(UITheme::getInstance().getMetrics(), input.hasTouch());
}

int hitTest(const GfxRenderer& renderer, const MappedInputManager& input, const int x, const int y, const int active) {
  const Rect bar = barRect(renderer, input);
  if (y < bar.y || y >= bar.y + bar.height) return -1;
  int tab = -1;
  if (!UITheme::getInstance().getTheme().tabIndexFromPoint(renderer, bar, build(active), x, y, tab)) return -1;
  return tab;
}

void activate(const int tab) {
  if (tab == Opds) {
    activityManager.goToOpdsServers();
    return;
  }
  if (tab == Files) {
    // Explicitly the card root: the default argument is an empty path, which lists nothing and
    // also fails showsLibraryTabs(), so the browser came up blank and without the band.
    activityManager.goToFileBrowser("/");
    return;
  }
  activityManager.goToLibrary(tab);
}

}  // namespace LibraryTabs
