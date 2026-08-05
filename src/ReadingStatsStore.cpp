#include "ReadingStatsStore.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

ReadingStatsStore ReadingStatsStore::instance;

static constexpr const char* STATS_PATH = "/system/reading_stats.bin";
// v3 appends the per-book block after the finished-book paths; v4 appends the per-day-per-language
// block after that. Each older reader stops where its own format ends and ignores what follows,
// rather than rejecting the file -- it will, however, drop the newer blocks the next time it saves.
static constexpr uint8_t STATS_VERSION = 4;

namespace {
int daysSinceEpoch(uint16_t y, uint8_t m, uint8_t d) {
  int yy = y, mm = m;
  if (mm <= 2) {
    yy--;
    mm += 12;
  }
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
  const int z = daysSinceEpoch(y, m, d) - n + 305;                             // days since 0000-03-01
  const int era = (z >= 0 ? z : z - 146096) / 146097;                          // 400-year eras
  const unsigned doe = static_cast<unsigned>(z - era * 146097);                // [0, 146096]
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;  // [0, 399]
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);                // [0, 365]
  const unsigned mp = (5 * doy + 2) / 153;                                     // [0, 11], 0 = March
  d = static_cast<uint8_t>(doy - (153 * mp + 2) / 5 + 1);
  m = static_cast<uint8_t>(mp < 10 ? mp + 3 : mp - 9);
  y = static_cast<uint16_t>(static_cast<int>(yoe) + era * 400 + (m <= 2 ? 1 : 0));
}

