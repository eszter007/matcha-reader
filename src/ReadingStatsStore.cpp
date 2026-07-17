#include "ReadingStatsStore.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

ReadingStatsStore ReadingStatsStore::instance;

static constexpr const char* STATS_PATH = "/system/reading_stats.bin";
static constexpr uint8_t STATS_VERSION = 2;

namespace {
int daysSinceEpoch(uint16_t y, uint8_t m, uint8_t d) {
  int yy = y, mm = m;
  if (mm <= 2) { yy--; mm += 12; }
  return 365 * yy + yy / 4 - yy / 100 + yy / 400 + (153 * (mm - 3) + 2) / 5 + d - 306;
}

int dowFromDate(uint16_t y, uint8_t m, uint8_t d) {
  return (daysSinceEpoch(y, m, d) + 1) % 7;  // 0=Sun
}

void subtractDays(uint16_t& y, uint8_t& m, uint8_t& d, int n) {
  // Exact Gregorian inverse of daysSinceEpoch (Howard Hinnant's civil_from_days, shifted so
  // day 0 = 0000-03-01). The previous version estimated the year with the Julian 1461-day
  // cycle, which by the 2020s runs ~15 days late -- dates in the first half of March resolved
  // into the previous March-based year and came back 1-2 days off, corrupting streaks and
  // week/month views that cross early March.
  const int z = daysSinceEpoch(y, m, d) - n + 305;                            // days since 0000-03-01
  const int era = (z >= 0 ? z : z - 146096) / 146097;                         // 400-year eras
  const unsigned doe = static_cast<unsigned>(z - era * 146097);               // [0, 146096]
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; // [0, 399]
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);               // [0, 365]
  const unsigned mp = (5 * doy + 2) / 153;                                    // [0, 11], 0 = March
  d = static_cast<uint8_t>(doy - (153 * mp + 2) / 5 + 1);
  m = static_cast<uint8_t>(mp < 10 ? mp + 3 : mp - 9);
  y = static_cast<uint16_t>(static_cast<int>(yoe) + era * 400 + (m <= 2 ? 1 : 0));
}

int daysInMonth(uint16_t y, uint8_t m) {
  static constexpr int dm[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m == 2 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0))) return 29;
  return dm[m];
}
}  // namespace

void ReadingStatsStore::addMinutes(uint16_t year, uint8_t month, uint8_t day, uint16_t minutes) {
  for (int i = 0; i < dayCount; i++) {
    if (days[i].year == year && days[i].month == month && days[i].day == day) {
      days[i].minutesRead += minutes;
      return;
    }
  }
  if (dayCount >= MAX_DAYS) {
    memmove(&days[0], &days[1], (MAX_DAYS - 1) * sizeof(DailyReading));
    dayCount = MAX_DAYS - 1;
  }
  days[dayCount++] = {year, month, day, minutes};
}

void ReadingStatsStore::markBookFinished(const std::string& bookPath) {
  for (const auto& p : finishedBookPaths) {
    if (p == bookPath) return;
  }
  finishedBookPaths.push_back(bookPath);
  booksFinished = static_cast<uint16_t>(finishedBookPaths.size());
}

uint16_t ReadingStatsStore::getMinutesForDay(uint16_t year, uint8_t month, uint8_t day) const {
  for (int i = 0; i < dayCount; i++) {
    if (days[i].year == year && days[i].month == month && days[i].day == day)
      return days[i].minutesRead;
  }
  return 0;
}

bool ReadingStatsStore::hasReadToday(uint16_t year, uint8_t month, uint8_t day) const {
  return getMinutesForDay(year, month, day) > 0;
}

int ReadingStatsStore::getStreak(uint16_t todayYear, uint8_t todayMonth, uint8_t todayDay) const {
  int streak = 0;
  if (getMinutesForDay(todayYear, todayMonth, todayDay) == 0) return 0;
  streak = 1;
  for (int i = 1; i < MAX_DAYS; i++) {
    uint16_t py = todayYear; uint8_t pm = todayMonth, pd = todayDay;
    subtractDays(py, pm, pd, i);
    if (getMinutesForDay(py, pm, pd) > 0) streak++;
    else break;
  }
  return streak;
}

int ReadingStatsStore::getLongestStreak() const {
  if (dayCount == 0) return 0;
  // Sort by date epoch, then find longest consecutive run
  int maxStreak = 0, cur = 1;
  // Build epoch array on stack (MAX_DAYS ≤ 365, so 365*4 = 1460 bytes — OK)
  int epochs[MAX_DAYS];
  for (int i = 0; i < dayCount; i++)
    epochs[i] = daysSinceEpoch(days[i].year, days[i].month, days[i].day);
  // Simple O(n^2) sort — dayCount is small
  for (int i = 0; i < dayCount - 1; i++)
    for (int j = i + 1; j < dayCount; j++)
      if (epochs[j] < epochs[i]) { int t = epochs[i]; epochs[i] = epochs[j]; epochs[j] = t; }
  maxStreak = 1;
  cur = 1;
  for (int i = 1; i < dayCount; i++) {
    if (epochs[i] == epochs[i - 1] + 1) {
      cur++;
      if (cur > maxStreak) maxStreak = cur;
    } else if (epochs[i] != epochs[i - 1]) {
      cur = 1;
    }
  }
  return maxStreak;
}

