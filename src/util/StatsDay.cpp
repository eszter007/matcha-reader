#include "StatsDay.h"

#include <cstdio>

#include "CrossPointSettings.h"

int32_t statsday::startOffsetSeconds() {
  if (SETTINGS.statsDayStartMode != CrossPointSettings::DAY_START_CUSTOM) return 0;
  const uint8_t quarter = SETTINGS.statsDayStartQuarter;
  return quarter < CrossPointSettings::QUARTER_HOURS_PER_DAY ? quarter * 15 * 60 : 0;
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

void statsday::formatQuarter(const uint8_t quarter, char* buf, const size_t size) {
  snprintf(buf, size, "%02u:%02u", quarter / 4u, (quarter % 4u) * 15u);
}
