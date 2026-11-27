#pragma once

#include <cstdint>
#include <ctime>

// The reading day of Insights and the stats sync. It starts at midnight, or at the time set in
// Settings > Insights > Day Starts At; reading before that time counts towards the previous day.
namespace statsday {

// Seconds after local midnight that a reading day starts (0 for midnight).
int32_t startOffsetSeconds();

// The local calendar date `t` counts towards. Only the date and weekday fields are meaningful.
void localDate(time_t t, tm& out);

// "4:30" or "04:30" as minutes after midnight, or -1 when it is not a valid 24-hour time.
inline int parseTime(const char* text) {
  if (!text) return -1;
  int hour = 0;
  int digits = 0;
  const char* p = text;
  while (*p == ' ') p++;
  for (; *p >= '0' && *p <= '9'; p++) {
    hour = hour * 10 + (*p - '0');
    if (++digits > 2) return -1;
  }
  if (digits == 0 || *p != ':') return -1;
  p++;
  if (!(p[0] >= '0' && p[0] <= '9' && p[1] >= '0' && p[1] <= '9')) return -1;
  const int minute = (p[0] - '0') * 10 + (p[1] - '0');
  p += 2;
  while (*p == ' ') p++;
  if (*p != '\0' || hour > 23 || minute > 59) return -1;
  return hour * 60 + minute;
}

}  // namespace statsday