int ReadingStatsStore::getDaysRead() const { return dayCount; }

uint32_t ReadingStatsStore::getTotalMinutes() const {
  uint32_t total = 0;
  for (int i = 0; i < dayCount; i++) total += days[i].minutesRead;
  return total;
}

uint16_t ReadingStatsStore::getMinutesThisWeek(uint16_t todayYear, uint8_t todayMonth, uint8_t todayDay) const {
  int dow = (dowFromDate(todayYear, todayMonth, todayDay) + 6) % 7;  // ISO Mon=0
  uint16_t total = 0;
  for (int i = 0; i <= dow; i++) {
    uint16_t y = todayYear; uint8_t m = todayMonth, d = todayDay;
    subtractDays(y, m, d, dow - i);
    total += getMinutesForDay(y, m, d);
  }
  return total;
}

void ReadingStatsStore::getWeekStatus(uint16_t todayYear, uint8_t todayMonth, uint8_t todayDay,
                                      int todayDow, bool readDays[7]) const {
  for (int i = 0; i < 7; i++) readDays[i] = false;
  for (int i = 0; i <= todayDow; i++) {
    uint16_t y = todayYear; uint8_t m = todayMonth, d = todayDay;
    subtractDays(y, m, d, todayDow - i);
    readDays[i] = getMinutesForDay(y, m, d) > 0;
  }
}

void ReadingStatsStore::getMonthStatus(uint16_t year, uint8_t month, bool out[32]) const {
  for (int i = 0; i < 32; i++) out[i] = false;
  int dim = daysInMonth(year, month);
  for (int d = 1; d <= dim; d++) {
    out[d] = getMinutesForDay(year, month, static_cast<uint8_t>(d)) > 0;
  }
}

int ReadingStatsStore::getDaysReadInMonth(uint16_t year, uint8_t month) const {
  int count = 0;
  int dim = daysInMonth(year, month);
  for (int d = 1; d <= dim; d++) {
    if (getMinutesForDay(year, month, static_cast<uint8_t>(d)) > 0) count++;
  }
  return count;
}

bool ReadingStatsStore::saveToFile() const {
  HalFile f;
  if (!Storage.openFileForWrite("STAT", STATS_PATH, f)) return false;
  f.write(&STATS_VERSION, 1);
  uint16_t count = static_cast<uint16_t>(dayCount);
  f.write(reinterpret_cast<const uint8_t*>(&count), 2);
  f.write(reinterpret_cast<const uint8_t*>(&booksFinished), 2);
  for (int i = 0; i < dayCount; i++) {
    f.write(reinterpret_cast<const uint8_t*>(&days[i]), sizeof(DailyReading));
  }
  // Write finished book paths
  uint16_t pathCount = static_cast<uint16_t>(finishedBookPaths.size());
  f.write(reinterpret_cast<const uint8_t*>(&pathCount), 2);
  for (const auto& p : finishedBookPaths) {
    uint16_t len = static_cast<uint16_t>(p.size());
    f.write(reinterpret_cast<const uint8_t*>(&len), 2);
    f.write(reinterpret_cast<const uint8_t*>(p.data()), len);
  }
  f.close();
  return true;
}

bool ReadingStatsStore::loadFromFile() {
  HalFile f;
  if (!Storage.openFileForRead("STAT", STATS_PATH, f)) return false;
  uint8_t version;
  if (f.read(&version, 1) != 1) { f.close(); return false; }
  uint16_t count;
  if (f.read(reinterpret_cast<uint8_t*>(&count), 2) != 2) { f.close(); return false; }
  if (version >= 2) {
    if (f.read(reinterpret_cast<uint8_t*>(&booksFinished), 2) != 2) { f.close(); return false; }
  }
  if (count > MAX_DAYS) count = MAX_DAYS;
  dayCount = 0;
  for (int i = 0; i < count; i++) {
    DailyReading dr;
    if (f.read(reinterpret_cast<uint8_t*>(&dr), sizeof(DailyReading)) != sizeof(DailyReading)) break;
    // Drop entries recorded while the system clock was unset (RTC-less devices booted at the
    // 1970 epoch before HalClock::restoreSystemTime existed) -- they are misdated garbage that
    // pollutes streaks, totals, and the calendar.
    if (dr.year < 2020) continue;
    days[dayCount++] = dr;
  }
  // Read finished book paths
  finishedBookPaths.clear();
  uint16_t pathCount = 0;
  if (f.read(reinterpret_cast<uint8_t*>(&pathCount), 2) == 2 && pathCount <= 500) {
    for (int i = 0; i < pathCount; i++) {
      uint16_t len = 0;
      if (f.read(reinterpret_cast<uint8_t*>(&len), 2) != 2 || len > 500) break;
      std::string p(len, '\0');
      if (f.read(reinterpret_cast<uint8_t*>(&p[0]), len) != len) break;
      finishedBookPaths.push_back(std::move(p));
    }
    booksFinished = static_cast<uint16_t>(finishedBookPaths.size());
  }
  f.close();
  return true;
}
