#include "LibraryTabs.h"

#include <I18n.h>

#include "activities/Activity.h"
#include "components/HomeTabBar.h"
#include "components/UITheme.h"
#include "components/UiTabBand.h"

namespace LibraryTabs {

int count() { return HomeTabBar::enabled() ? 4 : 2; }

int bandItems(freeink::ui::TabItem* tabs, const int active) {
  static constexpr StrId LABELS[MAX_TABS] = {StrId::STR_TAB_BOOKS, StrId::STR_TAB_SHELVES, StrId::STR_TAB_OPDS,
                                             StrId::STR_TAB_FILES};
  const int n = count();
  for (int i = 0; i < n; i++) {
    tabs[i].label = I18n::getInstance().get(LABELS[i]);
    tabs[i].value = static_cast<int16_t>(i);
    tabs[i].selected = i == active;
  }
  return n;
}

UiTabBand::Options bandOptions(const bool focused, const bool hasTouch, const freeink::ui::ActionId action) {
  UiTabBand::Options options;
  options.action = action;
  options.focused = focused;
  options.hasTouch = hasTouch;
  // Two tabs in equal slots (themes other than Cover Grid): keep the pill near its label.
  options.pillMaxPad = 16;
  return options;
}

void buildBand(UiAppHost::UiScreen& screen, const GfxRenderer& renderer, const int active, const bool focused,
               const bool hasTouch, const freeink::ui::ActionId action) {
  freeink::ui::TabItem tabs[MAX_TABS];
  const int n = bandItems(tabs, active);
  UiTabBand::build(screen, renderer, tabs, n, bandOptions(focused, hasTouch, action));
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
