#include "ReadingStatsStore.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>
#include <numeric>

ReadingStatsStore ReadingStatsStore::instance;

static constexpr const char* STATS_PATH = "/system/reading_stats.bin";
// v3 appends the per-book block after the finished-book paths; v4 appends the per-day-per-language
// block after that. Each older reader stops where its own format ends and ignores what follows,
// rather than rejecting the file -- it will, however, drop the newer blocks the next time it saves.
static constexpr uint8_t STATS_VERSION = 4;
// Longest language tag a per-book entry stores, on disk and in memory. Enforced on BOTH sides:
// the writer clamps to it, and the loader rejects anything longer rather than allocating on a
// corrupt or misaligned file's say-so.
static constexpr size_t MAX_STORED_LANGUAGE = 15;

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
void normalizeLanguage(const char* in, char out[4]) {
  size_t n = 0;
  for (const char* p = in; p && *p; p++) {
    const char c = *p;
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
  const auto* alias =
      std::find_if(std::begin(ALIASES), std::end(ALIASES), [&out](const auto& a) { return strcmp(out, a.from) == 0; });
  if (alias != std::end(ALIASES)) strncpy(out, alias->to, 4);
}

int daysInMonth(uint16_t y, uint8_t m) {
  static constexpr int dm[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m == 2 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0))) return 29;
  return dm[m];
}
}  // namespace

void ReadingStatsStore::addMinutes(uint16_t year, uint8_t month, uint8_t day, uint16_t minutes) {
  const int epoch = daysSinceEpoch(year, month, day);
  // From the back: the day added is almost always the newest, so this ends in a step or two.
  for (auto it = days.rbegin(); it != days.rend(); ++it) {
    const int e = daysSinceEpoch(it->year, it->month, it->day);
    if (e == epoch) {
      // Saturating: a wrap would display a small plausible number instead of an obvious fault.
      const uint32_t sum = static_cast<uint32_t>(it->minutesRead) + minutes;
      it->minutesRead = static_cast<uint16_t>(sum > UINT16_MAX ? UINT16_MAX : sum);
      return;
    }
    if (e < epoch) {
      // Sorted insert. Only a backwards clock jump lands anywhere but the end; the streak
      // passes depend on the ordering.
      days.insert(it.base(), {year, month, day, minutes});
      return;
    }
  }
  days.insert(days.begin(), {year, month, day, minutes});
}

