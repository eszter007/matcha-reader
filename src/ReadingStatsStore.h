#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct DailyReading {
  uint16_t year;
  uint8_t month;
  uint8_t day;
  uint16_t minutesRead;
};

// Per-book totals, alongside the per-day ones. Carries the book's language so reading time can
// later be broken down by language (issue #38) -- captured now, displayed nowhere yet.
struct BookReading {
  std::string path;      // book file (EPUB/TXT/XTC) or manga folder -- the same identity used elsewhere
  std::string language;  // BCP-47/ISO-639 tag, empty when the book declares none. Short enough for SSO.
  uint32_t minutesRead = 0;
  int32_t lastReadDay = 0;  // days since the civil epoch, for eviction and (later) recency sorting
};

// One day's minutes in one language -- what a "Japanese only" view needs (issue #38): with a
// day granularity it yields the same streak/calendar/week/total numbers the overall stats show,
// per language. A per-book-per-day matrix would give the same answers and more, but 150 books x
// 365 days does not fit in DRAM, so the book dimension keeps only a running total.
struct LanguageDaily {
  uint16_t year;
  uint8_t month;
  uint8_t day;
  // Primary subtag only, lowercased and NUL-padded: "ja-JP" and "ja" both bucket as "ja".
  // Fixed-size to keep this POD (no allocation, written and read as a flat record). Empty
  // first byte = language unknown (TXT/XTC, manga converted before meta.bin carried a tag).
  char language[4];
  uint16_t minutesRead;
};

class ReadingStatsStore {
  static ReadingStatsStore instance;

  // The day history is unbounded: a calendar you can page back through is worth little if it
  // silently forgets the year before last, which the old fixed 365-entry ring did.
  //
  // The cost is 6 bytes per day READ, so a reader who reads every single day adds ~2.2KB of DRAM
  // per year -- the same order as the fixed array it replaces (2.19KB), which was paid in full
  // from day one whether or not it was used. Years two onward are the new cost. If that ever
  // stops being acceptable, the fix is to page the history off SD rather than to cap it again.
  //
  // `days` is kept sorted by date (see addMinutes), which lets getStreak and getLongestStreak
  // run as single passes with no allocation and no fixed-size scratch buffer.
  static constexpr size_t MAX_DAYS_SANE = 40000;  // ~110 years; guards a corrupt length field only
  // Bounded because each entry heap-allocates its path (~40-80 bytes) and the store is a
  // long-lived singleton. When full, the least recently read book is dropped -- its minutes
  // stay counted in the per-day totals, which are the numbers the UI shows today.
  static constexpr size_t MAX_BOOKS = 150;
  std::vector<DailyReading> days;  // sorted ascending by date
  uint16_t booksFinished = 0;
  std::vector<std::string> finishedBookPaths;
  std::vector<BookReading> books;
  // Per-day-per-language history, also unbounded: it backs a calendar of its own, and the 512
  // entry ring that used to hold it dropped the oldest days silently -- barely over a year for a
  // single language, and under nine months for someone reading in two.
  //
  // 10 bytes per (day, language), so ~3.6KB per year per language actually read. Dearer than the
  // overall history (6 bytes/day) because each record carries its own tag; if that ever matters,
  // the fix is a language table with a one-byte index, not a cap.
  //
  // No sanity bound here, unlike MAX_DAYS_SANE: the on-disk count is a uint16, so the format
  // itself caps a load at 65535 records (~90 years across two languages). A separate constant
  // above that is a check that can never fire.
  std::vector<LanguageDaily> languageDays;  // sorted ascending by date

 public:
  static ReadingStatsStore& getInstance() { return instance; }

  void addMinutes(uint16_t year, uint8_t month, uint8_t day, uint16_t minutes);
  // Same minutes, attributed to one book. Called alongside addMinutes(); a null or empty
  // bookPath is ignored. A non-empty language overwrites a previously unknown one (a book
  // converted before meta.bin carried a language tag starts blank and fills in after a
  // re-convert).
  //
  // const char* rather than const std::string&: the readers call this via flushReadingStats()
  // on every loop() tick, and a std::string parameter makes each of those ticks construct a
  // temporary -- a heap allocation per tick, on the device least able to afford the churn.
  void addBookMinutes(const char* bookPath, const char* language, uint16_t minutes, uint16_t year, uint8_t month,
                      uint8_t day);
  const std::vector<BookReading>& getBooks() const { return books; }
  // Same minutes again, bucketed by day and language. Unlike addBookMinutes this DOES record an
  // empty language, as its own "unknown" bucket -- dropping it would make the per-language days
  // silently disagree with the overall ones for anyone reading TXT/XTC.
  void addLanguageMinutes(const char* language, uint16_t minutes, uint16_t year, uint8_t month, uint8_t day);
  const std::vector<LanguageDaily>& getLanguageDays() const { return languageDays; }
  // language is matched the way it is stored: primary subtag, lowercase ("ja"). Empty = unknown.
  uint16_t getMinutesForDay(const char* language, uint16_t year, uint8_t month, uint8_t day) const;
  uint32_t getTotalMinutes(const char* language) const;
  void markBookFinished(const std::string& bookPath);

  // ---- per-language views, mirroring the overall ones above ----
  // One entry per language ever read, most-read first. Returned by value into a caller-supplied
  // vector so the store keeps no derived state; the list is short (one per language, not per day).
  struct LanguageSummary {
    char code[4];  // primary subtag, lowercase; empty first byte = unknown
    uint32_t minutes;
  };
  void getLanguages(std::vector<LanguageSummary>& out) const;

  int getStreak(const char* language, uint16_t todayYear, uint8_t todayMonth, uint8_t todayDay) const;
  int getLongestStreak(const char* language) const;
  int getDaysRead(const char* language) const;
  uint16_t getMinutesThisWeek(const char* language, uint16_t todayYear, uint8_t todayMonth, uint8_t todayDay) const;
  void getWeekStatus(const char* language, uint16_t todayYear, uint8_t todayMonth, uint8_t todayDay, int todayDow,
                     bool readDays[7]) const;
  void getMonthStatus(const char* language, uint16_t year, uint8_t month, bool out[32]) const;
  int getDaysReadInMonth(const char* language, uint16_t year, uint8_t month) const;
  // Finished books whose recorded language matches. Only as complete as the per-book block, which
  // is capped and LRU-evicted: a book finished long ago may have lost its language, so this can
  // undercount. It never overcounts, which is the direction that matters for a headline number.
  uint16_t getBooksFinished(const char* language) const;

  int getStreak(uint16_t todayYear, uint8_t todayMonth, uint8_t todayDay) const;
  int getLongestStreak() const;
  int getDaysRead() const;
  uint32_t getTotalMinutes() const;
  uint16_t getBooksFinished() const { return booksFinished; }

  uint16_t getMinutesForDay(uint16_t year, uint8_t month, uint8_t day) const;
  uint16_t getMinutesThisWeek(uint16_t todayYear, uint8_t todayMonth, uint8_t todayDay) const;
  bool hasReadToday(uint16_t year, uint8_t month, uint8_t day) const;

  void getWeekStatus(uint16_t todayYear, uint8_t todayMonth, uint8_t todayDay, int todayDow, bool readDays[7]) const;

  // Get reading status for every day of a given month (1-indexed, out[1]..out[31]).
  void getMonthStatus(uint16_t year, uint8_t month, bool out[32]) const;

  // Count days read in a given month.
  int getDaysReadInMonth(uint16_t year, uint8_t month) const;

  bool saveToFile() const;
  bool loadFromFile();
};

#define READING_STATS_STORE ReadingStatsStore::getInstance()
