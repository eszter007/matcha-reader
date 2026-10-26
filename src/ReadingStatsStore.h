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

// The sync account's numbers from the server's overview (GET /api/v1/stats/overview), counting
// every device: one digest for all languages, then one per language. Days count from 2000-01-01.
struct StatsDigest {
  char code[4];       // primary subtag, lowercase; empty for all languages
  int32_t anchorDay;  // the server's today when it answered
  uint16_t days;      // bits used in history
  uint16_t longestStreak;
  uint16_t booksFinished;
  uint32_t readingDays;
  uint32_t minutes;
  uint32_t weekMinutes;  // Monday through anchorDay
  uint8_t history[92];   // bit N, LSB first: day anchorDay - N was a reading day
};

class ReadingStatsStore {
  static ReadingStatsStore instance;

  // Day history the calendar pages back through; the old 365-entry ring silently forgot the
  // year before last. 6 bytes/day, ~2.2KB per year of daily reading.
  //
  // Bounded by MEMORY, not by a corruption guard: std::vector allocates with new, and this
  // firmware builds -fno-exceptions, so a failed reserve() aborts rather than returning null.
  // 3650 days is a decade of reading every day for 22KB. A file holding more keeps its most
  // recent 3650 (see loadFromFile, which consumes the rest to stay aligned).
  static constexpr size_t MAX_DAYS = 3650;

 public:
  // Bounded because each entry heap-allocates its path (~40-80 bytes). When full, the least
  // recently read book is dropped -- its minutes stay counted in the per-day totals, which are the
  // numbers the UI shows today. Public for the file reader that checks it without loading.
  static constexpr size_t MAX_BOOKS = 150;

 private:
  std::vector<DailyReading> days;  // sorted ascending by date
  uint16_t booksFinished = 0;
  std::vector<std::string> finishedBookPaths;
  std::vector<BookReading> books;
  // Same, for the per-language calendar; the old 512-entry ring held barely a year for one
  // language. 10 bytes per (day, language) -- dearer than `days` because each record carries
  // its tag -- so a smaller cap buys the same 25KB. Shrink the record with a language table
  // before raising this.
  static constexpr size_t MAX_LANG_DAYS = 2500;
  std::vector<LanguageDaily> languageDays;  // sorted ascending by date
  bool loaded = false;                      // set by loadFromFile(), cleared by release()
  // The account digests ([0] all languages), loaded only for display: with them the getters count
  // a day read on any device, plus this reader's own minutes since the server answered.
  std::vector<StatsDigest> account;
  void loadAccount();
  const StatsDigest* accountFor(const char* language) const;  // nullptr language: all languages
  bool readOn(const char* language, const StatsDigest* g, uint16_t year, uint8_t month, uint8_t day) const;
  template <typename F>
  void eachLocalDay(const char* language, F&& f) const;
  int accountStreak(const char* language, const StatsDigest& g, uint16_t y, uint8_t m, uint8_t d) const;
  int accountDaysRead(const char* language, const StatsDigest& g) const;
  uint32_t accountMinutes(const char* language, const StatsDigest& g) const;
  uint16_t accountWeekMinutes(const char* language, const StatsDigest& g, uint16_t y, uint8_t m, uint8_t d) const;
  // The journal prefix (byte count and hash) this file already holds; replay starts after it.
  // Stored after the last block, where older readers stop.
  uint32_t foldedJournalBytes = 0;
  uint32_t foldedJournalHash = 0;

  bool loadMainFile(size_t pendingRecords);
  bool mergeDownloadFile();
  bool mergeBookDownload(const std::string& list);
  bool replayJournal();

 public:
  static ReadingStatsStore& getInstance() { return instance; }

  void addMinutes(uint16_t year, uint8_t month, uint8_t day, uint16_t minutes);
  // The day number BookReading::lastReadDay is counted in.
  static int32_t dayNumber(uint16_t year, uint8_t month, uint8_t day);
  // Raises a day to `minutes` if it holds fewer; for minutes read back from the sync server, which
  // are this device's own (or another CrossPoint reader's on the same account), so taking the larger
  // value never counts a session twice.
  void mergeMinutes(uint16_t year, uint8_t month, uint8_t day, uint16_t minutes);
  // The same for one language's day (see addLanguageMinutes).
  void mergeLanguageMinutes(const char* language, uint16_t minutes, uint16_t year, uint8_t month, uint8_t day);
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

