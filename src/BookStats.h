#pragma once
#include <cstdint>
#include <string>
#include <vector>

// One day this book was read. 6 bytes; the same shape as DailyReading, deliberately -- both
// calendars answer the same question and the widget that draws them takes either.
struct BookDay {
  uint16_t year;
  uint8_t month;
  uint8_t day;
  uint16_t minutes;
};

// Per-book reading history: sessions, minutes, and every day the book was opened.
//
// Deliberately NOT a singleton and NOT held in DRAM alongside ReadingStatsStore. Only one book's
// history is ever on screen, so this is loaded on demand, read, and dropped -- the steady-state
// cost is zero. ReadingStatsStore's per-book block stays what it is (a running total for all 150
// books at once); this is the day dimension that would not fit there, kept per book on SD where
// it costs nothing until asked for. See the header comment on LanguageDaily for why the in-DRAM
// matrix was rejected.
//
// Lives in /system/bookstats/, NOT in the book's .crosspoint/ cache directory: "delete
// .crosspoint/ to fix rendering" is a documented troubleshooting step, and reading history must
// not be collateral damage of a render-cache clear.
class BookStats {
 public:
  // Loads this book's history. Returns false only on a read error; a book with no history yet
  // loads clean and empty, which is the normal first-open case.
  bool load(const char* bookPath);
  bool save() const;

  // One session = one opening of the book. Called from each reader's onEnter, which runs exactly
  // once per open: the reader stays alive behind its menus and settings screens
  // (startActivityForResult), so browsing them mid-chapter does not count again.
  //
  // Known and accepted: waking the device back into a book re-enters the reader (see main.cpp's
  // lastSleepFromReader resume) and so counts a further session. A long read broken up by sleep
  // therefore reads as several sessions, which also pulls the average session down. Suppressing
  // it would mean threading a resumed-vs-opened flag through every reader; judged not worth it.
  //
  // Load-increment-save in one call, because the caller has nothing else to do with the object.
  static bool recordOpen(const char* bookPath);

  // Adds minutes to the given day. Sessions are counted by recordOpen, not here -- a reading
  // session is an opening of the book, whether or not it lasted long enough to bank a minute.
  void recordMinutes(uint16_t year, uint8_t month, uint8_t day, uint16_t minutes);

  // Times the book was opened.
  uint32_t getSessions() const { return sessions; }
  uint32_t getTotalMinutes() const;
  int getDaysRead() const { return static_cast<int>(days.size()); }
  // Mean minutes per opening, rounded to nearest. 0 when the book has never been opened.
  // Opens that banked no whole minute still count in the divisor -- they were real sittings,
  // and excluding them would report an average longer than any session actually was.
  uint32_t getAverageSessionMinutes() const;

  void getMonthStatus(uint16_t year, uint8_t month, bool out[32]) const;
  int getDaysReadInMonth(uint16_t year, uint8_t month) const;

  const std::vector<BookDay>& getDays() const { return days; }

  // Path of the file backing a given book, for callers that want to check existence cheaply.
  static std::string filePathFor(const char* bookPath);

 private:
  std::string bookPath;
  uint32_t sessions = 0;
  std::vector<BookDay> days;
};
