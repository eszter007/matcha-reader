#pragma once

#include <cstdint>
#include <ctime>

// The night light schedule's arithmetic and rules: sunrise/sunset, the set-times window, and when
// the light should switch. Pure functions with no device state, so they run in host tests;
// util/NightLight applies the results to the frontlight.
namespace nightschedule {

enum class DayKind : uint8_t { Normal, PolarDay, PolarNight };

struct SolarDay {
  // UTC epochs; meaningful only when kind == Normal.
  time_t sunrise = 0;
  time_t sunset = 0;
  DayKind kind = DayKind::Normal;
};

// Sunrise and sunset of the solar day whose noon is nearest `now`, at latitude/longitude in
// degrees (north and east positive). NOAA's simplified sunrise equation: within a couple of
// minutes away from the polar circles.
SolarDay solarDayAt(time_t now, float latitudeDeg, float longitudeDeg);

// True when minuteOfDay is in [startMinute, endMinute), wrapping past midnight when
// start > end. start == end is an empty window.
bool inWindow(int minuteOfDay, int startMinute, int endMinute);

// Persisted as a number (CrossPointState::nightLightPhase): do not renumber.
enum class Phase : uint8_t { Off = 0, Day = 1, Night = 2 };

// The current phase and the UTC epoch of the boundary that began it. `since` is 0 when the phase
// has no boundary to point at: a polar day or night, or an empty set-times window.
struct Position {
  Phase phase = Phase::Off;
  time_t since = 0;
};

// Set times: local wall-clock minute and second of `now`, window minutes as for inWindow().
Position windowPosition(time_t now, int minuteOfDay, int second, int startMinute, int endMinute);

// Sunset to sunrise at a place.
Position sunPosition(time_t now, float latitudeDeg, float longitudeDeg);

struct Action {
  bool record = false;       // the position differs from the one last acted on: store it
  bool switchLight = false;  // turn the light on (lightOn) or off
  bool lightOn = false;
};

// What to do given where the schedule is now and the position it last acted on. Entering the
// night turns the light on. Entering the day turns it off only after a night the schedule saw or
// slept through, so switching the schedule on by day leaves the light alone. A boundary at least
// MIN_EDGE_GAP_S newer than the last one acted on is a new edge even in the same phase: the device
// slept through a whole night or day. Smaller moves are the same boundary, shifted by a clock
// change. After a setting edit (`edited`), only a change of phase switches the light: moving the
// times or the city within the day or night must not undo what the user set by hand.
inline constexpr time_t MIN_EDGE_GAP_S = 12 * 60 * 60;
Action decide(Position now, Phase lastPhase, time_t lastSince, bool edited = false);

}  // namespace nightschedule
