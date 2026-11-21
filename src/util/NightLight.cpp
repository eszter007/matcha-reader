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
#include "Timezones.h"

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

// How far the levels last applied were toward night, in permille; set by every check.
int appliedPermille = 0;
bool levelsHeld = false;

Phase lastPhase() {
  const uint8_t stored = APP_STATE.nightLightPhase;
  return stored <= static_cast<uint8_t>(Phase::Night) ? static_cast<Phase>(stored) : Phase::Off;
}

}  // namespace

void formatHalfHour(const uint8_t halfHour, char* buf, const size_t size) {
  const uint8_t wrapped = halfHour % HALF_HOURS_PER_DAY;
  formatClock(wrapped / 2, (wrapped % 2) * 30, buf, size);
}

bool nightActive() { return appliedPermille >= nightschedule::FULL_NIGHT / 2; }

void holdLevels(const bool hold) { levelsHeld = hold; }

void saveLevels(const uint8_t brightness, const uint8_t warmth, const bool night) {
  if (SETTINGS.nightLightMode == CrossPointSettings::NIGHT_LIGHT_OFF) {
    SETTINGS.frontlightBrightness = brightness;
    SETTINGS.frontlightWarmth = warmth;
    return;
  }
  // Mid-ramp, store the value that keeps the light where it was left.
  const int p = appliedPermille;
  if (night) {
    SETTINGS.nightLightBrightness = nightschedule::levelForShown(brightness, SETTINGS.frontlightBrightness, p, true);
    SETTINGS.nightLightWarmth = nightschedule::levelForShown(warmth, SETTINGS.frontlightWarmth, p, true);
  } else {
    SETTINGS.frontlightBrightness = nightschedule::levelForShown(brightness, SETTINGS.nightLightBrightness, p, false);
    SETTINGS.frontlightWarmth = nightschedule::levelForShown(warmth, SETTINGS.nightLightWarmth, p, false);
  }
}

#if FREEINK_CAP_FRONTLIGHT

namespace {

constexpr unsigned long CHECK_INTERVAL_MS = 20000;

struct Reading {
  Position position;
  int permille = 0;  // how far the levels are toward night
};

// Where the schedule stands now. False when the schedule is on but there is no clock to place it
// by: nothing is decided or recorded then, so the next reading starts from the last real one.
bool currentReading(Reading& out) {
  const uint8_t mode = SETTINGS.nightLightMode;
  if (mode != CrossPointSettings::NIGHT_LIGHT_CUSTOM && mode != CrossPointSettings::NIGHT_LIGHT_SUN) {
    out = {{Phase::Off, 0}, 0};
    return true;
  }
  struct tm local;
  if (!halClock.localTime(local)) return false;
  const int minute = local.tm_hour * 60 + local.tm_min;
  const int second = local.tm_sec;
  // localTime() renders the clock under the active TZ rule, so mktime() gives back its UTC epoch.
  const time_t now = mktime(&local);
  if (mode == CrossPointSettings::NIGHT_LIGHT_CUSTOM) {
    const int start = SETTINGS.nightLightStart * 30;
    const int end = SETTINGS.nightLightEnd * 30;
    out.position = nightschedule::windowPosition(now, minute, second, start, end);
    out.permille = nightschedule::windowNightPermille(now, minute, second, start, end);
  } else {
    float lat;
    float lon;
    timezones::location(timezones::activeIndex(), lat, lon);
    out.position = nightschedule::sunPosition(now, lat, lon);
    out.permille = nightschedule::sunNightPermille(now, lat, lon);
  }
  return true;
}

uint8_t blendedBrightness(const int permille) {
  return nightschedule::blendLevel(SETTINGS.frontlightBrightness, SETTINGS.nightLightBrightness, permille);
}
uint8_t blendedWarmth(const int permille) {
  return nightschedule::blendLevel(SETTINGS.frontlightWarmth, SETTINGS.nightLightWarmth, permille);
}

void applyLevels(const int permille) {
  appliedPermille = permille;
  const uint8_t brightness = blendedBrightness(permille);
  const uint8_t warmth = blendedWarmth(permille);
  if (Frontlight.brightness() != brightness) Frontlight.setBrightness(brightness);
  if (Frontlight.warmth() != warmth) Frontlight.setWarmth(warmth);
}

// Switching at the night's edges is optional (Switch Light On and Off).
bool scheduleSwitches() { return SETTINGS.nightLightSwitch != 0 && !slideSwitchOwnsLight(); }

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

// Acts on the current reading: the levels (unless the panel holds them), the light on an edge, then
// the record.
Outcome act(const Reading& reading, const bool edited) {
  const Position now = reading.position;
  const nightschedule::Action action = nightschedule::decide(now, lastPhase(), APP_STATE.nightLightEdge, edited);
  Outcome outcome;
  outcome.switched = action.switchLight && scheduleSwitches();
  if (outcome.switched && !action.lightOn) Frontlight.setOn(false);
  // With the schedule off, the levels are the panel's; set them only when the schedule changes.
  const bool scheduled = SETTINGS.nightLightMode != CrossPointSettings::NIGHT_LIGHT_OFF;
  if (!levelsHeld && (scheduled || edited)) applyLevels(reading.permille);
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

void adjustBootState(uint8_t& brightness, uint8_t& warmth, bool& lightOn) {
  Reading reading;
  if (!currentReading(reading)) {
    // No clock to check against: stay at night levels if the last check was in the night, so the
    // rest of it is not spent at the daytime levels.
    if (lastPhase() == Phase::Night) reading.permille = nightschedule::FULL_NIGHT;
    appliedPermille = reading.permille;
    brightness = blendedBrightness(reading.permille);
    warmth = blendedWarmth(reading.permille);
    return;
  }
  appliedPermille = reading.permille;
  brightness = blendedBrightness(reading.permille);
  warmth = blendedWarmth(reading.permille);
  const Position now = reading.position;
  const nightschedule::Action action = nightschedule::decide(now, lastPhase(), APP_STATE.nightLightEdge);
  if (action.switchLight && scheduleSwitches()) {
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

  Reading reading;
  if (!currentReading(reading)) return false;
  const Outcome outcome = act(reading, false);
  if (outcome.settingsChanged) SETTINGS.saveToFile();
  return outcome.switched;
}

bool refresh() {
  if (!Frontlight.present()) return false;
  Reading reading;
  if (!currentReading(reading)) return false;
  return act(reading, true).settingsChanged;
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
        float lat;
        float lon;
        timezones::location(timezones::activeIndex(), lat, lon);
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

void adjustBootState(uint8_t&, uint8_t&, bool&) {}
bool tick() { return false; }
bool refresh() { return false; }
void formatSummary(char* buf, const size_t size) { snprintf(buf, size, "%s", tr(STR_STATE_OFF)); }

#endif

}  // namespace nightlight
