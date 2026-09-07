#pragma once
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class ReadingStatsActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  // Swallows the release that ends a long Back press, so going home does not also finish().
  bool backLongPressFired = false;
  int scrollOffset = 0;
  int maxScrollOffset = 0;
  // One swipe's worth of scroll, in px. render() owns it because the visible
  // height is only known once the header and button hints have been measured;
  // it stays 0 until the first frame, which is also when maxScrollOffset is
  // still 0, so an early swipe is a no-op either way.
  int scrollPageHeight = 0;
  // Calendar month navigation
  uint16_t calYear = 0;
  uint8_t calMonth = 1;

 public:
  explicit ReadingStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ReadingStats", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
