#pragma once
#include <string>
#include <vector>

#include "ReadingStatsStore.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Insights split by language: one tab each, same cards as the overall screen.
// Opened with Confirm ("Details") via startActivityForResult, so Insights keeps its state.
class LanguageStatsActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  // Snapshotted in onEnter: deriving it walks the whole history, and render() runs per scroll.
  std::vector<ReadingStatsStore::LanguageSummary> languages;
  // Owned strings, not a rotating buffer: render() collects every label into a TabInfo vector
  // (which holds const char*) before any is drawn, so all of them must stay alive at once.
  std::vector<std::string> labels;
  int selectedTab = 0;
  // Swallows the release ending a long Back press, so going home does not also finish().
  bool backLongPressFired = false;
  int scrollOffset = 0;
  int maxScrollOffset = 0;
  uint16_t calYear = 0;
  uint8_t calMonth = 1;

  // Language endonym, or the bare tag when the firmware ships no UI for it.
  static std::string makeTabLabel(const char* code);
  const char* selectedCode() const;

 public:
  explicit LanguageStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("LanguageStats", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