// "ja-JP" / "JA" / "ja_jp" all bucket as "ja". Anything longer than 3 chars (no ISO-639 code is)
// is truncated rather than rejected, so a malformed tag still lands in a stable bucket.
void normalizeLanguage(const std::string& in, char out[4]) {
  size_t n = 0;
  for (const char c : in) {
    if (c == '-' || c == '_' || n == 3) break;
    out[n++] = static_cast<char>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
  }
  for (; n < 4; n++) out[n] = '\0';

  // Country codes people reach for instead of the language code. Left unmapped they would each
  // open a second bucket for a language that already has one, splitting its totals in half.
  static constexpr struct {
    const char* from;
    const char* to;
  } ALIASES[] = {{"jp", "ja"}, {"cn", "zh"}, {"kr", "ko"}};
  for (const auto& a : ALIASES) {
    if (strcmp(out, a.from) == 0) {
      strncpy(out, a.to, 4);
      return;
    }
  }
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

void ReadingStatsStore::addBookMinutes(const std::string& bookPath, const std::string& language, const uint16_t minutes,
                                       const uint16_t year, const uint8_t month, const uint8_t day) {
  if (bookPath.empty()) return;
  const int32_t today = daysSinceEpoch(year, month, day);
  // Clamped to the small-string-optimisation limit: real tags ("ja", "zh-Hant") fit easily, and
  // this keeps each entry's language free of a heap allocation and its on-disk length in a byte.
  const std::string lang = language.substr(0, 15);

  for (auto& b : books) {
    if (b.path != bookPath) continue;
    b.minutesRead += minutes;
    b.lastReadDay = today;
    if (!lang.empty()) b.language = lang;
    return;
  }

  if (books.size() >= MAX_BOOKS) {
    auto oldest = std::min_element(books.begin(), books.end(), [](const BookReading& a, const BookReading& b) {
      return a.lastReadDay < b.lastReadDay;
    });
    books.erase(oldest);
  }
  // Deliberately not reserve(MAX_BOOKS): that would commit ~11KB of DRAM the moment the first
  // book is read. This grows one entry per new book, not in a loop.
  books.push_back({bookPath, lang, minutes, today});
}

void ReadingStatsStore::addLanguageMinutes(const std::string& language, const uint16_t minutes, const uint16_t year,
                                           const uint8_t month, const uint8_t day) {
  char lang[4];
  normalizeLanguage(language, lang);

  for (auto& e : languageDays) {
    if (e.year != year || e.month != month || e.day != day) continue;
    if (memcmp(e.language, lang, sizeof(lang)) != 0) continue;
    e.minutesRead = static_cast<uint16_t>(e.minutesRead + minutes);
    return;
  }

  if (languageDays.size() >= MAX_LANG_DAYS) {
    // Oldest calendar day first, matching how days[] evicts: the recent past is what the
    // streak and calendar views read.
    auto oldest =
        std::min_element(languageDays.begin(), languageDays.end(), [](const LanguageDaily& a, const LanguageDaily& b) {
          return daysSinceEpoch(a.year, a.month, a.day) < daysSinceEpoch(b.year, b.month, b.day);
        });
    languageDays.erase(oldest);
  }
  LanguageDaily e{year, month, day, {}, minutes};
  memcpy(e.language, lang, sizeof(lang));
  languageDays.push_back(e);
}

uint16_t ReadingStatsStore::getMinutesForDay(const std::string& language, const uint16_t year, const uint8_t month,
                                             const uint8_t day) const {
  char lang[4];
  normalizeLanguage(language, lang);
  for (const auto& e : languageDays) {
    if (e.year == year && e.month == month && e.day == day && memcmp(e.language, lang, sizeof(lang)) == 0) {
      return e.minutesRead;
    }
  }
  return 0;
}

uint32_t ReadingStatsStore::getTotalMinutes(const std::string& language) const {
  char lang[4];
  normalizeLanguage(language, lang);
  uint32_t total = 0;
  for (const auto& e : languageDays) {
    if (memcmp(e.language, lang, sizeof(lang)) == 0) total += e.minutesRead;
  }
  return total;
}

void ReadingStatsStore::markBookFinished(const std::string& bookPath) {
  if (std::any_of(finishedBookPaths.begin(), finishedBookPaths.end(),
                  [&bookPath](const std::string& p) { return p == bookPath; })) {
    return;
  }
  finishedBookPaths.push_back(bookPath);
  booksFinished = static_cast<uint16_t>(finishedBookPaths.size());
}

uint16_t ReadingStatsStore::getMinutesForDay(uint16_t year, uint8_t month, uint8_t day) const {
  for (int i = 0; i < dayCount; i++) {
    if (days[i].year == year && days[i].month == month && days[i].day == day) return days[i].minutesRead;
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
    uint16_t py = todayYear;
    uint8_t pm = todayMonth, pd = todayDay;
    subtractDays(py, pm, pd, i);
    if (getMinutesForDay(py, pm, pd) > 0)
      streak++;
    else
      break;
  }
  return streak;
}

int ReadingStatsStore::getLongestStreak() const {
  if (dayCount == 0) return 0;
  // Sort by date epoch, then find longest consecutive run
  int maxStreak = 0, cur = 1;
  // Build epoch array on stack (MAX_DAYS ≤ 365, so 365*4 = 1460 bytes — OK)
  int epochs[MAX_DAYS];
  for (int i = 0; i < dayCount; i++) epochs[i] = daysSinceEpoch(days[i].year, days[i].month, days[i].day);
  // Simple O(n^2) sort — dayCount is small
  for (int i = 0; i < dayCount - 1; i++)
    for (int j = i + 1; j < dayCount; j++)
      if (epochs[j] < epochs[i]) {
        int t = epochs[i];
        epochs[i] = epochs[j];
        epochs[j] = t;
      }
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
    uint16_t y = todayYear;
    uint8_t m = todayMonth, d = todayDay;
    subtractDays(y, m, d, dow - i);
    total += getMinutesForDay(y, m, d);
  }
  return total;
}

