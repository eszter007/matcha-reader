#include "StatsDay.h"

#include "CrossPointSettings.h"

int32_t statsday::startOffsetSeconds() {
  if (SETTINGS.statsDayStartMode != CrossPointSettings::DAY_START_CUSTOM) return 0;
  const int hour = SETTINGS.statsDayStartHour < 24 ? SETTINGS.statsDayStartHour : 0;
  const int minute = SETTINGS.statsDayStartMinute < 60 ? SETTINGS.statsDayStartMinute : 0;
  return (hour * 60 + minute) * 60;
}

void statsday::localDate(const time_t t, tm& out) {
  // localtime_r into the caller's tm: the shared static buffer is not safe with the render task.
  localtime_r(&t, &out);
  // Compared on the local clock, so the day still starts at the set time on a daylight-saving night.
  if (out.tm_hour * 3600 + out.tm_min * 60 >= startOffsetSeconds()) return;
  out.tm_mday -= 1;
  out.tm_hour = 12;  // noon: mktime() normalises the date without crossing a DST gap
  out.tm_isdst = -1;
  mktime(&out);
}
