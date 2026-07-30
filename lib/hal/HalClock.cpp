#include "HalClock.h"

#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <sys/time.h>
#include <time.h>

#include <cassert>
#include <cstdlib>
#include <cstring>

HalClock halClock;  // Singleton instance

namespace {
// Epoch stash for RTC-less devices; a plain decimal epoch in a tiny SD file.
constexpr const char* CLOCK_STASH_PATH = "/system/.clock";

// The firmware build date as an epoch -- the absolute "time can't be before this" floor.
// Computed once from the compiler-provided __DATE__/__TIME__ ("Jul  4 2026" / "12:34:56").
time_t buildEpoch() {
  static time_t cached = [] {
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char* d = __DATE__;
    struct tm t = {};
    for (int i = 0; i < 12; i++) {
      if (strncmp(d, months + i * 3, 3) == 0) {
        t.tm_mon = i;
        break;
      }
    }
    t.tm_mday = atoi(d + 4);
    t.tm_year = atoi(d + 7) - 1900;
    const char* tt = __TIME__;
    t.tm_hour = atoi(tt);
    t.tm_min = atoi(tt + 3);
    t.tm_sec = atoi(tt + 6);
    return mktime(&t);  // TZ is unset on this firmware, so mktime treats t as UTC
  }();
  return cached;
}
}  // namespace

// DS3231 register layout (BCD encoded):
//   0x00: Seconds  (bits 6-4 = tens, bits 3-0 = ones)
//   0x01: Minutes  (bits 6-4 = tens, bits 3-0 = ones)
//   0x02: Hours    (bit 6 = 12/24 mode, bits 5-4 = tens, bits 3-0 = ones)

static uint8_t bcdToDec(uint8_t bcd) { return ((bcd >> 4) * 10) + (bcd & 0x0F); }
static uint8_t decToBcd(uint8_t dec) { return ((dec / 10) << 4) | (dec % 10); }

void HalClock::begin() {
  _available = _sdkRtc.begin();
  LOG_INF("CLK", _available ? "SDK RTC found" : "RTC not found");
}

bool HalClock::getTime(uint8_t& hour, uint8_t& minute) const {
  if (!_available) return false;

  const unsigned long now = millis();
  if (_lastPollMs != 0 && (now - _lastPollMs) < CLOCK_POLL_MS) {
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }

  Rtc::DateTime dt;
  if (!_sdkRtc.now(dt)) {
    if (!_hasCachedTime) return false;
    _lastPollMs = now;
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }
  _cachedHour = dt.hour;
  _cachedMinute = dt.minute;
  _lastPollMs = now;
  _hasCachedTime = true;
  hour = _cachedHour;
  minute = _cachedMinute;
  return true;
}

bool HalClock::formatTime(char* buf, size_t bufSize, uint8_t utcOffsetQuarterHoursBiased, bool use12Hour) const {
  if (bufSize < (use12Hour ? 9u : 6u)) return false;
  uint8_t h, m;
  if (!getTime(h, m)) return false;

  // Apply UTC offset: convert biased value to signed quarter-hours.
  // Clamp against corrupted persisted values so display time can't drift outside [-12:00, +14:00].
  if (utcOffsetQuarterHoursBiased > 104) utcOffsetQuarterHoursBiased = 104;
  int offsetQuarterHours = static_cast<int>(utcOffsetQuarterHoursBiased) - 48;
  int totalMinutes = static_cast<int>(h) * 60 + static_cast<int>(m) + offsetQuarterHours * 15;

  // Wrap around 24 hours
  totalMinutes = ((totalMinutes % 1440) + 1440) % 1440;

  const int hour24 = totalMinutes / 60;
  const int min = totalMinutes % 60;
  if (use12Hour) {
    const bool pm = hour24 >= 12;
    int hour12 = hour24 % 12;
    if (hour12 == 0) hour12 = 12;
    snprintf(buf, bufSize, "%d:%02d %s", hour12, min, pm ? "PM" : "AM");
  } else {
    snprintf(buf, bufSize, "%02d:%02d", hour24, min);
  }
  return true;
}

