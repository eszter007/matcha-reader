#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>

// The reading day of Insights and the stats sync. It starts at midnight, or at the time set in
// Settings > Insights > Day starts at; reading before that time counts towards the previous day.
namespace statsday {

// Seconds after local midnight that a reading day starts (0 for midnight).
int32_t startOffsetSeconds();

// The local calendar date `t` counts towards. Only the date and weekday fields are meaningful.
void localDate(time_t t, tm& out);

// A quarter-hour of the day as 24-hour "HH:MM".
void formatQuarter(uint8_t quarter, char* buf, size_t size);

}  // namespace statsday
