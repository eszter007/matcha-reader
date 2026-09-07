#pragma once

#include <Arduino.h>
#include <BatteryMonitor.h>
#include <InputManager.h>
#include <Logging.h>
#include <freertos/semphr.h>

#include <atomic>
#include <cassert>

#include "HalGPIO.h"

class HalPowerManager;
extern HalPowerManager powerManager;  // Singleton

class HalPowerManager {
  int normalFreq = 0;  // MHz
  bool isLowPower = false;

  mutable int _batteryCachedPercent = 0;         // Last read battery percentage (0-100)
  mutable unsigned long _batteryLastPollMs = 0;  // Timestamp of last battery read in milliseconds

  // Nesting count, not a flag: the render task and a foreground section build hold a lock at the
  // same time, and whichever released first would otherwise un-throttle the other.
  //
  // Atomic rather than mutex-protected: setPowerSaving() reads it from the main loop while Lock
  // ctors/dtors run on the render and build tasks, and a plain read racing those writes is UB
  // regardless of how stale a value we are willing to tolerate. 16 bits with no clamp keeps the
  // increment and decrement exactly symmetric -- Lock is non-copyable and non-movable, so every
  // increment has exactly one matching decrement and the count cannot run away.
  std::atomic<uint16_t> lockCount{0};

  // Serializes the actual setCpuFrequencyMhz() transition and the isLowPower flag behind it.
  // Locks are taken from the render and build tasks, so two tasks can now reach the transition
  // at once; without this they could interleave a raise and a drop and leave isLowPower
  // disagreeing with the real clock.
  SemaphoreHandle_t freqMutex = nullptr;

 public:
#if BOARD_HAS_PSRAM
  static constexpr int LOW_POWER_FREQ = 80;  // MHz
#else
  static constexpr int LOW_POWER_FREQ = 10;  // MHz
#endif
  static constexpr unsigned long IDLE_POWER_SAVING_MS = 3000;  // ms
  static constexpr unsigned long BATTERY_POLL_MS = 1500;       // ms

  void begin();

  // Control CPU frequency for power saving
  void setPowerSaving(bool enabled);

  // Setup wake up GPIO and enter deep sleep
  // Should be called inside main loop() to handle the pending lock state
  void startDeepSleep(HalGPIO& gpio) const;

  // Get battery percentage (range 0-100)
  uint16_t getBatteryPercentage() const;

  // RAII helper class to manage power saving locks
  // Usage: create an instance of Lock in a scope to disable power saving, for example when running a task that needs
  // full performance. Locks nest: power saving is re-enabled when the LAST one goes out of scope.
  class Lock {
    friend class HalPowerManager;

   public:
    explicit Lock();
    ~Lock();

    // Non-copyable and non-movable
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
    Lock(Lock&&) = delete;
    Lock& operator=(Lock&&) = delete;
  };
};
