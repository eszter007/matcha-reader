#pragma once
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
  // loads clean and empty, which is the normal first-open case.
  bool load(const char* bookPath);
  bool save() const;

  // One session = one opening. Called from each reader's onEnter, which runs once per open:
  // readers survive their own menus (startActivityForResult), so those do not re-count.
  //
  // Accepted inaccuracy: a sleep-wake re-enters the reader (main.cpp, lastSleepFromReader) and
  // counts again, so a read broken by sleep reads as several sessions.
  static bool recordOpen(const char* bookPath);

  // Sessions are counted by recordOpen, not here: an opening counts even if it banks no minute.
  void recordMinutes(uint16_t year, uint8_t month, uint8_t day, uint16_t minutes);

  uint32_t getSessions() const { return sessions; }
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
  std::string bookPath;
  uint32_t sessions = 0;
  std::vector<BookDay> days;
};
