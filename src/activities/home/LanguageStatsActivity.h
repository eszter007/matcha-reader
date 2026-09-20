#pragma once
#include <string>
#include <vector>

#include "ReadingStatsStore.h"
#include "activities/Activity.h"
#include "components/StatsWidgets.h"
#include "components/themes/BaseTheme.h"
#include "util/ButtonNavigator.h"

// Insights split by language: one tab each, same cards as the overall screen.
// Opened with Confirm ("Details") via startActivityForResult, so Insights keeps its state.
class LanguageStatsActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  // Snapshotted in onEnter: deriving it walks the whole history, and render() runs per scroll.
  std::vector<ReadingStatsStore::LanguageSummary> languages;
  // Owned strings, not a rotating buffer: render() collects every label into a TabInfo vector
  // (which holds const char*) before any is drawn, so all of them must stay alive at once.
  std::vector<std::string> tabLabels;
  int selectedTab = 0;
  // Swallows the release ending a long Back press, so going home does not also finish().
  bool backLongPressFired = false;
  int scrollOffset = 0;
  int maxScrollOffset = 0;
  // One swipe's worth of scroll, in px, as on the overall screen: render() owns it
  // because the visible height is only known once the header band is measured.
  int scrollPageHeight = 0;
  // Where render() last drew the tab row, for hit-testing taps on it. The tabs sit
  // in the fixed band above the scrolled content, so this does not move with scroll.
  Rect tabBar{};
  // Calendar month chevrons, same reason as on the overall screen: the keys that
  // step months do not exist on a touch board.
  StatsWidgets::MonthNav monthNav{};
  uint16_t calYear = 0;
  uint8_t calMonth = 1;

  // Language endonym, or the bare tag when the firmware ships no UI for it.
  static std::string makeTabLabel(const char* code);
  const char* selectedCode() const;
  std::vector<TabInfo> buildTabs() const;
  void selectTab(int index);
  bool stepMonthFromTap();

 public:
  explicit LanguageStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("LanguageStats", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
