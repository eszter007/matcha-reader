#pragma once

#include "activities/Activity.h"

// Refresh Stats on the Insights screen, and Sync Stats in Settings: joins WiFi, downloads this account's daily
// minutes and uploads the device's own stats. From Insights it also syncs every book's reading progress. The download
// is merged at the next boot (ReadingStatsStore::compact()), and the restart that ends the WiFi session lands back on
// Insights, so the screen shows the merged numbers. From Insights it is opened with replaceActivity(), so the stats
// tables are freed first; from Settings it returns there instead. A button shortcut can start it anywhere, syncing the
// clock in the same Wi-Fi session; its restart lands back in the book when started from one, else on Home.
class StatsSyncActivity final : public Activity {
 public:
  enum class Origin : uint8_t { Insights, Settings, Shortcut, ShortcutInReader };

  explicit StatsSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                             const Origin origin = Origin::Insights, const bool syncClock = false)
      : Activity("StatsSync", renderer, mappedInput),
        origin(origin),
        syncClock(syncClock),
        restartOnExit(fromShortcut()) {
    running = true;
  }
  ~StatsSyncActivity() override { running = false; }

  // One sync at a time: the shortcuts are live on every screen, this one included.
  static bool isRunning() { return running; }

  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool skipLoopDelay() override { return true; }
  void render(RenderLock&&) override;

 private:
  enum State { SYNCING, DONE, NO_ACCOUNT, FAILED, CANCELLED };
  State state = SYNCING;
  const Origin origin;
  const bool syncClock;
  static inline bool running = false;
  // Set from the start for a shortcut (it may have replaced the screen it returns to), else once
  // WiFi was joined here; leaving then restarts back to where the sync started.
  bool restartOnExit = false;

  // Book being synced and the library's book count, shown while the books sync.
  uint16_t bookDone = 0;
  uint16_t bookTotal = 0;

  bool fromShortcut() const { return origin == Origin::Shortcut || origin == Origin::ShortcutInReader; }
  // Insights also syncs the reading progress of every book opened on this device.
  bool syncsBooks() const { return origin == Origin::Insights; }
  void runSync();
  // False when a book's progress could not be synced; books never opened here are skipped.
  bool syncAllBooks();
  void ensureLibraryIndex();
  void leave();
};
