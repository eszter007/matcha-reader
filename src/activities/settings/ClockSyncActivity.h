#pragma once

#include "activities/Activity.h"

// Manual NTP resync action. Runs a forced sync (bypassing the once-per-device debounce),
// reports success/failure, then waits for Back. If WiFi is not connected yet, it reuses the
// normal WiFi selection flow first.
//
// Reached from Settings, or from anywhere with Power + Up. returnToReader is set when it was
// opened over a book: the Wi-Fi session then ends in a restart back into that book, not Home.
class ClockSyncActivity final : public Activity {
 public:
  explicit ClockSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const bool returnToReader = false)
      : Activity("ClockSync", renderer, mappedInput), returnToReader(returnToReader) {
    running = true;
  }
  ~ClockSyncActivity() override { running = false; }

  // One sync at a time: the button combination is live on every screen, this one included.
  static bool isRunning() { return running; }

  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool skipLoopDelay() override { return true; }
  void render(RenderLock&&) override;

 private:
  enum State { SYNCING, SUCCESS, NO_WIFI, FAILED };
  State state = SYNCING;
  char syncedTime[16] = {0};
  bool shouldTearDownWifiOnExit = false;
  const bool returnToReader;
  static inline bool running = false;

  void runSync();
  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
};
