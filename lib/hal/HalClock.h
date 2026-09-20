#pragma once

#include <Arduino.h>
#include <Rtc.h>

class HalClock;
extern HalClock halClock;  // Singleton

class HalClock {
  bool _available = false;
  mutable Rtc _sdkRtc;
  // The RTC keeps UTC; local time comes from newlib's localtime_r under the
  // POSIX TZ rule set via setTimezone(), so zones with DST are correct
  // year-round. Cached as a UTC epoch to keep the RTC bus quiet.
  mutable time_t _cachedUtc = 0;
  mutable bool _hasCachedTime = false;
  mutable unsigned long _lastPollMs = 0;

  static constexpr unsigned long CLOCK_POLL_MS = 10000;  // 10 seconds

 public:
  // Call after BoardConfig has selected the active device.
  void begin();

  // True if an RTC chip is present on this device. Use hasTime() to decide whether a time
  // can be shown -- these differ on RTC-less boards, where the system clock still works.
  bool isAvailable() const { return _available; }

  // True when a time can be reported at all: from the RTC, or from the system clock that
  // restoreSystemTime() and the NTP resync keep honest on boards without one.
  bool hasTime() const { return _available || systemTimeValid(); }

  // Set the POSIX TZ rule (e.g. "CET-1CEST,M3.5.0,M10.5.0/3") applied to every
  // read. nullptr/empty falls back to UTC. Drops the read cache so the change
  // shows immediately.
  void setTimezone(const char* posixTz);

  // Current wall-clock time in the configured timezone.
  // Returns false if RTC is not available.
  bool localTime(struct tm& out) const;

  // Get current local hour (0-23) and minute (0-59).
  // Returns false if RTC is not available.
  bool getTime(uint8_t& hour, uint8_t& minute) const;

  // Format the local time into a caller-provided buffer.
  // 24h mode produces "HH:MM" (needs >=6 bytes); 12h mode produces "H:MM AM"/"HH:MM PM" (needs >=9 bytes).
  // Returns false if RTC is not available.
  bool formatTime(char* buf, size_t bufSize, bool use12Hour = false) const;

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

 private:
  bool writeTimeToRTC(uint8_t hour, uint8_t minute, uint8_t second);
};
