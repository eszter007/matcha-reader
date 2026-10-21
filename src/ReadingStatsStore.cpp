#include "ReadingStatsStore.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <SdSystemDir.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <numeric>

ReadingStatsStore ReadingStatsStore::instance;

static std::string statsPath() { return sdsystem::path("reading_stats.bin"); }
// Changes recorded since the last compact(); see record().
static std::string journalPath() { return sdsystem::path("reading_stats.log"); }
// Journal records start with this byte, so a torn or foreign tail is recognised and ignored.
static constexpr uint8_t JOURNAL_RECORD_MAGIC = 0xA5;
// A journal opens with this byte and a random nonce, so no two journals share a prefix: the fold
// marker (see compact()) recognises a journal by its leading bytes.
static constexpr uint8_t JOURNAL_HEADER_MAGIC = 0x5A;
static constexpr size_t JOURNAL_HEADER_BYTES = 5;
static constexpr size_t MAX_STORED_PATH = 500;
// record() runs on the main loop, the end-of-book summary on the render task; one journal writer
// at a time, and no fold while a record is being appended.
static std::mutex& journalMutex() {
  static std::mutex m;
  return m;
}
// Heap a table reserve must leave behind; a reserve() that cannot be served aborts.
static constexpr uint32_t TABLE_RESERVE_MARGIN = 4 * 1024;
static bool tableFits(const size_t bytes) { return ESP.getMaxAllocHeap() >= bytes + TABLE_RESERVE_MARGIN; }
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
  return daysSinceEpoch(y, m, d) % 7;  // 0=Sun
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

  // Sorted like days, so the per-language streak passes need no scratch buffer. From the back:
  // the date added is almost always the newest.
  const auto dateOf = [](const LanguageDaily& e) { return daysSinceEpoch(e.year, e.month, e.day); };
  auto pos = languageDays.end();
  while (pos != languageDays.begin() && dateOf(*(pos - 1)) > epoch) --pos;
  // The whole run of this date, not just its last entry: reading ja, en, then ja again in one day
  // must add to the first ja record, not open a second one.
  for (auto it = pos; it != languageDays.begin() && dateOf(*(it - 1)) == epoch; --it) {
    auto& e = *(it - 1);
    if (memcmp(e.language, lang, sizeof(lang)) == 0) {
      const uint32_t sum = static_cast<uint32_t>(e.minutesRead) + minutes;
      e.minutesRead = static_cast<uint16_t>(sum > UINT16_MAX ? UINT16_MAX : sum);
      return;
    }
  }
  languageDays.insert(pos, fresh);
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
  const std::string tmpPath = statsPath() + ".tmp";
  HalFile f;
  if (!Storage.openFileForWrite("STAT", tmpPath.c_str(), f)) return false;
  // Every write is checked: a short file must never replace a good one.
  bool ok = true;
  const auto put = [&f, &ok](const void* data, const size_t n) {
    if (ok && n && f.write(static_cast<const uint8_t*>(data), n) != n) ok = false;
  };
  put(&STATS_VERSION, 1);
  // days never exceeds MAX_DAYS in memory, so this only guards the 16-bit field.
  const uint16_t count = static_cast<uint16_t>(std::min<size_t>(days.size(), UINT16_MAX));
  put(reinterpret_cast<const uint8_t*>(&count), 2);
  put(reinterpret_cast<const uint8_t*>(&booksFinished), 2);
  for (uint16_t i = 0; i < count; i++) {
    put(reinterpret_cast<const uint8_t*>(&days[i]), sizeof(DailyReading));
  }
  // Write finished book paths
  uint16_t pathCount = static_cast<uint16_t>(finishedBookPaths.size());
  put(reinterpret_cast<const uint8_t*>(&pathCount), 2);
  for (const auto& p : finishedBookPaths) {
    uint16_t len = static_cast<uint16_t>(p.size());
    put(reinterpret_cast<const uint8_t*>(&len), 2);
    put(reinterpret_cast<const uint8_t*>(p.data()), len);
  }
  // Per-book block (v3+)
  uint16_t bookCount = static_cast<uint16_t>(books.size());
  put(reinterpret_cast<const uint8_t*>(&bookCount), 2);
  for (const auto& b : books) {
    uint16_t pathLen = static_cast<uint16_t>(b.path.size());
    put(reinterpret_cast<const uint8_t*>(&pathLen), 2);
    put(reinterpret_cast<const uint8_t*>(b.path.data()), pathLen);
    uint8_t langLen = static_cast<uint8_t>(b.language.size());
    put(&langLen, 1);
    put(reinterpret_cast<const uint8_t*>(b.language.data()), langLen);
    put(reinterpret_cast<const uint8_t*>(&b.minutesRead), 4);
    put(reinterpret_cast<const uint8_t*>(&b.lastReadDay), 4);
  }
  // Per-day-per-language block (v4+). Fields written individually rather than as a struct blob:
  // LanguageDaily has trailing padding on some targets, and a padded layout would not survive a
  // format change on the reading side.
  // 16-bit count, as the format defines; clamped rather than silently truncated lower.
  const uint16_t langDayCount = static_cast<uint16_t>(std::min<size_t>(languageDays.size(), UINT16_MAX));
  put(reinterpret_cast<const uint8_t*>(&langDayCount), 2);
  size_t written = 0;
  for (const auto& e : languageDays) {
    if (written++ >= langDayCount) break;
    put(reinterpret_cast<const uint8_t*>(&e.year), 2);
    put(&e.month, 1);
    put(&e.day, 1);
    put(reinterpret_cast<const uint8_t*>(e.language), 4);
    put(reinterpret_cast<const uint8_t*>(&e.minutesRead), 2);
  }
  put(&foldedJournalBytes, 4);
  put(&foldedJournalHash, 4);
  // SdFat writes the last sector on close: a failure there is a short file too.
  ok = f.close() && ok;
  if (!ok) {
    LOG_ERR("STAT", "Stats save failed; previous file kept");
    Storage.remove(tmpPath.c_str());
    return false;
  }
  return Storage.replaceFile(tmpPath.c_str(), statsPath().c_str());
}