  // ---- per-language views, mirroring the overall ones ----
  // Every language read, most-read first. Written into the caller's vector so the store keeps
  // no derived state.
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
  // Can undercount: language lives in the per-book block, which is capped and LRU-evicted, so a
  // book finished long ago may have lost its tag. Never overcounts.
  uint16_t getBooksFinished(const char* language) const;

  int getStreak(uint16_t todayYear, uint8_t todayMonth, uint8_t todayDay) const;
  int getLongestStreak() const;
  int getDaysRead() const;
  uint32_t getTotalMinutes() const;
  uint16_t getBooksFinished() const;
  bool isFinished(const std::string& bookPath) const;
  const std::vector<DailyReading>& getDays() const { return days; }

  uint16_t getMinutesForDay(uint16_t year, uint8_t month, uint8_t day) const;
  uint16_t getMinutesThisWeek(uint16_t todayYear, uint8_t todayMonth, uint8_t todayDay) const;

  void getWeekStatus(uint16_t todayYear, uint8_t todayMonth, uint8_t todayDay, int todayDow, bool readDays[7]) const;

  // Get reading status for every day of a given month (1-indexed, out[1]..out[31]).
  void getMonthStatus(uint16_t year, uint8_t month, bool out[32]) const;

  // Count days read in a given month.
  int getDaysReadInMonth(uint16_t year, uint8_t month) const;

  // Writes the tables through a temp file, so an interrupted save leaves the old file whole.
  bool saveToFile() const;
  // Reads the stats file and replays the journal on top. Returns at once if already loaded; false
  // (with nothing held) when the file is unreadable or its tables do not fit the heap right now.
  bool loadFromFile();
  // Frees everything loaded. Screens that show stats load, read and release, so the store holds no
  // heap between uses.
  void release();

  // A change recorded without loading anything: one record appended to a small journal. What the
  // reader saves while a chapter is laid out (session minutes, a finished book) goes through here.
  struct Change {
    uint16_t year = 0;
    uint8_t month = 0;
    uint8_t day = 0;
    uint16_t minutes = 0;                // 0 = no reading session
    const char* bookPath = nullptr;      // book the minutes belong to
    const char* language = nullptr;      // that book's language tag
    const char* finishedPath = nullptr;  // book marked finished, or nullptr
  };
  static bool record(const Change& change);
  // Folds the journal and any saved download into the stats file (load, replay, merge, save, remove
  // them). Run where the heap is free -- boot -- so the tables are only ever built there or on a stats
  // screen.
  bool compact();

  // Daily minutes downloaded from the sync server, kept on SD until the next compact() folds them in
  // with mergeMinutes(). Written with WiFi up, when the tables may not fit; a newer download replaces
  // an unmerged one, and one that cannot be folded in yet stays for the next boot.
  static bool saveDownload(const std::vector<DailyReading>& downloaded);
  // A saved download the next compact() has not folded in yet.
  static bool hasPendingDownload();
  // A stats file exists: what a download since the last one would add to.
  static bool hasStatsFile();
  // The server's overview for the sync account, for the All devices view; removeAccount() drops it
  // when another account signs in.
  static bool saveAccount(const std::vector<StatsDigest>& digests);
  static void removeAccount();

  // The numbers the end-of-book summary shows, read straight from the file and the journal with no
  // tables built: that screen runs with the book still in memory. Same values as the getters.
  struct Summary {
    int streak = 0;
    // The stamp's "Nth book": finishedPath's place in the finish order when it was finished before (a
    // reopening keeps its number), else every finished book.
    uint16_t booksFinished = 0;
    uint16_t languageBooksFinished = 0;  // the same within summarize()'s language; 0 without one
    bool firstFinish = false;  // finishedPath's latest record is its first finish; false when unsure (table path)
  };
  // finishedPath: a book just record()ed as finished, to learn whether this is a reopening.
  Summary summarize(uint16_t todayYear, uint8_t todayMonth, uint8_t todayDay, const char* language,
                    const char* finishedPath = nullptr);

  // Holds the full tables for as long as it lives (a stats screen). loaded() is false when they
  // could not be read or did not fit the heap.
  class Loaded {
   public:
    // withAccount: the getters also count the sync account's other devices (Insights' "All devices").
    // Display only: never with a save or upload.
    explicit Loaded(const bool withAccount = false) : ok_(getInstance().loadFromFile()) {
      if (ok_ && withAccount) getInstance().loadAccount();
    }
    ~Loaded() { getInstance().release(); }
    Loaded(const Loaded&) = delete;
    Loaded& operator=(const Loaded&) = delete;
    bool loaded() const { return ok_; }

   private:
    bool ok_;
  };
};

#define READING_STATS_STORE ReadingStatsStore::getInstance()
