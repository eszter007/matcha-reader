#include <gtest/gtest.h>

#include <cstdlib>
#include <ctime>

#include "NightSchedule.h"

namespace {

using nightschedule::Action;
using nightschedule::DayKind;
using nightschedule::Phase;
using nightschedule::Position;

// UTC epoch for a calendar date and time, independent of the host's TZ.
time_t utc(int year, int month, int day, int hour, int minute) {
  struct tm t = {};
  t.tm_year = year - 1900;
  t.tm_mon = month - 1;
  t.tm_mday = day;
  t.tm_hour = hour;
  t.tm_min = minute;
  return timegm(&t);
}

constexpr float LONDON_LAT = 51.51f;
constexpr float LONDON_LON = -0.13f;

// Published sunrise/sunset times are rounded to the minute; the simplified equation adds about one more.
constexpr long TOLERANCE_S = 3 * 60;

void expectNear(time_t actual, time_t expected) {
  EXPECT_LE(std::labs(static_cast<long>(actual - expected)), TOLERANCE_S);
}

TEST(SolarDay, LondonMidsummer) {
  const auto day = nightschedule::solarDayAt(utc(2026, 6, 21, 12, 0), LONDON_LAT, LONDON_LON);
  ASSERT_EQ(day.kind, DayKind::Normal);
  expectNear(day.sunrise, utc(2026, 6, 21, 3, 43));
  expectNear(day.sunset, utc(2026, 6, 21, 20, 21));
}

TEST(SolarDay, LondonMidwinter) {
  const auto day = nightschedule::solarDayAt(utc(2026, 12, 21, 12, 0), LONDON_LAT, LONDON_LON);
  ASSERT_EQ(day.kind, DayKind::Normal);
  expectNear(day.sunrise, utc(2026, 12, 21, 8, 4));
  expectNear(day.sunset, utc(2026, 12, 21, 15, 54));
}

// Late evening still belongs to the same solar day, so its sunset is the one that just passed.
TEST(SolarDay, LateEveningPicksTodaysSunset) {
  const auto day = nightschedule::solarDayAt(utc(2026, 11, 4, 23, 30), LONDON_LAT, LONDON_LON);
  ASSERT_EQ(day.kind, DayKind::Normal);
  expectNear(day.sunrise, utc(2026, 11, 4, 6, 58));
  expectNear(day.sunset, utc(2026, 11, 4, 16, 29));
}

// Far from Greenwich the local day straddles two UTC dates.
TEST(SolarDay, TokyoDayStraddlesUtcMidnight) {
  const auto day = nightschedule::solarDayAt(utc(2026, 11, 4, 3, 0), 35.68f, 139.69f);
  ASSERT_EQ(day.kind, DayKind::Normal);
  expectNear(day.sunrise, utc(2026, 11, 3, 21, 4));
  expectNear(day.sunset, utc(2026, 11, 4, 7, 44));
}

TEST(SolarDay, SouthernHemisphereSummer) {
  const auto day = nightschedule::solarDayAt(utc(2026, 1, 1, 2, 0), -33.87f, 151.21f);
  ASSERT_EQ(day.kind, DayKind::Normal);
  expectNear(day.sunrise, utc(2025, 12, 31, 18, 47));
  expectNear(day.sunset, utc(2026, 1, 1, 9, 9));
}

TEST(SunPosition, PolarDayAndNightHaveNoBoundary) {
  const Position winter = nightschedule::sunPosition(utc(2026, 12, 21, 11, 0), 69.65f, 18.96f);
  EXPECT_EQ(winter.phase, Phase::Night);
  EXPECT_EQ(winter.since, 0);
  const Position summer = nightschedule::sunPosition(utc(2026, 6, 21, 23, 0), 69.65f, 18.96f);
  EXPECT_EQ(summer.phase, Phase::Day);
  EXPECT_EQ(summer.since, 0);
}

TEST(SunPosition, LondonEveningNightAndMorning) {
  const time_t sunset = nightschedule::solarDayAt(utc(2026, 11, 4, 12, 0), LONDON_LAT, LONDON_LON).sunset;
  const time_t sunrise = nightschedule::solarDayAt(utc(2026, 11, 5, 12, 0), LONDON_LAT, LONDON_LON).sunrise;

  EXPECT_EQ(nightschedule::sunPosition(utc(2026, 11, 4, 16, 20), LONDON_LAT, LONDON_LON).phase, Phase::Day);
  const Position evening = nightschedule::sunPosition(utc(2026, 11, 4, 16, 40), LONDON_LAT, LONDON_LON);
  EXPECT_EQ(evening.phase, Phase::Night);
  EXPECT_EQ(evening.since, sunset);
  // After midnight the night still began at the previous evening's sunset.
  const Position small_hours = nightschedule::sunPosition(utc(2026, 11, 5, 2, 0), LONDON_LAT, LONDON_LON);
  EXPECT_EQ(small_hours.phase, Phase::Night);
  EXPECT_EQ(small_hours.since, sunset);
  const Position morning = nightschedule::sunPosition(utc(2026, 11, 5, 7, 10), LONDON_LAT, LONDON_LON);
  EXPECT_EQ(morning.phase, Phase::Day);
  EXPECT_EQ(morning.since, sunrise);
}

TEST(Window, WrapsPastMidnight) {
  constexpr int start = 20 * 60;
  constexpr int end = 6 * 60;
  EXPECT_TRUE(nightschedule::inWindow(20 * 60, start, end));
  EXPECT_TRUE(nightschedule::inWindow(23 * 60 + 59, start, end));
  EXPECT_TRUE(nightschedule::inWindow(0, start, end));
  EXPECT_TRUE(nightschedule::inWindow(5 * 60 + 59, start, end));
  EXPECT_FALSE(nightschedule::inWindow(6 * 60, start, end));
  EXPECT_FALSE(nightschedule::inWindow(12 * 60, start, end));
  EXPECT_FALSE(nightschedule::inWindow(19 * 60 + 59, start, end));
}

TEST(Window, WithinOneDayAndEmpty) {
  EXPECT_TRUE(nightschedule::inWindow(13 * 60, 12 * 60, 14 * 60));
  EXPECT_FALSE(nightschedule::inWindow(14 * 60, 12 * 60, 14 * 60));
  EXPECT_FALSE(nightschedule::inWindow(11 * 60, 12 * 60, 14 * 60));
  EXPECT_FALSE(nightschedule::inWindow(12 * 60, 12 * 60, 12 * 60));
}

// Boundaries are the latest past start or end time, measured back from `now` on the wall clock.
TEST(Window, PositionPointsAtTheBoundaryThatBeganIt) {
  const time_t now = utc(2026, 11, 5, 1, 30);  // treat UTC as the wall clock here
  const Position night = nightschedule::windowPosition(now, 90, 0, 20 * 60, 6 * 60);
  EXPECT_EQ(night.phase, Phase::Night);
  EXPECT_EQ(night.since, utc(2026, 11, 4, 20, 0));

  const time_t noon = utc(2026, 11, 5, 12, 0) + 15;
  const Position day = nightschedule::windowPosition(noon, 12 * 60, 15, 20 * 60, 6 * 60);
  EXPECT_EQ(day.phase, Phase::Day);
  EXPECT_EQ(day.since, utc(2026, 11, 5, 6, 0));

  EXPECT_EQ(nightschedule::windowPosition(noon, 12 * 60, 15, 600, 600).since, 0);
}

Action act(Position now, Phase lastPhase, time_t lastSince, bool edited = false) {
  return nightschedule::decide(now, lastPhase, lastSince, edited);
}

TEST(Decide, NightfallTurnsTheLightOn) {
  const Action a = act({Phase::Night, 2000}, Phase::Day, 1000);
  EXPECT_TRUE(a.record);
  EXPECT_TRUE(a.switchLight);
  EXPECT_TRUE(a.lightOn);
}

TEST(Decide, MorningTurnsTheLightOff) {
  const Action a = act({Phase::Day, 3000}, Phase::Night, 2000);
  EXPECT_TRUE(a.record);
  EXPECT_TRUE(a.switchLight);
  EXPECT_FALSE(a.lightOn);
}

// Between edges the light is the user's: nothing to do.
TEST(Decide, SamePositionDoesNothing) {
  const Action a = act({Phase::Night, 2000}, Phase::Night, 2000);
  EXPECT_FALSE(a.record);
  EXPECT_FALSE(a.switchLight);
}

TEST(Decide, SwitchingTheScheduleOnByDayLeavesTheLightAlone) {
  const Action a = act({Phase::Day, 3000}, Phase::Off, 0);
  EXPECT_TRUE(a.record);
  EXPECT_FALSE(a.switchLight);
}

TEST(Decide, SwitchingTheScheduleOnAtNightTurnsTheLightOn) {
  const Action a = act({Phase::Night, 2000}, Phase::Off, 0);
  EXPECT_TRUE(a.switchLight);
  EXPECT_TRUE(a.lightOn);
}

// Asleep from one night to the next: a new nightfall even though the phase is unchanged.
TEST(Decide, SleepingThroughADayStillOpensTheNextNight) {
  const Action a = act({Phase::Night, 2000 + 86400}, Phase::Night, 2000);
  EXPECT_TRUE(a.switchLight);
  EXPECT_TRUE(a.lightOn);
}

// Asleep from one day to the next: the night in between came and went.
TEST(Decide, SleepingThroughANightClosesIt) {
  const Action a = act({Phase::Day, 3000 + 86400}, Phase::Day, 3000);
  EXPECT_TRUE(a.switchLight);
  EXPECT_FALSE(a.lightOn);
}

// Moving the start earlier during a night must not re-open the night the user already had.
TEST(Decide, AnOlderBoundaryIsNotANewEdge) {
  const Action a = act({Phase::Night, 1500}, Phase::Night, 2000);
  EXPECT_FALSE(a.record);
  EXPECT_FALSE(a.switchLight);
}

TEST(Decide, TurningTheScheduleOffNeverSwitchesTheLight) {
  const Action a = act({Phase::Off, 0}, Phase::Night, 2000);
  EXPECT_TRUE(a.record);
  EXPECT_FALSE(a.switchLight);
}

// The clocks going back an hour mid-window moves the computed start an hour later: same night.
TEST(Decide, AClockChangeIsNotANewEdge) {
  EXPECT_FALSE(act({Phase::Night, 2000 + 3600}, Phase::Night, 2000).record);
  EXPECT_FALSE(act({Phase::Day, 3000 + 3600}, Phase::Day, 3000).switchLight);
}

// Editing Light Off At by day (06:00 -> 07:00) moves the boundary later but is not a morning.
TEST(Decide, EditingWithinThePhaseRecordsWithoutSwitching) {
  const Action day = act({Phase::Day, 3000 + 3600}, Phase::Day, 3000, true);
  EXPECT_TRUE(day.record);
  EXPECT_FALSE(day.switchLight);
  const Action night = act({Phase::Night, 2000 + 86400}, Phase::Night, 2000, true);
  EXPECT_FALSE(night.switchLight);
}

// An edit that moves "now" into the night still opens it.
TEST(Decide, EditingIntoTheNightOpensIt) {
  const Action a = act({Phase::Night, 4000}, Phase::Day, 3000, true);
  EXPECT_TRUE(a.switchLight);
  EXPECT_TRUE(a.lightOn);
}

TEST(Decide, PolarNightOpensOnceOnTheChangeOfPhase) {
  EXPECT_TRUE(act({Phase::Night, 0}, Phase::Day, 3000).lightOn);
  EXPECT_FALSE(act({Phase::Night, 0}, Phase::Night, 0).record);
}

}  // namespace

