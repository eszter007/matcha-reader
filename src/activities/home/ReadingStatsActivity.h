#pragma once
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class ReadingStatsActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  int scrollOffset = 0;
  int maxScrollOffset = 0;
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
