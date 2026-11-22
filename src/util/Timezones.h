#pragma once

#include <cstddef>
#include <cstdint>

// Curated IANA-style timezone list for the clock. Each entry carries the POSIX
// TZ rule newlib applies in localtime_r(), so zones with daylight saving get
// the correct wall time year-round from a UTC-keeping RTC — the old biased
// quarter-hour UTC offset could not represent DST at all.
//
// APPEND ONLY: CrossPointSettings::clockTimezone persists an index into this
// table, so reordering or removing entries retargets users' saved zones.
struct TimezoneInfo {
  const char* name{};           // shown in the picker; city/region names stay untranslated
  const char* posixTz{};        // POSIX TZ rule, e.g. "CET-1CEST,M3.5.0,M10.5.0/3"
  int16_t stdOffsetQ{};         // standard (non-DST) offset in quarter hours, for display
                                // and for migrating the legacy clockUtcOffsetQ setting
  int16_t latE2 = NO_LOCATION;  // first named city, hundredths of a degree, north positive
  int16_t lonE2 = 0;            // east positive
  static constexpr int16_t NO_LOCATION = INT16_MIN;
};

namespace timezones {

const TimezoneInfo* table();
size_t count();

// SETTINGS.clockTimezone when valid; otherwise the legacy clockUtcOffsetQ
// mapped to the first entry with the same standard offset, falling back to
// UTC. 255 in the setting means "never chosen".
uint8_t activeIndex();

// Format an entry's standard offset as "UTC+HH:MM" / "UTC-H:MM" / "UTC".
void formatOffset(uint8_t index, char* buf, size_t bufSize);

// Where sunset and sunrise are worked out for a zone: its first named city, or for a bare
// offset its central meridian at 45°N.
void location(uint8_t index, float& latDeg, float& lonDeg);

// Push the active zone's TZ rule into HalClock. Call once at boot after
// settings load, and again whenever clockTimezone changes.
void applyToClock();

}  // namespace timezones
