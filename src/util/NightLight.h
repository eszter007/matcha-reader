#pragma once

#include <cstddef>
#include <cstdint>

// Scheduled night light. The frontlight's brightness and warmth move from the daytime pair to the
// night pair over the hour before the night starts, and back over the hour after it ends. The
// night is either two clock times or sunset to sunrise in the clock's time zone
// (timezones::location). The levels are worked out from the clock on every check, so nothing has
// to be caught up. With Switch Light On and Off the light also turns on when the night starts and
// off when it ends; the position last acted on is kept in CrossPointState, so an edge passed while
// the device slept or was off is caught up on the next boot.
namespace nightlight {

// Settings store window times as half-hours after midnight.
inline constexpr uint8_t HALF_HOURS_PER_DAY = 48;
// "20:00", or "8:00 PM" when the clock is set to 12-hour.
void formatHalfHour(uint8_t halfHour, char* buf, size_t size);

// Boot, before Frontlight.begin(): replaces the daytime levels with the scheduled ones, and
// replaces lightOn when the schedule switches the light and the night started or ended since the
// last check.
// Records that check, so skip it on a boot that goes straight back to sleep.
void adjustBootState(uint8_t& brightness, uint8_t& warmth, bool& lightOn);

// Main loop: moves the levels and switches the light on time. Rate-limited internally. Returns
// true when it switched the light, so the screen can show the new state.
bool tick();

// After a schedule setting changes: re-check now and re-apply the current levels. Returns true
// when it changed SETTINGS (the light's saved on/off state); the caller saves.
bool refresh();

// True while the levels are nearer the night pair; brightness and warmth edits then change it.
bool nightActive();

// While held (the frontlight panel is open), checks leave the levels alone.
void holdLevels(bool hold);

// Stores levels picked on the frontlight panel into the night pair when `night`, else the daytime
// pair, so that the light stays at them even partway through a change. Does not save.
void saveLevels(uint8_t brightness, uint8_t warmth, bool night);

// Value text for the Light Schedule row: "Off", "20:00–06:00", or tonight's sunset and sunrise.
void formatSummary(char* buf, size_t size);

}  // namespace nightlight