namespace {

// 2026-11-04 local = UTC (London in winter); window 20:00-06:00.
constexpr int START = 20 * 60;
constexpr int END = 6 * 60;

int windowAt(int hour, int minute) {
  const time_t now = utc(2026, 11, 4, hour, minute);
  return nightschedule::windowNightPermille(now, hour * 60 + minute, 0, START, END);
}

}  // namespace

TEST(Ramp, WindowFullInsideAndDayOutside) {
  EXPECT_EQ(windowAt(23, 0), nightschedule::FULL_NIGHT);
  EXPECT_EQ(windowAt(3, 0), nightschedule::FULL_NIGHT);
  EXPECT_EQ(windowAt(12, 0), 0);
  EXPECT_EQ(windowAt(18, 59), 0);
  EXPECT_EQ(windowAt(7, 0), 0);
}

TEST(Ramp, WindowRampsBeforeStartAndAfterEnd) {
  EXPECT_EQ(windowAt(19, 30), 500);
  EXPECT_EQ(windowAt(19, 45), 750);
  EXPECT_EQ(windowAt(20, 0), nightschedule::FULL_NIGHT);
  EXPECT_EQ(windowAt(6, 0), nightschedule::FULL_NIGHT);
  EXPECT_EQ(windowAt(6, 30), 500);
}

