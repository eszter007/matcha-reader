#pragma once
#include <string>
#include <vector>

#include "ReadingStatsStore.h"
#include "activities/Activity.h"
#include "components/StatsWidgets.h"
#include "components/TabRing.h"
#include "components/UiTabBand.h"
#include "components/themes/BaseTheme.h"
#include "util/ButtonNavigator.h"

class ReadingStatsActivity final : public Activity, public TabRing::Host {
  ButtonNavigator buttonNavigator;
  // The language tabs, the same FreeInkUI band the list screens build.
  UiTabBand::Host tabBand_;
  // Swallows the release that ends a long Back press, so going home does not also finish().
  bool backLongPressFired = false;
  int scrollOffset = 0;
  int maxScrollOffset = 0;
  // One swipe's worth of scroll, in px. render() owns it because the visible
  // height is only known once the header and button hints have been measured;
  // it stays 0 until the first frame, which is also when maxScrollOffset is
  // still 0, so an early swipe is a no-op either way.
  int scrollPageHeight = 0;
  // Tap targets for the calendar's month chevrons. Month stepping is bound to
  // ScreenLeft/ScreenRight, which resolve to FRONT buttons -- keys a touch board
  // does not have, so the arrows were decoration there and only the current month
  // could be viewed. Recomputed every render.
  StatsWidgets::MonthNav monthNav{};
  // Calendar month navigation
  uint16_t calYear = 0;
  uint8_t calMonth = 1;
  // Cursor in the bottom tab band, or -1 while it is on the page. Left/Right step the month on
  // this screen, so the band cannot own them all the time: Down past the end of the scroll moves
  // the cursor into it, Up takes it back out.
  int tabFocus = -1;

  // Tab 0 is every language together; 1..N are the languages the store has records for, in its
  // order. One screen rather than a list and a detail screen behind a button, so a language is
  // one tab away on every theme.
  std::vector<ReadingStatsStore::LanguageSummary> languages;
  std::vector<std::string> tabLabels;
  int selectedTab = 0;
  // Where the band was drawn, for loop()'s hit test. Empty until the first render.
  Rect tabBar{};

  // nullptr on the All tab, which reads the store's unfiltered totals.
  const char* selectedCode() const;
  void selectTab(int index);
  void stepTab(int direction);
  static std::string makeTabLabel(const char* code);
  bool stepMonthFromTap();

  // TabRing::Host. The page is one scrolling view under the tabs, so the cursor is either on the
  // page (tabs included: Confirm steps them from anywhere) or in the bottom bar.
  int ringTopTabCount() const override { return static_cast<int>(tabLabels.size()); }
  int ringActiveTopTab() const override { return selectedTab; }
  void ringSelectTopTab(int index) override { selectTab(index); }
  HomeTab ringBottomTab() const override { return HomeTab::Stats; }
  // Left/Right step the month on the page.
  bool ringContentLeftRightToBar() const override { return false; }
  TabRing::Focus ringFocus() const override {
    return tabFocus >= 0 ? TabRing::Focus::BottomBar : TabRing::Focus::Content;
  }
  void ringSetFocus(TabRing::Focus focus, bool atEnd) override;
  int& ringBarSlot() override { return tabFocus; }
  void ringChanged() override { requestUpdate(); }

 public:
  explicit ReadingStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ReadingStats", renderer, mappedInput), tabBand_(renderer) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
