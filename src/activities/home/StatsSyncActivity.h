#pragma once

#include "activities/Activity.h"

// Refresh Stats on the Insights screen, and Sync Stats in Settings: joins WiFi, downloads this account's daily minutes and
// uploads the device's own stats. The download is merged at the next boot (ReadingStatsStore::
// compact()), and the restart that ends the WiFi session lands back on Insights, so the screen
// shows the merged numbers. From Insights it is opened with replaceActivity(), so the stats tables are
// freed first; from Settings it returns there instead.
class StatsSyncActivity final : public Activity {
 public:
  explicit StatsSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const bool fromSettings = false)
      : Activity("StatsSync", renderer, mappedInput), fromSettings(fromSettings) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool skipLoopDelay() override { return true; }
  void render(RenderLock&&) override;

 private:
  enum State { SYNCING, NO_ACCOUNT, FAILED, CANCELLED };
  State state = SYNCING;
  const bool fromSettings;
  // Set once WiFi was joined here; leaving then restarts back into Insights.
  bool restartOnExit = false;

  void runSync();
  void leave();
};
