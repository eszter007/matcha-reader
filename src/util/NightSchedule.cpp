#include "NightSchedule.h"

#include <cmath>

namespace nightschedule {
namespace {

// Double throughout: Julian dates are ~2.46e6, where a float step is a quarter of a day.
constexpr double J2000 = 2451545.0;
constexpr double UNIX_EPOCH_JD = 2440587.5;
constexpr double SECONDS_PER_DAY = 86400.0;
constexpr double DEG = 3.14159265358979323846 / 180.0;
// Upper limb at the horizon, corrected for refraction.
constexpr double SUN_ALTITUDE_DEG = -0.833;
constexpr double AXIAL_TILT_DEG = 23.4397;
constexpr int MINUTES_PER_DAY = 24 * 60;

time_t epochFromJulian(const double jd) {
  return static_cast<time_t>(std::llround((jd - UNIX_EPOCH_JD) * SECONDS_PER_DAY));
}

// Epoch of the latest past occurrence of local wall-clock minute `boundary`.
time_t lastOccurrence(const time_t now, const int minuteOfDay, const int second, const int boundary) {
  const int minutesAgo = ((minuteOfDay - boundary) % MINUTES_PER_DAY + MINUTES_PER_DAY) % MINUTES_PER_DAY;
  return now - static_cast<time_t>(minutesAgo) * 60 - second;
}

// Night fraction from one night [start, end]: full inside, ramping up before and down after.
int intervalPermille(const time_t now, const time_t start, const time_t end) {
  if (now >= start && now <= end) return FULL_NIGHT;
  const time_t gap = now < start ? start - now : now - end;
  if (gap >= RAMP_S) return 0;
  return static_cast<int>((RAMP_S - gap) * FULL_NIGHT / RAMP_S);
}

int clampLevel(const int value) { return value < 0 ? 0 : value > 100 ? 100 : value; }

}  // namespace

SolarDay solarDayAt(const time_t now, const float latitudeDeg, const float longitudeDeg) {
  const double lon = longitudeDeg;
  const double jd = static_cast<double>(now) / SECONDS_PER_DAY + UNIX_EPOCH_JD;
  // Mean solar noon k days after J2000 falls at J2000 + k - lon/360; take the nearest one.
  const double n = std::round(jd - J2000 + lon / 360.0);
  const double meanNoon = n - lon / 360.0;

  const double anomaly = std::fmod(357.5291 + 0.98560028 * meanNoon, 360.0) * DEG;
  const double center = 1.9148 * std::sin(anomaly) + 0.0200 * std::sin(2 * anomaly) + 0.0003 * std::sin(3 * anomaly);
  const double eclipticLon = std::fmod(anomaly / DEG + center + 180.0 + 102.9372, 360.0) * DEG;
  const double transit = J2000 + meanNoon + 0.0053 * std::sin(anomaly) - 0.0069 * std::sin(2 * eclipticLon);

  const double sinDecl = std::sin(eclipticLon) * std::sin(AXIAL_TILT_DEG * DEG);
  const double cosDecl = std::cos(std::asin(sinDecl));
  const double lat = static_cast<double>(latitudeDeg) * DEG;
  const double cosHourAngle = (std::sin(SUN_ALTITUDE_DEG * DEG) - std::sin(lat) * sinDecl) / (std::cos(lat) * cosDecl);

  SolarDay day;
  if (cosHourAngle >= 1.0) {
    day.kind = DayKind::PolarNight;
    return day;
  }
  if (cosHourAngle <= -1.0) {
    day.kind = DayKind::PolarDay;
    return day;
  }
  const double halfDay = std::acos(cosHourAngle) / DEG / 360.0;
  day.sunrise = epochFromJulian(transit - halfDay);
  day.sunset = epochFromJulian(transit + halfDay);
  return day;
}

bool inWindow(const int minuteOfDay, const int startMinute, const int endMinute) {
  if (startMinute == endMinute) return false;
  if (startMinute < endMinute) return minuteOfDay >= startMinute && minuteOfDay < endMinute;
  return minuteOfDay >= startMinute || minuteOfDay < endMinute;
}

Position windowPosition(const time_t now, const int minuteOfDay, const int second, const int startMinute,
                        const int endMinute) {
  if (startMinute == endMinute) return {Phase::Day, 0};
  if (inWindow(minuteOfDay, startMinute, endMinute)) {
    return {Phase::Night, lastOccurrence(now, minuteOfDay, second, startMinute)};
  }
  return {Phase::Day, lastOccurrence(now, minuteOfDay, second, endMinute)};
}

Position sunPosition(const time_t now, const float latitudeDeg, const float longitudeDeg) {
  const SolarDay day = solarDayAt(now, latitudeDeg, longitudeDeg);
  switch (day.kind) {
    case DayKind::PolarDay:
      return {Phase::Day, 0};
    case DayKind::PolarNight:
      return {Phase::Night, 0};
    case DayKind::Normal:
      break;
  }
  if (now >= day.sunset) return {Phase::Night, day.sunset};
  if (now >= day.sunrise) return {Phase::Day, day.sunrise};
  // Before dawn: the night began at the previous solar day's sunset.
  const SolarDay yesterday = solarDayAt(now - static_cast<time_t>(SECONDS_PER_DAY), latitudeDeg, longitudeDeg);
  return {Phase::Night, yesterday.kind == DayKind::Normal ? yesterday.sunset : 0};
}

int windowNightPermille(const time_t now, const int minuteOfDay, const int second, const int startMinute,
                        const int endMinute) {
  if (startMinute == endMinute) return 0;
  const time_t midnight = now - static_cast<time_t>(minuteOfDay) * 60 - second;
  const int lengthMinutes = ((endMinute - startMinute) % MINUTES_PER_DAY + MINUTES_PER_DAY) % MINUTES_PER_DAY;
  int best = 0;
  // Yesterday's night can still be ramping down and today's ramping up or running.
  for (int day = -1; day <= 1; day++) {
    const time_t start = midnight + static_cast<time_t>(day) * 86400 + static_cast<time_t>(startMinute) * 60;
    const int p = intervalPermille(now, start, start + static_cast<time_t>(lengthMinutes) * 60);
    if (p > best) best = p;
  }
  return best;
}

int sunNightPermille(const time_t now, const float latitudeDeg, const float longitudeDeg) {
  const auto day = static_cast<time_t>(SECONDS_PER_DAY);
  const SolarDay today = solarDayAt(now, latitudeDeg, longitudeDeg);
  if (today.kind == DayKind::PolarNight) return FULL_NIGHT;
  if (today.kind == DayKind::PolarDay) return 0;
  const SolarDay yesterday = solarDayAt(now - day, latitudeDeg, longitudeDeg);
  const SolarDay tomorrow = solarDayAt(now + day, latitudeDeg, longitudeDeg);
  // Next to a polar night, the night on that side has no sunset or sunrise: it runs on.
  constexpr time_t FAR = 2 * 86400;
  int best = 0;
  if (yesterday.kind != DayKind::PolarDay) {
    const time_t start = yesterday.kind == DayKind::Normal ? yesterday.sunset : today.sunrise - FAR;
    best = intervalPermille(now, start, today.sunrise);
  }
  if (tomorrow.kind != DayKind::PolarDay) {
    const time_t end = tomorrow.kind == DayKind::Normal ? tomorrow.sunrise : today.sunset + FAR;
    const int p = intervalPermille(now, today.sunset, end);
    if (p > best) best = p;
  }
  return best;
}

uint8_t blendLevel(const uint8_t day, const uint8_t night, const int permille) {
  const int delta = (static_cast<int>(night) - day) * permille;
  return static_cast<uint8_t>(day + (delta >= 0 ? delta + FULL_NIGHT / 2 : delta - FULL_NIGHT / 2) / FULL_NIGHT);
}

uint8_t levelForShown(const uint8_t shown, const uint8_t other, const int permille, const bool night) {
  const int weight = night ? permille : FULL_NIGHT - permille;
  if (weight <= 0) return shown;
  // shown = other + (edited - other) * weight / 1000, solved for edited.
  const int scaled = (static_cast<int>(shown) - other) * FULL_NIGHT;
  const int edited = other + (scaled >= 0 ? scaled + weight / 2 : scaled - weight / 2) / weight;
  return static_cast<uint8_t>(clampLevel(edited));
}

Action decide(const Position now, const Phase lastPhase, const time_t lastSince, const bool edited) {
  Action action;
  if (edited && now.phase == lastPhase) {
    action.record = now.since != lastSince;
    return action;
  }
  const bool newBoundary = now.since != 0 && now.since >= lastSince + MIN_EDGE_GAP_S;
  if (now.phase == lastPhase && !newBoundary) return action;
  action.record = true;
  if (now.phase == Phase::Night) {
    action.switchLight = true;
    action.lightOn = true;
  } else if (now.phase == Phase::Day && (lastPhase == Phase::Night || (lastPhase == Phase::Day && newBoundary))) {
    action.switchLight = true;
    action.lightOn = false;
  }
  return action;
}

}  // namespace nightschedule
