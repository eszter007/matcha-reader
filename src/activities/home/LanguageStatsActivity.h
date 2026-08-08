#pragma once
#include <vector>

#include "ReadingStatsStore.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Insights, broken down by the language of what was read: one tab per language, each showing the
// same streak card, tile grid and calendar as the overall screen.
//
// Reached with Confirm ("Details") from ReadingStatsActivity, via startActivityForResult, so the
// overall screen keeps its scroll position and calendar month underneath.
class LanguageStatsActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  // Snapshotted in onEnter rather than recomputed per render: the list is derived by walking the
  // whole per-language history, and render() runs on every scroll step.
  std::vector<ReadingStatsStore::LanguageSummary> languages;
  int selectedTab = 0;
  // Swallows the release that ends a long Back press, so going home does not also finish().
  bool backLongPressFired = false;
  int scrollOffset = 0;
  int maxScrollOffset = 0;
  uint16_t calYear = 0;
  uint8_t calMonth = 1;

  // Display label for a stored code: the tag uppercased ("ja" -> "JA"), or "Unknown" for the
  // empty bucket that TXT/XTC and untagged manga fall into. Deliberately not a code-to-name
  // table: that would be ~180 entries of flash for a screen whose tabs a reader picked
  // themselves, and it could not be localised without 180 more strings per language.
  const char* tabLabel(int index) const;
  const char* selectedCode() const;

 public:
  explicit LanguageStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("LanguageStats", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