bool ReadingStatsStore::loadMainFile(const size_t pendingRecords) {
  HalFile f;
  if (!Storage.openFileForRead("STAT", statsPath().c_str(), f)) return false;
  // Shorter than its own header (an older firmware's in-place save cut off by power loss): no
  // history to keep, as opposed to a read error, which leaves the file for the next try.
  if (f.size() < 5) return true;
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
  // Keep the most recent MAX_DAYS and CONSUME the rest: skipping them by clamping `count`
  // would leave the file positioned mid-block and misalign everything after it.
  days.clear();
  const size_t keepDays = std::min<size_t>(count, MAX_DAYS);
  for (size_t i = 0; i + keepDays < count; i++) {
    DailyReading discard;
    if (f.read(reinterpret_cast<uint8_t*>(&discard), sizeof(DailyReading)) != sizeof(DailyReading)) break;
  }
  // Room for the journal's records too, so replaying them never regrows the table.
  const size_t dayCapacity = keepDays + pendingRecords;
  if (!tableFits(dayCapacity * sizeof(DailyReading))) return false;
  days.reserve(dayCapacity);
  for (size_t i = 0; i < keepDays; i++) {
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
      const size_t bookCapacity = std::min<size_t>(bookCount + pendingRecords, MAX_BOOKS);
      if (!tableFits(bookCapacity * sizeof(BookReading))) return false;
      books.reserve(bookCapacity);
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
    if (f.read(reinterpret_cast<uint8_t*>(&langDayCount), 2) == 2) {
      // Same bounded-tail read as the day block above: a uint16 count is 640KB of records at
      // worst, and reserve() aborts under -fno-exceptions rather than failing.
      const size_t keepLang = std::min<size_t>(langDayCount, MAX_LANG_DAYS);
      for (size_t i = 0; i + keepLang < langDayCount; i++) {
        uint8_t discard[10];
        if (f.read(discard, sizeof(discard)) != sizeof(discard)) break;
      }
      const size_t langCapacity = keepLang + pendingRecords;  // see days
      if (!tableFits(langCapacity * sizeof(LanguageDaily))) return false;
      languageDays.reserve(langCapacity);
      bool langIntact = true;
      for (size_t i = 0; i < keepLang && langIntact; i++) {
        LanguageDaily e{};
        langIntact = f.read(reinterpret_cast<uint8_t*>(&e.year), 2) == 2 && f.read(&e.month, 1) == 1 &&
                     f.read(&e.day, 1) == 1 && f.read(reinterpret_cast<uint8_t*>(e.language), 4) == 4 &&
                     f.read(reinterpret_cast<uint8_t*>(&e.minutesRead), 2) == 2;
        e.language[3] = '\0';  // a corrupt record must not leave an unterminated tag
        if (langIntact && e.year >= 2020) languageDays.push_back(e);  // same unset-clock rule as days
      }
      // The journal prefix this file already holds (see compact()); absent in older files. A file
      // damaged before this point reads as holding none, so the whole journal is replayed.
      if (!langIntact || f.read(reinterpret_cast<uint8_t*>(&foldedJournalBytes), 4) != 4 ||
          f.read(reinterpret_cast<uint8_t*>(&foldedJournalHash), 4) != 4) {
        foldedJournalBytes = 0;
        foldedJournalHash = 0;
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

namespace {
uint32_t fnv1a(uint32_t h, const uint8_t* p, const size_t n) {
  for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
  return h;
}
constexpr uint32_t FNV_BASIS = 2166136261u;
uint32_t pathHash(const std::string& s) {
  return fnv1a(FNV_BASIS, reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

bool skipBytes(HalFile& f, size_t n) {
  uint8_t buf[64];
  while (n) {
    const size_t k = std::min(n, sizeof(buf));
    if (f.read(buf, k) != static_cast<int>(k)) return false;
    n -= k;
  }
  return true;
}

// One journal record: magic, date, minutes, then three length-prefixed strings (language, book
// path, finished path). Built whole so it goes to the card in one write.
void appendString(std::string& out, const char* s, const size_t maxLen) {
  const auto len = static_cast<uint16_t>(s ? strnlen(s, maxLen) : 0);
  out.append(reinterpret_cast<const char*>(&len), 2);
  if (len) out.append(s, len);
}
bool readString(HalFile& f, std::string& out, const size_t maxLen) {
  uint16_t len = 0;
  if (f.read(reinterpret_cast<uint8_t*>(&len), 2) != 2 || len > maxLen) return false;
  out.assign(len, '\0');
  return len == 0 || f.read(reinterpret_cast<uint8_t*>(&out[0]), len) == static_cast<int>(len);
}
struct JournalRecord {
  uint16_t year = 0;
  uint8_t month = 0;
  uint8_t day = 0;
  uint16_t minutes = 0;
  std::string language, bookPath, finishedPath;
};
bool readRecord(HalFile& f, JournalRecord& r) {
  uint8_t magic = 0;
  return f.read(&magic, 1) == 1 && magic == JOURNAL_RECORD_MAGIC &&
         f.read(reinterpret_cast<uint8_t*>(&r.year), 2) == 2 && f.read(&r.month, 1) == 1 && f.read(&r.day, 1) == 1 &&
         f.read(reinterpret_cast<uint8_t*>(&r.minutes), 2) == 2 && readString(f, r.language, MAX_STORED_LANGUAGE) &&
         readString(f, r.bookPath, MAX_STORED_PATH) && readString(f, r.finishedPath, MAX_STORED_PATH);
}

// Size and hash of the whole journal: what compact() records as folded into the stats file.
void journalDigest(uint32_t& bytes, uint32_t& hash) {
  bytes = 0;
  hash = FNV_BASIS;
  HalFile f;
  if (!Storage.openFileForRead("STAT", journalPath().c_str(), f)) return;
  uint8_t buf[64];
  int k;
  while ((k = f.read(buf, sizeof(buf))) > 0) {
    hash = fnv1a(hash, buf, static_cast<size_t>(k));
    bytes += static_cast<uint32_t>(k);
  }
}

// Opens the journal positioned after the prefix a stats file already holds, so a fold whose
// journal removal was interrupted is never counted twice. A journal that does not start with that
// prefix is a new one, read from the start.
bool openUnfoldedJournal(HalFile& f, const uint32_t foldedBytes, const uint32_t foldedHash) {
  if (!Storage.openFileForRead("STAT", journalPath().c_str(), f)) return false;
  if (foldedBytes == 0 || f.size() < foldedBytes) return skipBytes(f, JOURNAL_HEADER_BYTES);
  uint32_t hash = FNV_BASIS;
  uint8_t buf[64];
  size_t left = foldedBytes;
  while (left) {
    const size_t k = std::min(left, sizeof(buf));
    if (f.read(buf, k) != static_cast<int>(k)) break;
    hash = fnv1a(hash, buf, k);
    left -= k;
  }
  if (left == 0 && hash == foldedHash) return true;
  f.close();
  return Storage.openFileForRead("STAT", journalPath().c_str(), f) && skipBytes(f, JOURNAL_HEADER_BYTES);
}

size_t countJournalRecords() {
  HalFile f;
  if (!Storage.openFileForRead("STAT", journalPath().c_str(), f) || !skipBytes(f, JOURNAL_HEADER_BYTES)) return 0;
  JournalRecord r;
  size_t n = 0;
  while (readRecord(f, r)) n++;
  return n;
}

// Reads only the trailer of a stats file: the journal prefix it holds (see compact()). False when the
// file does not read whole to its trailer. Same bounds as the loader, so the two agree on a file.
bool readFoldMarker(const std::string& path, uint32_t& bytes, uint32_t& hash) {
  bytes = hash = 0;
  HalFile f;
  if (!Storage.openFileForRead("STAT", path.c_str(), f) || f.size() < 5) return false;
  uint8_t version = 0;
  uint16_t count = 0, finished = 0, n = 0;
  if (f.read(&version, 1) != 1 || version < 4 || f.read(reinterpret_cast<uint8_t*>(&count), 2) != 2 ||
      f.read(reinterpret_cast<uint8_t*>(&finished), 2) != 2 || !skipBytes(f, count * sizeof(DailyReading)) ||
      f.read(reinterpret_cast<uint8_t*>(&n), 2) != 2 || n > 500) {
    return false;
  }
  std::string s;
  for (uint16_t i = 0; i < n; i++) {
    if (!readString(f, s, MAX_STORED_PATH)) return false;
  }
  if (f.read(reinterpret_cast<uint8_t*>(&n), 2) != 2 || n > ReadingStatsStore::MAX_BOOKS) return false;
  for (uint16_t i = 0; i < n; i++) {
    uint8_t langLen = 0;
    if (!readString(f, s, MAX_STORED_PATH) || f.read(&langLen, 1) != 1 || langLen > MAX_STORED_LANGUAGE ||
        !skipBytes(f, langLen + 8u)) {
      return false;
    }
  }
  uint32_t b = 0, h = 0;
  if (f.read(reinterpret_cast<uint8_t*>(&n), 2) != 2 || !skipBytes(f, n * 10u) ||
      f.read(reinterpret_cast<uint8_t*>(&b), 4) != 4 || f.read(reinterpret_cast<uint8_t*>(&h), 4) != 4) {
    return false;
  }
  bytes = b;
  hash = h;
  return true;
}

// A save swaps its temp file in by removing the old file first; a power loss in between leaves only
// the temp. It is adopted before anything reads or writes the stats -- but only when it reads whole
// to its trailer: a temp cut off mid-write is dropped, and the journal still holds everything.
void recoverInterruptedSave() {
  const std::string tmpPath = statsPath() + ".tmp";
  if (Storage.exists(statsPath().c_str()) || !Storage.exists(tmpPath.c_str())) return;
  uint32_t bytes = 0, hash = 0;
  if (readFoldMarker(tmpPath, bytes, hash)) {
    Storage.rename(tmpPath.c_str(), statsPath().c_str());
  } else {
    Storage.remove(tmpPath.c_str());
  }
}
}  // namespace

bool ReadingStatsStore::replayJournal() {
  HalFile f;
  if (!openUnfoldedJournal(f, foldedJournalBytes, foldedJournalHash)) return true;  // nothing recorded
  // A record cut short by a power loss ends the replay; everything before it counts.
  JournalRecord r;
  while (readRecord(f, r)) {
    if (r.minutes > 0) {
      if (r.year >= 2020) {  // the loader drops unset-clock days; the per-book total keeps them
        addMinutes(r.year, r.month, r.day, r.minutes);
        addLanguageMinutes(r.language.c_str(), r.minutes, r.year, r.month, r.day);
      }
      addBookMinutes(r.bookPath.c_str(), r.language.c_str(), r.minutes, r.year, r.month, r.day);
    }
    if (!r.finishedPath.empty()) markBookFinished(r.finishedPath);
  }
  return true;
}

bool ReadingStatsStore::loadFromFile() {
  if (loaded) return true;
  recoverInterruptedSave();
  const size_t pending = countJournalRecords();
  if (Storage.exists(statsPath().c_str()) && !loadMainFile(pending)) {
    LOG_ERR("STAT", "Stats not loaded (unreadable or no heap: maxAlloc=%u, %u journal records)", ESP.getMaxAllocHeap(),
            static_cast<unsigned>(pending));
    release();
    return false;
  }
  replayJournal();
  loaded = true;
  return true;
}

void ReadingStatsStore::release() {
  std::vector<DailyReading>().swap(days);
  std::vector<std::string>().swap(finishedBookPaths);
  std::vector<BookReading>().swap(books);
  std::vector<LanguageDaily>().swap(languageDays);
  booksFinished = 0;
  foldedJournalBytes = 0;
  foldedJournalHash = 0;
  loaded = false;
}

bool ReadingStatsStore::record(const Change& change) {
  std::string rec;
  rec.reserve(JOURNAL_HEADER_BYTES + 13 + MAX_STORED_LANGUAGE +
              (change.bookPath ? strnlen(change.bookPath, MAX_STORED_PATH) : 0) +
              (change.finishedPath ? strnlen(change.finishedPath, MAX_STORED_PATH) : 0));
  rec.push_back(static_cast<char>(JOURNAL_RECORD_MAGIC));
  rec.append(reinterpret_cast<const char*>(&change.year), 2);
  rec.push_back(static_cast<char>(change.month));
  rec.push_back(static_cast<char>(change.day));
  rec.append(reinterpret_cast<const char*>(&change.minutes), 2);
  appendString(rec, change.language, MAX_STORED_LANGUAGE);
  appendString(rec, change.bookPath, MAX_STORED_PATH);
  appendString(rec, change.finishedPath, MAX_STORED_PATH);

  std::lock_guard<std::mutex> lock(journalMutex());
  HalFile f;
  if (!Storage.openFileForAppend("STAT", journalPath().c_str(), f)) return false;
  const size_t before = f.size();
  if (before == 0) {
    const uint32_t nonce = esp_random();
    std::string header(1, static_cast<char>(JOURNAL_HEADER_MAGIC));
    header.append(reinterpret_cast<const char*>(&nonce), 4);
    rec.insert(0, header);
  }
  // A short append is cut back off: a torn record would end every replay before the ones after it.
  if (f.write(rec.data(), rec.size()) != rec.size()) {
    f.truncate(before);
    f.close();
    return false;
  }
  if (f.close()) return true;
  // A failed close may still have committed the record; retrying it then would count it twice.
  HalFile check;
  return Storage.openFileForRead("STAT", journalPath().c_str(), check) && check.size() == before + rec.size();
}

ReadingStatsStore::Summary ReadingStatsStore::summarize(const uint16_t todayYear, const uint8_t todayMonth,
                                                        const uint8_t todayDay, const char* language) {
  Summary out;
  recoverInterruptedSave();
  const int today = daysSinceEpoch(todayYear, todayMonth, todayDay);
  char lang[4] = {};
  if (language && *language) normalizeLanguage(language, lang);

  // The table path, for what the stream below does not handle (older formats, unsorted days).
  const auto fromTables = [&]() {
    // Held so a record() on the main loop cannot add to the journal between its count and replay.
    std::lock_guard<std::mutex> lock(journalMutex());
    Summary t;
    if (loadFromFile()) {
      t.streak = getStreak(todayYear, todayMonth, todayDay);
      t.booksFinished = getBooksFinished();
      if (lang[0]) t.languageBooksFinished = getBooksFinished(lang);
    }
    release();
    return t;
  };

  // The journal past what the file holds, condensed to what the numbers need: no strings kept.
  struct Entry {
    int day;
    uint16_t minutes;
    uint32_t book;      // hash of the book path, 0 for none
    uint32_t finished;  // hash of the finished path, 0 for none
    char language[4];
    bool hasLanguage;
  };
  std::vector<Entry> journal;
  {
    uint32_t foldedBytes = 0, foldedHash = 0;
    readFoldMarker(statsPath(), foldedBytes, foldedHash);
    HalFile j;
    if (openUnfoldedJournal(j, foldedBytes, foldedHash)) {
      journal.reserve(countJournalRecords());
      JournalRecord r;
      while (readRecord(j, r)) {
        Entry e{daysSinceEpoch(r.year, r.month, r.day), r.minutes, 0, 0, {}, !r.language.empty()};
        if (r.minutes > 0 && !r.bookPath.empty()) e.book = pathHash(r.bookPath);
        if (!r.finishedPath.empty()) e.finished = pathHash(r.finishedPath);
        normalizeLanguage(r.language.c_str(), e.language);
        if (r.minutes > 0 && r.year < 2020) e.minutes = 0;  // dropped from days, as on replay
        journal.push_back(e);
      }
    }
  }
  // Journal minutes per day, ascending.
  std::vector<std::pair<int, uint32_t>> journalDays;
  journalDays.reserve(journal.size());
  for (const auto& e : journal) {
    if (e.minutes == 0) continue;
    auto it = std::lower_bound(journalDays.begin(), journalDays.end(), std::make_pair(e.day, uint32_t{0}));
    if (it != journalDays.end() && it->first == e.day) {
      it->second += e.minutes;
    } else {
      journalDays.insert(it, {e.day, e.minutes});
    }
  }

  HalFile f;
  const bool hasFile = Storage.openFileForRead("STAT", statsPath().c_str(), f) && f.size() >= 5;
  uint8_t version = 0;
  uint16_t count = 0, headerFinished = 0;
  if (hasFile &&
      (f.read(&version, 1) != 1 || version != STATS_VERSION || f.read(reinterpret_cast<uint8_t*>(&count), 2) != 2 ||
       f.read(reinterpret_cast<uint8_t*>(&headerFinished), 2) != 2)) {
    f.close();
    return fromTables();
  }

  // Days, file and journal merged in date order; the run of read days ending today is the streak.
  int prev = INT32_MIN, run = 0, lastFileDay = INT32_MIN;
  size_t jd = 0;
  const auto takeDay = [&](const int e, const uint32_t minutes) {
    if (minutes == 0 || e > today) return;  // as getStreak(): empty days and future dates do not count
    run = (e == prev + 1) ? run + 1 : 1;
    prev = e;
  };
  const size_t skip = count > MAX_DAYS ? count - MAX_DAYS : 0;  // the loader keeps the newest MAX_DAYS
  for (size_t i = 0; hasFile && i < count; i++) {
    DailyReading d;
    if (f.read(reinterpret_cast<uint8_t*>(&d), sizeof(d)) != sizeof(d)) {
      f.close();
      return fromTables();
    }
    if (i < skip || d.year < 2020) continue;
    const int e = daysSinceEpoch(d.year, d.month, d.day);
    if (e < lastFileDay) {  // unsorted: the loader sorts, so leave it to the tables
      f.close();
      return fromTables();
    }
    lastFileDay = e;
    uint32_t minutes = d.minutesRead;
    for (; jd < journalDays.size() && journalDays[jd].first < e; jd++)
      takeDay(journalDays[jd].first, journalDays[jd].second);
    if (jd < journalDays.size() && journalDays[jd].first == e) minutes += journalDays[jd++].second;
    takeDay(e, minutes);
  }
  for (; jd < journalDays.size(); jd++) takeDay(journalDays[jd].first, journalDays[jd].second);
  out.streak = prev == today ? run : 0;

  // Finished books: hashes of their paths, then their languages from the per-book block.
  std::vector<uint32_t> finished;
  bool pathsIntact = false;
  uint16_t pathCount = 0;
  std::string s;
  if (hasFile && f.read(reinterpret_cast<uint8_t*>(&pathCount), 2) == 2 && pathCount <= 500) {
    pathsIntact = true;
    finished.reserve(pathCount + journal.size());
    for (uint16_t i = 0; i < pathCount; i++) {
      if (!readString(f, s, MAX_STORED_PATH)) {
        pathsIntact = false;
        break;
      }
      finished.push_back(pathHash(s));
    }
  }
  uint16_t booksFinished = hasFile ? (pathsIntact ? static_cast<uint16_t>(finished.size()) : headerFinished) : 0;
  for (const auto& e : journal) {
    if (!e.finished || std::find(finished.begin(), finished.end(), e.finished) != finished.end()) continue;
    finished.push_back(e.finished);
    booksFinished = std::max(booksFinished, static_cast<uint16_t>(finished.size()));  // as markBookFinished()
  }
  out.booksFinished = booksFinished;

  if (lang[0]) {
    // Language per finished book: from the per-book block, then as the journal sets it.
    std::vector<std::pair<uint32_t, std::array<char, 4>>> finishedLang;
    uint16_t bookCount = 0;
    if (pathsIntact && f.read(reinterpret_cast<uint8_t*>(&bookCount), 2) == 2 && bookCount <= MAX_BOOKS) {
      std::string bookLang;
      for (uint16_t i = 0; i < bookCount; i++) {
        uint8_t langLen = 0;
        if (!readString(f, s, MAX_STORED_PATH) || f.read(&langLen, 1) != 1 || langLen > MAX_STORED_LANGUAGE) break;
        bookLang.assign(langLen, '\0');
        if ((langLen && f.read(reinterpret_cast<uint8_t*>(&bookLang[0]), langLen) != langLen) || !skipBytes(f, 8)) {
          break;
        }
        const uint32_t h = pathHash(s);
        if (std::find(finished.begin(), finished.end(), h) == finished.end()) continue;
        std::array<char, 4> code{};
        normalizeLanguage(bookLang.c_str(), code.data());
        finishedLang.push_back({h, code});
      }
    }
    for (const auto& e : journal) {
      if (!e.book || std::find(finished.begin(), finished.end(), e.book) == finished.end()) continue;
      auto it =
          std::find_if(finishedLang.begin(), finishedLang.end(), [&e](const auto& x) { return x.first == e.book; });
      if (it == finishedLang.end()) {
        finishedLang.push_back({e.book, {}});
        it = finishedLang.end() - 1;
      } else if (!e.hasLanguage) {
        continue;  // an empty tag never overwrites a known one (addBookMinutes)
      }
      memcpy(it->second.data(), e.language, 4);
    }
    for (const auto& x : finishedLang) {
      if (memcmp(x.second.data(), lang, 4) == 0) out.languageBooksFinished++;
    }
  }
  return out;
}

bool ReadingStatsStore::compact() {
  std::lock_guard<std::mutex> lock(journalMutex());
  recoverInterruptedSave();
  const bool hasJournal = Storage.exists(journalPath().c_str());
  bool outdated = false;  // an older format: one rewrite upgrades it
  {
    HalFile f;
    uint8_t version = 0;
    if (Storage.openFileForRead("STAT", statsPath().c_str(), f) && f.size() >= 5 && f.read(&version, 1) == 1) {
      outdated = version != STATS_VERSION;
    }
  }
  if (!hasJournal && !outdated) return true;

  release();
  uint32_t bytes = 0, hash = 0;
  if (hasJournal) journalDigest(bytes, hash);
  // The saved file records which journal it now holds, so a removal cut short here never counts the
  // journal twice; until that save lands the journal is untouched.
  bool ok = loadFromFile();
  if (ok) {
    foldedJournalBytes = bytes;
    foldedJournalHash = hash;
    ok = saveToFile();
  }
  if (ok && hasJournal) Storage.remove(journalPath().c_str());
  if (!ok) LOG_ERR("STAT", "Stats journal not folded in; kept for the next try");
  release();
  return ok;
}
