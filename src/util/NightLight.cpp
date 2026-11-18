#include "NightLight.h"

#include <HalClock.h>
#include <HalFrontlight.h>
#include <I18n.h>
#include <Logging.h>

#include <cstdio>
#include <ctime>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "NightSchedule.h"
#include "SlideSwitch.h"

namespace nightlight {

using nightschedule::Phase;
using nightschedule::Position;

namespace {

void formatClock(const int hour, const int minute, char* buf, const size_t size) {
  if (SETTINGS.clockFormat == 1) {
    const int hour12 = hour % 12 == 0 ? 12 : hour % 12;
    snprintf(buf, size, "%d:%02d %s", hour12, minute, hour >= 12 ? "PM" : "AM");
  } else {
    snprintf(buf, size, "%02d:%02d", hour, minute);
  }
}

Phase lastPhase() {
  const uint8_t stored = APP_STATE.nightLightPhase;
  return stored <= static_cast<uint8_t>(Phase::Night) ? static_cast<Phase>(stored) : Phase::Off;
}

}  // namespace

void formatHalfHour(const uint8_t halfHour, char* buf, const size_t size) {
  const uint8_t wrapped = halfHour % HALF_HOURS_PER_DAY;
  formatClock(wrapped / 2, (wrapped % 2) * 30, buf, size);
}

bool nightActive() { return lastPhase() == Phase::Night; }

void saveLevels(const uint8_t brightness, const uint8_t warmth, const bool night) {
  (night ? SETTINGS.nightLightBrightness : SETTINGS.frontlightBrightness) = brightness;
  (night ? SETTINGS.nightLightWarmth : SETTINGS.frontlightWarmth) = warmth;
}

#if FREEINK_CAP_FRONTLIGHT

namespace {

// London first: it is the default. The rest run west to east. APPEND ONLY (see header).
constexpr City CITIES[] = {
    {"London", 5151, -13},
    {"Honolulu", 2131, -15786},
    {"Anchorage", 6122, -14990},
    {"Vancouver", 4928, -12312},
    {"Seattle", 4761, -12233},
    {"San Francisco", 3777, -12242},
    {"Los Angeles", 3405, -11824},
    {"Phoenix", 3345, -11207},
    {"Denver", 3974, -10499},
    {"Mexico City", 1943, -9913},
    {"Dallas", 3278, -9680},
    {"Chicago", 4188, -8763},
    {"Atlanta", 3375, -8439},
    {"Toronto", 4365, -7938},
    {"Montreal", 4550, -7357},
    {"New York", 4071, -7401},
    {"Bogota", 471, -7407},
    {"Lima", -1205, -7704},
    {"Halifax", 4465, -6357},
    {"Santiago", -3345, -7067},
    {"Buenos Aires", -3460, -5838},
    {"Sao Paulo", -2355, -4663},
    {"Rio de Janeiro", -2291, -4317},
    {"Reykjavik", 6415, -2194},
    {"Lisbon", 3872, -914},
    {"Dublin", 5335, -626},
    {"Edinburgh", 5595, -319},
    {"Manchester", 5348, -224},
    {"Madrid", 4042, -370},
    {"Paris", 4886, 235},
    {"Brussels", 5085, 435},
    {"Amsterdam", 5237, 490},
    {"Zurich", 4738, 854},
    {"Oslo", 5991, 1075},
    {"Copenhagen", 5568, 1257},
    {"Berlin", 5252, 1340},
    {"Rome", 4190, 1250},
    {"Vienna", 4821, 1637},
    {"Stockholm", 5933, 1807},
    {"Warsaw", 5223, 2101},
    {"Helsinki", 6017, 2494},
    {"Athens", 3798, 2373},
    {"Istanbul", 4101, 2898},
    {"Kyiv", 5045, 3052},
    {"Cairo", 3004, 3124},
    {"Lagos", 652, 338},
    {"Johannesburg", -2620, 2805},
    {"Nairobi", -129, 3682},
    {"Moscow", 5576, 3762},
    {"Tehran", 3569, 5139},
    {"Dubai", 2520, 5527},
    {"Karachi", 2486, 6701},
    {"Mumbai", 1908, 7288},
    {"Delhi", 2861, 7721},
    {"Bangalore", 1297, 7759},
    {"Dhaka", 2381, 9041},
    {"Bangkok", 1376, 10050},
    {"Singapore", 135, 10382},
    {"Jakarta", -621, 10685},
    {"Hong Kong", 2232, 11417},
    {"Manila", 1460, 12098},
    {"Shanghai", 3123, 12147},
    {"Beijing", 3990, 11641},
    {"Taipei", 2503, 12157},
    {"Seoul", 3757, 12698},
    {"Fukuoka", 3359, 13040},
    {"Osaka", 3469, 13550},
    {"Tokyo", 3568, 13969},
    {"Sapporo", 4306, 14135},
    {"Perth", -3195, 11586},
    {"Adelaide", -3493, 13860},
    {"Brisbane", -2747, 15303},
    {"Sydney", -3387, 15121},
    {"Melbourne", -3781, 14496},
    {"Auckland", -3685, 17476},
};
static_assert(sizeof(CITIES) / sizeof(CITIES[0]) == CITY_COUNT, "update CITY_COUNT with the table");

constexpr unsigned long CHECK_INTERVAL_MS = 20000;

float toDegrees(const int16_t e2) { return static_cast<float>(e2) / 100.0f; }

// Where the schedule stands now. False when the schedule is on but there is no clock to place it
// by: nothing is decided or recorded then, so the next reading starts from the last real one.
bool currentPosition(Position& out) {
  const uint8_t mode = SETTINGS.nightLightMode;
  if (mode != CrossPointSettings::NIGHT_LIGHT_CUSTOM && mode != CrossPointSettings::NIGHT_LIGHT_SUN) {
    out = {Phase::Off, 0};
    return true;
  }
  struct tm local;
  if (!halClock.localTime(local)) return false;
  const int minute = local.tm_hour * 60 + local.tm_min;
  const int second = local.tm_sec;
  // localTime() renders the clock under the active TZ rule, so mktime() gives back its UTC epoch.
  const time_t now = mktime(&local);
  if (mode == CrossPointSettings::NIGHT_LIGHT_CUSTOM) {
    out =
        nightschedule::windowPosition(now, minute, second, SETTINGS.nightLightStart * 30, SETTINGS.nightLightEnd * 30);
  } else {
    const City& c = city(SETTINGS.nightLightCity);
    out = nightschedule::sunPosition(now, toDegrees(c.latE2), toDegrees(c.lonE2));
  }
  return true;
}

void applyLevels(const Phase phase) {
  if (phase == Phase::Night) {
    Frontlight.setBrightness(SETTINGS.nightLightBrightness);
    Frontlight.setWarmth(SETTINGS.nightLightWarmth);
  } else {
    Frontlight.setBrightness(SETTINGS.frontlightBrightness);
    Frontlight.setWarmth(SETTINGS.frontlightWarmth);
  }
}

void record(const Position now) {
  APP_STATE.nightLightPhase = static_cast<uint8_t>(now.phase);
  APP_STATE.nightLightEdge = static_cast<uint32_t>(now.since);
  APP_STATE.saveToFile();
}

// The saved preference follows the light, so Restore Light on Wake brings back what the schedule
// chose. True when it changed.
bool rememberLight(const bool on) {
  if ((SETTINGS.frontlightOn != 0) == on) return false;
  SETTINGS.frontlightOn = on ? 1 : 0;
  return true;
}

struct Outcome {
  bool switched = false;
  bool settingsChanged = false;
};

// Acts on the current position: levels when the night levels go on or come off (or always, after
// a setting edit), the light on an edge, then the record.
Outcome act(const Position now, const bool edited) {
  const Phase previous = lastPhase();
  const nightschedule::Action action = nightschedule::decide(now, previous, APP_STATE.nightLightEdge, edited);
  const bool levelsChange = edited || (action.record && (now.phase == Phase::Night || previous == Phase::Night));
  Outcome outcome;
  outcome.switched = action.switchLight && !slideSwitchOwnsLight();
  // Off before the levels change and on after, so the other pair never shows.
  if (outcome.switched && !action.lightOn) Frontlight.setOn(false);
  if (levelsChange) applyLevels(now.phase);
  if (outcome.switched && action.lightOn) Frontlight.setOn(true);
  if (outcome.switched) {
    outcome.settingsChanged = rememberLight(action.lightOn);
    LOG_INF("NLIGHT", "Night window %s: light %s", action.lightOn ? "opened" : "closed", action.lightOn ? "on" : "off");
  }
  if (action.record) record(now);
  return outcome;
}

void formatEpoch(const time_t epoch, char* buf, const size_t size) {
  struct tm local;
  localtime_r(&epoch, &local);
  formatClock(local.tm_hour, local.tm_min, buf, size);
}

}  // namespace

const City& city(const uint8_t index) { return CITIES[index < CITY_COUNT ? index : DEFAULT_CITY]; }

void adjustBootState(uint8_t& brightness, uint8_t& warmth, bool& lightOn) {
  Position now;
  if (!currentPosition(now)) {
    // No clock to check against: stay with the levels last applied, so the rest of a night is
    // not spent at the daytime levels once the clock reads again.
    if (lastPhase() == Phase::Night) {
      brightness = SETTINGS.nightLightBrightness;
      warmth = SETTINGS.nightLightWarmth;
    }
    return;
  }
  if (now.phase == Phase::Night) {
    brightness = SETTINGS.nightLightBrightness;
    warmth = SETTINGS.nightLightWarmth;
  }
  const nightschedule::Action action = nightschedule::decide(now, lastPhase(), APP_STATE.nightLightEdge);
  if (action.switchLight && !slideSwitchOwnsLight()) {
    lightOn = action.lightOn;
    if (rememberLight(lightOn)) SETTINGS.saveToFile();
    LOG_INF("NLIGHT", "Night window %s since last check: light %s", lightOn ? "opened" : "closed",
            lightOn ? "on" : "off");
  }
  if (action.record) record(now);
}

bool tick() {
  if (!Frontlight.present()) return false;
  static unsigned long lastCheckMs = 0;
  const unsigned long nowMs = millis();
  if (nowMs - lastCheckMs < CHECK_INTERVAL_MS) return false;
  lastCheckMs = nowMs;

  Position now;
  if (!currentPosition(now)) return false;
  const Outcome outcome = act(now, false);
  if (outcome.settingsChanged) SETTINGS.saveToFile();
  return outcome.switched;
}

bool refresh() {
  if (!Frontlight.present()) return false;
  Position now;
  if (!currentPosition(now)) return false;
  return act(now, true).settingsChanged;
}

void formatSummary(char* buf, const size_t size) {
  char from[12];
  char to[12];
  switch (SETTINGS.nightLightMode) {
    case CrossPointSettings::NIGHT_LIGHT_CUSTOM:
      formatHalfHour(SETTINGS.nightLightStart, from, sizeof(from));
      formatHalfHour(SETTINGS.nightLightEnd, to, sizeof(to));
      snprintf(buf, size, "%s–%s", from, to);
      return;
    case CrossPointSettings::NIGHT_LIGHT_SUN: {
      struct tm local;
      if (halClock.localTime(local)) {
        const time_t now = mktime(&local);
        const City& c = city(SETTINGS.nightLightCity);
        const float lat = toDegrees(c.latE2);
        const float lon = toDegrees(c.lonE2);
        const nightschedule::SolarDay today = nightschedule::solarDayAt(now, lat, lon);
        if (today.kind == nightschedule::DayKind::Normal) {
          // The night in progress or the next one: before dawn it began at yesterday's sunset.
          const bool beforeDawn = now < today.sunrise;
          const nightschedule::SolarDay other =
              nightschedule::solarDayAt(now + (beforeDawn ? -86400 : 86400), lat, lon);
          if (other.kind == nightschedule::DayKind::Normal) {
            formatEpoch(beforeDawn ? other.sunset : today.sunset, from, sizeof(from));
            formatEpoch(beforeDawn ? today.sunrise : other.sunrise, to, sizeof(to));
            snprintf(buf, size, "%s–%s", from, to);
            return;
          }
        }
      }
      snprintf(buf, size, "%s", tr(STR_SCHEDULE_SUNSET_SUNRISE));
      return;
    }
    default:
      snprintf(buf, size, "%s", tr(STR_STATE_OFF));
      return;
  }
}

#else  // No frontlight: the schedule has nothing to drive and its settings rows are not built.

const City& city(uint8_t) {
  static constexpr City NONE = {"", 0, 0};
  return NONE;
}
void adjustBootState(uint8_t&, uint8_t&, bool&) {}
bool tick() { return false; }
bool refresh() { return false; }
void formatSummary(char* buf, const size_t size) { snprintf(buf, size, "%s", tr(STR_STATE_OFF)); }

#endif

}  // namespace nightlight
