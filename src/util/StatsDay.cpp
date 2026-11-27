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
  const time_t shifted = t - static_cast<time_t>(startOffsetSeconds());
  localtime_r(&shifted, &out);
}
