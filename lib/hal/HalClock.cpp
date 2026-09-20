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

#include "SdSystemDir.h"

HalClock halClock;  // Singleton instance

namespace {
// Epoch stash for RTC-less devices; a plain decimal epoch in a tiny SD file.
std::string clockStashPath() { return sdsystem::path(".clock"); }

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

namespace {
// UTC calendar date -> Unix epoch, no timezone involvement (newlib has no
// timegm). Days-from-civil per Howard Hinnant's algorithm.
time_t epochFromUtc(const Rtc::DateTime& dt) {
  int y = dt.year;
  const int m = dt.month;
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153u * static_cast<unsigned>(m + (m > 2 ? -3 : 9)) + 2u) / 5u + dt.day - 1u;
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  const long days = static_cast<long>(era) * 146097L + static_cast<long>(doe) - 719468L;
  return static_cast<time_t>(days) * 86400 + dt.hour * 3600L + dt.minute * 60L + dt.second;
}
}  // namespace

void HalClock::setTimezone(const char* posixTz) {
  setenv("TZ", posixTz && posixTz[0] != '\0' ? posixTz : "UTC0", 1);
  tzset();
  _lastPollMs = 0;  // re-derive local time under the new rule immediately
}

bool HalClock::localTime(struct tm& out) const {
  if (!_available) {
    // No DS3231 (X4). The system clock is still real here: restoreSystemTime() seeds it at
    // boot and WifiSelectionActivity re-syncs it on every connect, so report that rather
    // than nothing. No caching -- time(nullptr) costs no bus traffic, which is the only
    // thing the cache below exists to avoid.
    if (!systemTimeValid()) return false;
    const time_t now = time(nullptr);
    localtime_r(&now, &out);
    return true;
  }

  const unsigned long now = millis();
  if (_lastPollMs == 0 || (now - _lastPollMs) >= CLOCK_POLL_MS) {
    Rtc::DateTime dt;
    if (_sdkRtc.now(dt)) {
      _cachedUtc = epochFromUtc(dt);
      _hasCachedTime = true;
    } else if (!_hasCachedTime) {
      return false;
    }
    _lastPollMs = now != 0 ? now : 1;  // 0 doubles as the invalidation sentinel
  }
  localtime_r(&_cachedUtc, &out);
  return true;
}

bool HalClock::getTime(uint8_t& hour, uint8_t& minute) const {
  struct tm local;
  if (!localTime(local)) return false;
  hour = static_cast<uint8_t>(local.tm_hour);
  minute = static_cast<uint8_t>(local.tm_min);
  return true;
}

bool HalClock::formatTime(char* buf, size_t bufSize, bool use12Hour) const {
  if (bufSize < (use12Hour ? 9u : 6u)) return false;
  struct tm local;
  if (!localTime(local)) return false;

  if (use12Hour) {
    const bool pm = local.tm_hour >= 12;
    int hour12 = local.tm_hour % 12;
    if (hour12 == 0) hour12 = 12;
    snprintf(buf, bufSize, "%d:%02d %s", hour12, local.tm_min, pm ? "PM" : "AM");
  } else {
    snprintf(buf, bufSize, "%02d:%02d", local.tm_hour, local.tm_min);
  }
  return true;
}

bool HalClock::syncFromNTP() {
  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("CLK", "WiFi not connected, cannot sync NTP");
    return false;
  }

  LOG_INF("CLK", "Starting NTP sync...");
  // configTzTime overwrites the process TZ with UTC0 for the SNTP exchange;
  // remember the display timezone so it can be restored below.
  const char* tzBefore = getenv("TZ");
  char savedTz[64] = {0};
  if (tzBefore) snprintf(savedTz, sizeof(savedTz), "%s", tzBefore);
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
      const bool ok = _sdkRtc.set(dt);
      if (ok) {
        _cachedUtc = epochFromUtc(dt);
        _hasCachedTime = true;
        _lastPollMs = 0;
        LOG_INF("CLK", "RTC set to %04u-%02u-%02u %02u:%02u:%02u UTC", dt.year, dt.month, dt.day, dt.hour, dt.minute,
                dt.second);
      } else {
        // No DS3231 (X4) or the write failed. The SYSTEM clock is set either way, and that is what
        // reading-stats dates read via time(nullptr) -- so this is still a successful sync for us.
        LOG_INF("CLK", "System time set to %02d:%02d:%02d UTC (hardware RTC not written)", timeinfo.tm_hour,
                timeinfo.tm_min, timeinfo.tm_sec);
      }
      setTimezone(savedTz);
      // Deliberately not `ok`: a board with no RTC to write is still synced, and
      // WifiSelectionActivity latches clockHasBeenSynced on this result.
      return true;
    }
    delay(100);
  }

  LOG_ERR("CLK", "NTP sync timed out");
  setTimezone(savedTz);
  return false;
}

bool HalClock::systemTimeValid() { return time(nullptr) >= buildEpoch(); }

void HalClock::restoreSystemTime() const {
  if (systemTimeValid()) return;  // internal RTC timer survived (deep sleep / soft reset)

  time_t best = buildEpoch();
  char buf[24] = {};
  if (Storage.readFileToBuffer(clockStashPath().c_str(), buf, sizeof(buf) - 1) > 0) {
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
  // sdsystem::dir() creates the folder when neither spelling exists.
  char buf[24];
  snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(time(nullptr)));
  HalFile f;
  if (Storage.openFileForWrite("CLK", clockStashPath().c_str(), f)) {
    f.write(reinterpret_cast<const uint8_t*>(buf), strlen(buf));
  }
}