TEST(Ramp, EmptyWindowIsDay) {
  const time_t now = utc(2026, 11, 4, 20, 0);
  EXPECT_EQ(nightschedule::windowNightPermille(now, 20 * 60, 0, START, START), 0);
}

TEST(Ramp, DaytimeWindowDoesNotWrap) {
  // 13:00-15:00: full at 14:00, half at 15:30, day at 23:00.
  const auto at = [](int h, int m) {
    return nightschedule::windowNightPermille(utc(2026, 11, 4, h, m), h * 60 + m, 0, 13 * 60, 15 * 60);
  };
  EXPECT_EQ(at(14, 0), nightschedule::FULL_NIGHT);
  EXPECT_EQ(at(15, 30), 500);
  EXPECT_EQ(at(23, 0), 0);
}

TEST(Ramp, SunFollowsSunsetAndSunrise) {
  // London, 2026-11-04: sunset ~16:29, sunrise ~06:58 UTC.
  EXPECT_EQ(nightschedule::sunNightPermille(utc(2026, 11, 4, 12, 0), LONDON_LAT, LONDON_LON), 0);
  EXPECT_EQ(nightschedule::sunNightPermille(utc(2026, 11, 4, 22, 0), LONDON_LAT, LONDON_LON),
            nightschedule::FULL_NIGHT);
  EXPECT_EQ(nightschedule::sunNightPermille(utc(2026, 11, 5, 3, 0), LONDON_LAT, LONDON_LON), nightschedule::FULL_NIGHT);
  const int beforeSunset = nightschedule::sunNightPermille(utc(2026, 11, 4, 16, 0), LONDON_LAT, LONDON_LON);
  EXPECT_GT(beforeSunset, 400);
  EXPECT_LT(beforeSunset, 600);
  const int afterSunrise = nightschedule::sunNightPermille(utc(2026, 11, 5, 7, 30), LONDON_LAT, LONDON_LON);
  EXPECT_GT(afterSunrise, 400);
  EXPECT_LT(afterSunrise, 600);
}