void ReadingStatsStore::getWeekStatus(uint16_t todayYear, uint8_t todayMonth, uint8_t todayDay, int todayDow,
                                      bool readDays[7]) const {
  for (int i = 0; i < 7; i++) readDays[i] = false;
  for (int i = 0; i <= todayDow; i++) {
    uint16_t y = todayYear;
    uint8_t m = todayMonth, d = todayDay;
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
  // Per-book block (v3+)
  uint16_t bookCount = static_cast<uint16_t>(books.size());
  f.write(reinterpret_cast<const uint8_t*>(&bookCount), 2);
  for (const auto& b : books) {
    uint16_t pathLen = static_cast<uint16_t>(b.path.size());
    f.write(reinterpret_cast<const uint8_t*>(&pathLen), 2);
    f.write(reinterpret_cast<const uint8_t*>(b.path.data()), pathLen);
    uint8_t langLen = static_cast<uint8_t>(b.language.size());
    f.write(&langLen, 1);
    f.write(reinterpret_cast<const uint8_t*>(b.language.data()), langLen);
    f.write(reinterpret_cast<const uint8_t*>(&b.minutesRead), 4);
    f.write(reinterpret_cast<const uint8_t*>(&b.lastReadDay), 4);
  }
  // Per-day-per-language block (v4+). Fields written individually rather than as a struct blob:
  // LanguageDaily has trailing padding on some targets, and a padded layout would not survive a
  // format change on the reading side.
  uint16_t langDayCount = static_cast<uint16_t>(languageDays.size());
  f.write(reinterpret_cast<const uint8_t*>(&langDayCount), 2);
  for (const auto& e : languageDays) {
    f.write(reinterpret_cast<const uint8_t*>(&e.year), 2);
    f.write(&e.month, 1);
    f.write(&e.day, 1);
    f.write(reinterpret_cast<const uint8_t*>(e.language), 4);
    f.write(reinterpret_cast<const uint8_t*>(&e.minutesRead), 2);
  }
  f.close();
  return true;
}

bool ReadingStatsStore::loadFromFile() {
  HalFile f;
  if (!Storage.openFileForRead("STAT", STATS_PATH, f)) return false;
  uint8_t version;
  if (f.read(&version, 1) != 1) {
    f.close();
    return false;
  }
  uint16_t count;
  if (f.read(reinterpret_cast<uint8_t*>(&count), 2) != 2) {
    f.close();
    return false;
  }
  if (version >= 2) {
    if (f.read(reinterpret_cast<uint8_t*>(&booksFinished), 2) != 2) {
      f.close();
      return false;
    }
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
  bool pathsIntact = false;
  uint16_t pathCount = 0;
  if (f.read(reinterpret_cast<uint8_t*>(&pathCount), 2) == 2 && pathCount <= 500) {
    pathsIntact = true;
    for (int i = 0; i < pathCount; i++) {
      uint16_t len = 0;
      if (f.read(reinterpret_cast<uint8_t*>(&len), 2) != 2 || len > 500) {
        pathsIntact = false;
        break;
      }
      std::string p(len, '\0');
      if (f.read(reinterpret_cast<uint8_t*>(&p[0]), len) != len) {
        pathsIntact = false;
        break;
      }
      finishedBookPaths.push_back(std::move(p));
    }
    booksFinished = static_cast<uint16_t>(finishedBookPaths.size());
  }
  // Per-book block (v3+). Only readable when the paths block above was consumed whole -- a short
  // read there leaves the file position mid-record, so anything after it is misaligned garbage.
  books.clear();
  bool booksIntact = false;
  if (version >= 3 && pathsIntact) {
    uint16_t bookCount = 0;
    if (f.read(reinterpret_cast<uint8_t*>(&bookCount), 2) == 2 && bookCount <= MAX_BOOKS) {
      books.reserve(bookCount);
      for (int i = 0; i < bookCount; i++) {
        BookReading b;
        uint16_t pathLen = 0;
        if (f.read(reinterpret_cast<uint8_t*>(&pathLen), 2) != 2 || pathLen > 500) break;
        b.path.assign(pathLen, '\0');
        if (pathLen && f.read(reinterpret_cast<uint8_t*>(&b.path[0]), pathLen) != pathLen) break;
        uint8_t langLen = 0;
        if (f.read(&langLen, 1) != 1) break;
        b.language.assign(langLen, '\0');
        if (langLen && f.read(reinterpret_cast<uint8_t*>(&b.language[0]), langLen) != langLen) break;
        if (f.read(reinterpret_cast<uint8_t*>(&b.minutesRead), 4) != 4) break;
        if (f.read(reinterpret_cast<uint8_t*>(&b.lastReadDay), 4) != 4) break;
        books.push_back(std::move(b));
      }
      // Only a complete block leaves the file positioned at the start of the next one; any
      // short read above stopped mid-record.
      booksIntact = books.size() == bookCount;
    }
  }
  // Per-day-per-language block (v4+)
  languageDays.clear();
  if (version >= 4 && booksIntact) {
    uint16_t langDayCount = 0;
    if (f.read(reinterpret_cast<uint8_t*>(&langDayCount), 2) == 2 && langDayCount <= MAX_LANG_DAYS) {
      languageDays.reserve(langDayCount);
      for (int i = 0; i < langDayCount; i++) {
        LanguageDaily e{};
        if (f.read(reinterpret_cast<uint8_t*>(&e.year), 2) != 2) break;
        if (f.read(&e.month, 1) != 1) break;
        if (f.read(&e.day, 1) != 1) break;
        if (f.read(reinterpret_cast<uint8_t*>(e.language), 4) != 4) break;
        if (f.read(reinterpret_cast<uint8_t*>(&e.minutesRead), 2) != 2) break;
        e.language[3] = '\0';         // a corrupt record must not leave an unterminated tag
        if (e.year < 2020) continue;  // same unset-clock garbage the per-day loop drops
        languageDays.push_back(e);
      }
    }
  }
  f.close();
  return true;
}
