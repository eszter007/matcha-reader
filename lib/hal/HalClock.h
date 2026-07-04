#pragma once

#include <Arduino.h>
#include <Wire.h>

#include "HalGPIO.h"

class HalClock;
extern HalClock halClock;  // Singleton

class HalClock {
  bool _available = false;
  mutable uint8_t _cachedHour = 0;
  mutable uint8_t _cachedMinute = 0;
  mutable bool _hasCachedTime = false;
  mutable unsigned long _lastPollMs = 0;

  static constexpr unsigned long CLOCK_POLL_MS = 10000;  // 10 seconds

 public:
  // Call after gpio.begin() and powerManager.begin() (I2C already initialised for X3)
  void begin();

  // True if the DS3231 RTC is present on this device
  bool isAvailable() const { return _available; }

  // Get current hour (0-23) and minute (0-59).
  // Returns false if RTC is not available.
  bool getTime(uint8_t& hour, uint8_t& minute) const;

  // Format time into a caller-provided buffer.
  // 24h mode produces "HH:MM" (needs >=6 bytes); 12h mode produces "H:MM AM"/"HH:MM PM" (needs >=9 bytes).
  // utcOffsetQuarterHoursBiased: biased quarter-hour offset (48 = UTC+0, 0 = UTC-12, 104 = UTC+14).
  // use12Hour: when true, format as 12-hour clock with AM/PM suffix.
  // Returns false if RTC is not available.
  bool formatTime(char* buf, size_t bufSize, uint8_t utcOffsetQuarterHoursBiased = 48, bool use12Hour = false) const;

  // Sync the system clock (and the DS3231 RTC when present) from an NTP server. Requires WiFi
  // to be connected. Blocks for up to ~5s while waiting for the SNTP response.
  // Returns true once the SYSTEM time is set -- on RTC-less devices (X4) that is the whole
  // point: reading-stats dates come from time(nullptr), not from the DS3231.
  //
  // Debouncing (skip if already synced once) is enforced by the caller, not here,
  // so the HAL stays free of any app-layer settings dependency.
  bool syncFromNTP();

  // --- System-time keeping for devices WITHOUT the DS3231 (X4) ---
  // The ESP32's internal RTC timer keeps time(nullptr) ticking across deep sleep and software
  // resets, but a full power-off restarts it at the 1970 epoch -- which is exactly where the
  // Insights calendar's "January 1970" came from. These keep the system clock plausible:
  // periodically stash the epoch to SD, and on boot restore from the stash (or, failing that,
  // the firmware build date) whenever the clock is clearly unset. Time spent powered off is
  // lost until the next NTP sync, but the DATE stays right for normal usage patterns.

  // True when time(nullptr) is at least the firmware build date (i.e. was ever set).
  static bool systemTimeValid();
  // Boot: seed the system clock from the SD stash / build date when it is unset.
  void restoreSystemTime() const;
  // Periodic (and after NTP sync): stash the current epoch to SD when valid.
  void persistSystemTime() const;
  // Current epoch shifted by the user's display UTC offset (SETTINGS.clockUtcOffsetQ encoding:
  // biased quarter hours, 48 = UTC+0). Use for DATE decisions (reading-stats day boundaries)
  // so days flip at local midnight instead of UTC midnight.
  static time_t localEpoch(uint8_t utcOffsetQuarterHoursBiased);

 private:
  bool writeTimeToRTC(uint8_t hour, uint8_t minute, uint8_t second);
};