TEST(Ramp, PolarNightAndDay) {
  EXPECT_EQ(nightschedule::sunNightPermille(utc(2026, 12, 21, 12, 0), 78.2f, 15.6f), nightschedule::FULL_NIGHT);
  EXPECT_EQ(nightschedule::sunNightPermille(utc(2026, 6, 21, 0, 0), 78.2f, 15.6f), 0);
}

TEST(Blend, EndsAndMiddle) {
  EXPECT_EQ(nightschedule::blendLevel(80, 30, 0), 80);
  EXPECT_EQ(nightschedule::blendLevel(80, 30, 1000), 30);
  EXPECT_EQ(nightschedule::blendLevel(80, 30, 500), 55);
  EXPECT_EQ(nightschedule::blendLevel(0, 100, 250), 25);
}

// The panel edits the night pair from halfway to night, the day pair before that.
TEST(Blend, LevelForShownKeepsTheLightWhereItWasLeft) {
  for (int p : {500, 700, 1000}) {
    const uint8_t night = nightschedule::levelForShown(50, 80, p, true);
    EXPECT_NEAR(nightschedule::blendLevel(80, night, p), 50, 1) << p;
  }
  for (int p : {0, 200, 499}) {
    const uint8_t day = nightschedule::levelForShown(50, 30, p, false);
    EXPECT_NEAR(nightschedule::blendLevel(day, 30, p), 50, 1) << p;
  }
  EXPECT_EQ(nightschedule::levelForShown(10, 90, 600, true), 0);  // out of reach: clamped
}

// Longyearbyen (78.2N): polar night ends mid-February. On the first days with a sunrise, the hours
// before it are still night, not day.
TEST(Ramp, PolarNightEdgesStayNight) {
  constexpr float LAT = 78.22f;
  constexpr float LON = 15.65f;
  time_t t = utc(2027, 2, 10, 0, 0);
  // Find the first day with a sunrise.
  while (nightschedule::solarDayAt(t, LAT, LON).kind != DayKind::Normal) t += 86400;
  const auto first = nightschedule::solarDayAt(t, LAT, LON);
  ASSERT_EQ(nightschedule::solarDayAt(t - 86400, LAT, LON).kind, DayKind::PolarNight);
  EXPECT_EQ(nightschedule::sunNightPermille(first.sunrise - 2 * 3600, LAT, LON), nightschedule::FULL_NIGHT);
}
