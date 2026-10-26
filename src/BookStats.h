#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Same shape as DailyReading: both feed the same calendar widget.
struct BookDay {
  uint16_t year;
  uint8_t month;
  uint8_t day;
  uint16_t minutes;
};

// FNV-1a of a path's last component: the name a book keeps across devices, so "/books/a.epub" and "a.epub"
// agree. Persisted (the per-book stats download), so it must not change.
inline uint32_t bookNameHash(const char* path, const size_t len) {
  size_t start = 0;
  for (size_t i = 0; i < len; i++) {
    if (path[i] == '/') start = i + 1;
  }
  uint32_t h = 2166136261u;
  for (size_t i = start; i < len; i++) h = (h ^ static_cast<uint8_t>(path[i])) * 16777619u;
  return h;
}

// Per-book reading history: sessions, minutes, days.
//
// Not a singleton, not resident: one book's history is loaded on demand and dropped, so the
// steady-state DRAM cost is zero. This is the day dimension that would not fit in
// ReadingStatsStore's per-book block (see LanguageDaily).
//
// Stored in /system/bookstats/, not the book's .crosspoint/ cache: clearing that cache is a
// documented fix for rendering faults and must not erase reading history.
class BookStats {
 public:
  // Bounded by MEMORY: std::vector allocates with new and this firmware builds -fno-exceptions,
  // so a failed reserve() aborts rather than returning null. 2000 days is five and a half years
  // of reading one book every day, for 12KB; a longer file keeps its most recent 2000.
  static constexpr size_t MAX_DAYS = 2000;

  // Loads this book's history. Returns false only on a read error; a book with no history yet
  // loads clean and empty, which is the normal first-open case. withServerCopy also takes in what the
  // last Refresh Stats downloaded for it (bookdownload): for screens that show it and the first open,
  // not for every save, which would scan that file each time.
  bool load(const char* bookPath, bool withServerCopy = false);
  bool save() const;

  // One session = one opening. Called from each reader's onEnter, which runs once per open:
  // readers survive their own menus (startActivityForResult), so those do not re-count.
  //
  // Accepted inaccuracy: a sleep-wake re-enters the reader (main.cpp, lastSleepFromReader) and
  // counts again, so a read broken by sleep reads as several sessions.
  static bool recordOpen(const char* bookPath);

  // Sessions are counted by recordOpen, not here: an opening counts even if it banks no minute.
  void recordMinutes(uint16_t year, uint8_t month, uint8_t day, uint16_t minutes);

  // Adds to the dictionary counters and saves: words whose definition was shown, and sentences
  // saved for Anki. One SD write per call, so lookup screens batch theirs until they close.
  static bool addCounts(const char* bookPath, uint32_t lookups, uint32_t sentences);

  // Adds reading minutes to the day and saves (the readers' periodic flush).
  static bool addMinutes(const char* bookPath, uint16_t year, uint8_t month, uint8_t day, uint16_t minutes);

  // Loads the book's stats into `out` and stores today as its finish day on a first finish, with the
  // lookup and sentence counts at that moment. A stored day always wins. Without one, a reopened book
  // shows its last reading day before today, which is not saved: a guess must not become the stored
  // date. A book finished before the counts were kept has them frozen from this visit.
  static bool recordFinish(const char* bookPath, uint16_t year, uint8_t month, uint8_t day, bool firstFinish,
                           BookStats& out);
  // Forgets the finish day and the counts kept with it (Mark as Unread).
  static bool clearFinish(const char* bookPath);
  // False when no finish day is stored or guessed.
  bool getFinishDay(uint16_t& year, uint8_t& month, uint8_t& day) const;

  uint32_t getSessions() const { return sessions; }
  uint32_t getLookups() const { return lookups; }
  uint32_t getSentencesSaved() const { return sentencesSaved; }
  // The counts when the book was finished; the current ones before that.
  uint32_t getLookupsAtFinish() const { return hasFinishCounts ? finishLookups : lookups; }
  uint32_t getSentencesAtFinish() const { return hasFinishCounts ? finishSentences : sentencesSaved; }
  // The read that finished the book, up to and including the finish day: its minutes, and the calendar
  // days from its first reading day to the finish (0 with none). Reopenings after it add nothing.
  void readUntil(uint16_t year, uint8_t month, uint8_t day, uint32_t& minutes, int& daysToFinish) const;
  uint32_t getTotalMinutes() const;
  int getDaysRead() const { return static_cast<int>(days.size()); }
  // Mean minutes per opening, rounded. Zero-minute opens stay in the divisor: excluding them
  // would report an average longer than any real session.
  uint32_t getAverageSessionMinutes() const;

  void getMonthStatus(uint16_t year, uint8_t month, bool out[32]) const;
  int getDaysReadInMonth(uint16_t year, uint8_t month) const;

  const std::vector<BookDay>& getDays() const { return days; }

  static std::string filePathFor(const char* bookPath);

 private:
  // Takes in what the sync server has for this book (bookdownload): the larger value per day and the
  // larger session count. True when that changed anything.
  bool mergeDownloaded(bool& finishedOnServer);
  bool loadLocal(const char* bookPath);
  // Set by load(): the server's copy changed this book and calls it finished.
  bool restoredFinished = false;

  std::string bookPath;
  uint32_t sessions = 0;
  uint32_t lookups = 0;
  uint32_t sentencesSaved = 0;
  std::vector<BookDay> days;
  BookDay finished{};  // first finish day; year 0 when unknown (minutes unused)
  bool hasFinishCounts = false;
  uint32_t finishLookups = 0;
  uint32_t finishSentences = 0;
};