void ReadingStatsStore::addBookMinutes(const char* bookPath, const char* language, const uint16_t minutes,
                                       const uint16_t year, const uint8_t month, const uint8_t day) {
  if (!bookPath || !*bookPath) return;
  const int32_t today = daysSinceEpoch(year, month, day);
  // Clamped to the small-string-optimisation limit: real tags ("ja", "zh-Hant") fit easily, and
  // this keeps each entry's language free of a heap allocation and its on-disk length in a byte.
  // MAX_STORED_LANGUAGE is the same bound the loader enforces on a language read back from disk.
  const std::string lang(language ? language : "", language ? strnlen(language, MAX_STORED_LANGUAGE) : 0);

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

void ReadingStatsStore::addLanguageMinutes(const char* language, const uint16_t minutes, const uint16_t year,
                                           const uint8_t month, const uint8_t day) {
  char lang[4];
  normalizeLanguage(language, lang);

  const int epoch = daysSinceEpoch(year, month, day);
  LanguageDaily fresh{year, month, day, {}, minutes};
  memcpy(fresh.language, lang, sizeof(lang));

  // Sorted like days, so the per-language streak passes need no scratch buffer.
  for (auto it = languageDays.rbegin(); it != languageDays.rend(); ++it) {
    const int e = daysSinceEpoch(it->year, it->month, it->day);
    if (e == epoch && memcmp(it->language, lang, sizeof(lang)) == 0) {
      const uint32_t sum = static_cast<uint32_t>(it->minutesRead) + minutes;
      it->minutesRead = static_cast<uint16_t>(sum > UINT16_MAX ? UINT16_MAX : sum);
      return;
    }
    if (e <= epoch) {
      // Same date, other language: insert alongside rather than walk the whole run.
      languageDays.insert(it.base(), fresh);
      return;
    }
  }
  languageDays.insert(languageDays.begin(), fresh);
}

uint16_t ReadingStatsStore::getMinutesForDay(const char* language, const uint16_t year, const uint8_t month,
                                             const uint8_t day) const {
  char lang[4];
  normalizeLanguage(language, lang);
  const auto it = std::find_if(languageDays.begin(), languageDays.end(), [&](const LanguageDaily& e) {
    return e.year == year && e.month == month && e.day == day && memcmp(e.language, lang, sizeof(lang)) == 0;
  });
  return it == languageDays.end() ? 0 : it->minutesRead;
}

uint32_t ReadingStatsStore::getTotalMinutes(const char* language) const {
  char lang[4];
  normalizeLanguage(language, lang);
  return std::accumulate(languageDays.begin(), languageDays.end(), uint32_t{0},
                         [&lang](const uint32_t sum, const LanguageDaily& e) {
                           return memcmp(e.language, lang, sizeof(lang)) == 0 ? sum + e.minutesRead : sum;
                         });
}

void ReadingStatsStore::getLanguages(std::vector<LanguageSummary>& out) const {
  out.clear();
  for (const auto& e : languageDays) {
    auto it = std::find_if(out.begin(), out.end(),
                           [&e](const LanguageSummary& s) { return memcmp(s.code, e.language, sizeof(s.code)) == 0; });
    if (it != out.end()) {
      it->minutes += e.minutesRead;
      continue;
    }
    LanguageSummary s{};
    memcpy(s.code, e.language, sizeof(s.code));
    s.minutes = e.minutesRead;
    out.push_back(s);
  }
  // Most-read first, so the wanted tab is the one you land on.
  std::sort(out.begin(), out.end(),
            [](const LanguageSummary& a, const LanguageSummary& b) { return a.minutes > b.minutes; });
}

int ReadingStatsStore::getStreak(const char* language, const uint16_t todayYear, const uint8_t todayMonth,
                                 const uint8_t todayDay) const {
  char lang[4];
  normalizeLanguage(language, lang);
  if (getMinutesForDay(language, todayYear, todayMonth, todayDay) == 0) return 0;

  int expected = daysSinceEpoch(todayYear, todayMonth, todayDay);
  int streak = 0;
  for (auto it = languageDays.rbegin(); it != languageDays.rend(); ++it) {
    if (memcmp(it->language, lang, sizeof(lang)) != 0 || it->minutesRead == 0) continue;
    const int e = daysSinceEpoch(it->year, it->month, it->day);
    if (e > expected) continue;
    if (e != expected) break;
    streak++;
    expected--;
  }
  return streak;
}

int ReadingStatsStore::getLongestStreak(const char* language) const {
  char lang[4];
  normalizeLanguage(language, lang);
  int maxStreak = 0, cur = 0, prev = 0;
  for (const auto& e : languageDays) {
    if (memcmp(e.language, lang, sizeof(lang)) != 0 || e.minutesRead == 0) continue;
    const int ep = daysSinceEpoch(e.year, e.month, e.day);
    if (cur == 0) {
      cur = 1;
    } else if (ep == prev + 1) {
      cur++;
    } else if (ep != prev) {
      cur = 1;
    }
    prev = ep;
    if (cur > maxStreak) maxStreak = cur;
  }
  return maxStreak;
}

int ReadingStatsStore::getDaysRead(const char* language) const {
  char lang[4];
  normalizeLanguage(language, lang);
  return static_cast<int>(std::count_if(languageDays.begin(), languageDays.end(), [&lang](const LanguageDaily& e) {
    return memcmp(e.language, lang, sizeof(lang)) == 0 && e.minutesRead > 0;
  }));
}

uint16_t ReadingStatsStore::getMinutesThisWeek(const char* language, const uint16_t todayYear, const uint8_t todayMonth,
                                               const uint8_t todayDay) const {
  const int dow = (dowFromDate(todayYear, todayMonth, todayDay) + 6) % 7;  // ISO Mon=0
  // Wider accumulator: per-day minutes saturate at UINT16_MAX, so seven of them can exceed it.
  // Wrapping would show a small plausible number rather than an obvious fault.
  uint32_t total = 0;
  for (int i = 0; i <= dow; i++) {
    uint16_t y = todayYear;
    uint8_t m = todayMonth, d = todayDay;
    subtractDays(y, m, d, dow - i);
    total += getMinutesForDay(language, y, m, d);
  }
  return static_cast<uint16_t>(std::min<uint32_t>(total, UINT16_MAX));
}

void ReadingStatsStore::getWeekStatus(const char* language, const uint16_t todayYear, const uint8_t todayMonth,
                                      const uint8_t todayDay, const int todayDow, bool readDays[7]) const {
  for (int i = 0; i < 7; i++) readDays[i] = false;
  for (int i = 0; i <= todayDow; i++) {
    uint16_t y = todayYear;
    uint8_t m = todayMonth, d = todayDay;
    subtractDays(y, m, d, todayDow - i);
    readDays[i] = getMinutesForDay(language, y, m, d) > 0;
  }
}

void ReadingStatsStore::getMonthStatus(const char* language, const uint16_t year, const uint8_t month,
                                       bool out[32]) const {
  char lang[4];
  normalizeLanguage(language, lang);
  for (int i = 0; i < 32; i++) out[i] = false;
  for (const auto& e : languageDays) {
    if (memcmp(e.language, lang, sizeof(lang)) != 0 || e.minutesRead == 0) continue;
    if (e.year == year && e.month == month && e.day >= 1 && e.day <= 31) out[e.day] = true;
  }
}

int ReadingStatsStore::getDaysReadInMonth(const char* language, const uint16_t year, const uint8_t month) const {
  bool status[32];
  getMonthStatus(language, year, month, status);
  int count = 0;
  const int dim = daysInMonth(year, month);
  for (int d = 1; d <= dim; d++) {
    if (status[d]) count++;
  }
  return count;
}

uint16_t ReadingStatsStore::getBooksFinished(const char* language) const {
  char lang[4];
  normalizeLanguage(language, lang);
  uint16_t count = 0;
  for (const auto& p : finishedBookPaths) {
    const auto it = std::find_if(books.begin(), books.end(), [&p](const BookReading& b) { return b.path == p; });
    if (it == books.end()) continue;  // evicted from the per-book block; language unknown
    char bookLang[4];
    normalizeLanguage(it->language.c_str(), bookLang);
    if (memcmp(bookLang, lang, sizeof(lang)) == 0) count++;
  }
  return count;
}

void ReadingStatsStore::markBookFinished(const std::string& bookPath) {
  if (std::any_of(finishedBookPaths.begin(), finishedBookPaths.end(),
                  [&bookPath](const std::string& p) { return p == bookPath; })) {
    return;
  }
  finishedBookPaths.push_back(bookPath);
  // max(), not assignment: after a truncated load the path list can hold fewer entries than the
  // count the file's header reported, and a finished-book tally must never count down.
  booksFinished = std::max(booksFinished, static_cast<uint16_t>(finishedBookPaths.size()));
}

uint16_t ReadingStatsStore::getMinutesForDay(uint16_t year, uint8_t month, uint8_t day) const {
  // Back to front: every caller here asks about recent days (today, this week, the month on
  // screen), which now sit at the end of a potentially years-long history.
  const auto it = std::find_if(days.rbegin(), days.rend(), [year, month, day](const DailyReading& d) {
    return d.year == year && d.month == month && d.day == day;
  });
  return it == days.rend() ? 0 : it->minutesRead;
}

bool ReadingStatsStore::hasReadToday(uint16_t year, uint8_t month, uint8_t day) const {
  return getMinutesForDay(year, month, day) > 0;
}

int ReadingStatsStore::getStreak(uint16_t todayYear, uint8_t todayMonth, uint8_t todayDay) const {
  // Backwards through the sorted history, rather than probing one candidate date per day.
  if (days.empty()) return 0;
  int expected = daysSinceEpoch(todayYear, todayMonth, todayDay);
  if (getMinutesForDay(todayYear, todayMonth, todayDay) == 0) return 0;

  int streak = 0;
  for (auto it = days.rbegin(); it != days.rend(); ++it) {
    if (it->minutesRead == 0) continue;
    const int e = daysSinceEpoch(it->year, it->month, it->day);
    if (e > expected) continue;  // days after today (a clock jump) do not extend today's streak
    if (e != expected) break;    // a gap ends it
    streak++;
    expected--;
  }
  return streak;
}

int ReadingStatsStore::getLongestStreak() const {
  // Single pass over sorted history: no scratch buffer (the old int[365] lived on the STACK,
  // which an unbounded history would overflow) and no O(n^2) sort.
  int maxStreak = 0, cur = 0, prev = 0;
  for (const auto& d : days) {
    if (d.minutesRead == 0) continue;
    const int e = daysSinceEpoch(d.year, d.month, d.day);
    if (cur == 0) {
      cur = 1;
    } else if (e == prev + 1) {
      cur++;
    } else if (e != prev) {
      cur = 1;
    }
    prev = e;
    if (cur > maxStreak) maxStreak = cur;
  }
  return maxStreak;
}

int ReadingStatsStore::getDaysRead() const { return static_cast<int>(days.size()); }

uint32_t ReadingStatsStore::getTotalMinutes() const {
  return std::accumulate(days.begin(), days.end(), uint32_t{0},
                         [](const uint32_t sum, const DailyReading& d) { return sum + d.minutesRead; });
}

uint16_t ReadingStatsStore::getMinutesThisWeek(uint16_t todayYear, uint8_t todayMonth, uint8_t todayDay) const {
  int dow = (dowFromDate(todayYear, todayMonth, todayDay) + 6) % 7;  // ISO Mon=0
  uint32_t total = 0;  // see the per-language overload: seven saturated days overflow a uint16
  for (int i = 0; i <= dow; i++) {
    uint16_t y = todayYear;
    uint8_t m = todayMonth, d = todayDay;
    subtractDays(y, m, d, dow - i);
    total += getMinutesForDay(y, m, d);
  }
  return static_cast<uint16_t>(std::min<uint32_t>(total, UINT16_MAX));
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
  // Bounded by BOTH the 16-bit count field and what loadFromFile will accept -- writing more
  // than MAX_DAYS_SANE would leave the loader mid-record and misalign every block after it.
  const uint16_t count = static_cast<uint16_t>(std::min<size_t>(days.size(), MAX_DAYS_SANE));
  f.write(reinterpret_cast<const uint8_t*>(&count), 2);
  f.write(reinterpret_cast<const uint8_t*>(&booksFinished), 2);
  for (uint16_t i = 0; i < count; i++) {
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
  // 16-bit count, as the format defines; clamped rather than silently truncated lower.
  const uint16_t langDayCount = static_cast<uint16_t>(std::min<size_t>(languageDays.size(), UINT16_MAX));
  f.write(reinterpret_cast<const uint8_t*>(&langDayCount), 2);
  size_t written = 0;
  for (const auto& e : languageDays) {
    if (written++ >= langDayCount) break;
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
  if (count > MAX_DAYS_SANE) count = MAX_DAYS_SANE;
  days.clear();
  days.reserve(count);
  for (int i = 0; i < count; i++) {
    DailyReading dr;
    if (f.read(reinterpret_cast<uint8_t*>(&dr), sizeof(DailyReading)) != sizeof(DailyReading)) break;
    // Drop entries recorded while the system clock was unset (RTC-less devices booted at the
    // 1970 epoch before HalClock::restoreSystemTime existed) -- they are misdated garbage that
    // pollutes streaks, totals, and the calendar.
    if (dr.year < 2020) continue;
    days.push_back(dr);
  }
  // The streak passes assume ascending order. Normally one comparison pass, since files are
  // written in order; a file that is not must not silently yield wrong streaks.
  if (!std::is_sorted(days.begin(), days.end(), [](const DailyReading& a, const DailyReading& b) {
        return daysSinceEpoch(a.year, a.month, a.day) < daysSinceEpoch(b.year, b.month, b.day);
      })) {
    std::sort(days.begin(), days.end(), [](const DailyReading& a, const DailyReading& b) {
      return daysSinceEpoch(a.year, a.month, a.day) < daysSinceEpoch(b.year, b.month, b.day);
    });
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
    // Only trust the list's length once it read whole. On a truncated file the header's count is
    // the better answer: leaving a non-zero booksFinished beside a partial list would let
    // markBookFinished() overwrite the real total with the partial one on the next save.
    if (pathsIntact) booksFinished = static_cast<uint16_t>(finishedBookPaths.size());
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
        if (f.read(&langLen, 1) != 1 || langLen > MAX_STORED_LANGUAGE) break;
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
    // No bound check: a uint16 cannot exceed what reserve() can take, and records are
    // fixed-size and validated as read.
    if (f.read(reinterpret_cast<uint8_t*>(&langDayCount), 2) == 2) {
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
      // Same ordering guarantee as days, for the same passes.
      const auto byDate = [](const LanguageDaily& a, const LanguageDaily& b) {
        return daysSinceEpoch(a.year, a.month, a.day) < daysSinceEpoch(b.year, b.month, b.day);
      };
      if (!std::is_sorted(languageDays.begin(), languageDays.end(), byDate)) {
        std::stable_sort(languageDays.begin(), languageDays.end(), byDate);
      }
    }
  }
  f.close();
  return true;
}