bool HalClock::syncFromNTP() {
  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("CLK", "WiFi not connected, cannot sync NTP");
    return false;
  }

  LOG_INF("CLK", "Starting NTP sync...");
  configTzTime("UTC0", "pool.ntp.org", "time.nist.gov");

  // Wait for SNTP sync to complete (up to 5 seconds)
  constexpr int maxAttempts = 50;
  for (int i = 0; i < maxAttempts; i++) {
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
      time_t now = time(nullptr);
      struct tm timeinfo;
      gmtime_r(&now, &timeinfo);

      // Matcha: stash the freshly-synced epoch so RTC-less devices (X4) survive the next
      // power-off with a plausible date (see restoreSystemTime). Independent of the DS3231,
      // so it must not sit inside the RTC-write branch below.
      persistSystemTime();

      Rtc::DateTime dt;
      dt.year = static_cast<uint16_t>(timeinfo.tm_year + 1900);
      dt.month = static_cast<uint8_t>(timeinfo.tm_mon + 1);
      dt.day = static_cast<uint8_t>(timeinfo.tm_mday);
      dt.hour = static_cast<uint8_t>(timeinfo.tm_hour);
      dt.minute = static_cast<uint8_t>(timeinfo.tm_min);
      dt.second = static_cast<uint8_t>(timeinfo.tm_sec);
      dt.weekday = static_cast<uint8_t>(timeinfo.tm_wday);
      if (_sdkRtc.set(dt)) {
        _lastPollMs = 0;
        _cachedHour = dt.hour;
        _cachedMinute = dt.minute;
        _hasCachedTime = true;
        LOG_INF("CLK", "RTC set to %04u-%02u-%02u %02u:%02u:%02u UTC", dt.year, dt.month, dt.day, dt.hour, dt.minute,
                dt.second);
        return true;
      }
      // No DS3231 (X4) or the write failed. The SYSTEM clock is set either way, and that is what
      // reading-stats dates read via time(nullptr) -- so this is still a successful sync for us.
      LOG_INF("CLK", "System time set to %02d:%02d:%02d UTC (hardware RTC not written)", timeinfo.tm_hour,
              timeinfo.tm_min, timeinfo.tm_sec);
      return true;
    }
    delay(100);
  }

  LOG_ERR("CLK", "NTP sync timed out");
  return false;
}

bool HalClock::systemTimeValid() { return time(nullptr) >= buildEpoch(); }

void HalClock::restoreSystemTime() const {
  if (systemTimeValid()) return;  // internal RTC timer survived (deep sleep / soft reset)

  time_t best = buildEpoch();
  char buf[24] = {};
  if (Storage.readFileToBuffer(CLOCK_STASH_PATH, buf, sizeof(buf) - 1) > 0) {
    const long long stashed = atoll(buf);
    if (stashed > static_cast<long long>(best)) best = static_cast<time_t>(stashed);
  }

  const timeval tv = {best, 0};
  settimeofday(&tv, nullptr);
  struct tm timeinfo;
  gmtime_r(&best, &timeinfo);
  LOG_INF("CLK", "System time restored to %04d-%02d-%02d %02d:%02d UTC (stash/build-date; sync via WiFi for exact)",
          timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday, timeinfo.tm_hour, timeinfo.tm_min);
}

void HalClock::persistSystemTime() const {
  if (!systemTimeValid()) return;
  Storage.mkdir("/system");  // no-op when it already exists
  char buf[24];
  snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(time(nullptr)));
  HalFile f;
  if (Storage.openFileForWrite("CLK", CLOCK_STASH_PATH, f)) {
    f.write(reinterpret_cast<const uint8_t*>(buf), strlen(buf));
  }
}

time_t HalClock::localEpoch(uint8_t utcOffsetQuarterHoursBiased) {
  if (utcOffsetQuarterHoursBiased > 104) utcOffsetQuarterHoursBiased = 104;  // same clamp as formatTime
  const int offsetQuarterHours = static_cast<int>(utcOffsetQuarterHoursBiased) - 48;
  return time(nullptr) + static_cast<time_t>(offsetQuarterHours) * 15 * 60;
}
