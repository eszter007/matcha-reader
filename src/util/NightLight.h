#pragma once

#include <cstddef>
#include <cstdint>

// Scheduled night light. While the nightly window is open the frontlight uses the night
// brightness and warmth; the window is either two clock times or sunset to sunrise at a chosen
// city. Opening the window turns the light on and closing it turns the light off and restores
// the daytime levels. Nothing happens between those two moments, so a manual change made during
// the night (or the day) stays until the next one. The rules are in NightSchedule.h; the position
// last acted on is kept in CrossPointState, so a window that opened or closed while the device
// slept or was off is caught up on the next boot.
namespace nightlight {

struct City {
  const char* name;  // shown untranslated, like the timezone picker's city names
  int16_t latE2;     // hundredths of a degree, north positive
  int16_t lonE2;     // hundredths of a degree, east positive
};

// CrossPointSettings::nightLightCity is an index into this table: APPEND ONLY.
inline constexpr uint8_t CITY_COUNT = 75;
inline constexpr uint8_t DEFAULT_CITY = 0;  // London
const City& city(uint8_t index);

// Settings store window times as half-hours after midnight.
inline constexpr uint8_t HALF_HOURS_PER_DAY = 48;
// "20:00", or "8:00 PM" when the clock is set to 12-hour.
void formatHalfHour(uint8_t halfHour, char* buf, size_t size);

// Boot, before Frontlight.begin(): replaces the daytime levels with the night levels while the
// window is open, and replaces lightOn when the window opened or closed since the last check.
// Records that check, so skip it on a boot that goes straight back to sleep.
void adjustBootState(uint8_t& brightness, uint8_t& warmth, bool& lightOn);

// Main loop: opens or closes the window on time. Rate-limited internally. Returns true when it
// switched the light, so the screen can show the new state.
bool tick();

// After a schedule setting changes: re-check now and re-apply the current levels. Returns true
// when it changed SETTINGS (the light's saved on/off state); the caller saves.
bool refresh();

// True while the window is open; brightness and warmth edits then change the night levels.
bool nightActive();

// Stores levels picked on the frontlight panel as the night pair when `night`, else as the daytime
// pair. Does not save.
void saveLevels(uint8_t brightness, uint8_t warmth, bool night);

// Value text for the Light Schedule row: "Off", "20:00–06:00", or tonight's sunset and sunrise.
void formatSummary(char* buf, size_t size);

}  // namespace nightlight
