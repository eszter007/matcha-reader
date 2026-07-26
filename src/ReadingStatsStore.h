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

class ReadingStatsStore {
  static ReadingStatsStore instance;

  static constexpr int MAX_DAYS = 365;
  DailyReading days[MAX_DAYS] = {};
  int dayCount = 0;
  uint16_t booksFinished = 0;
  std::vector<std::string> finishedBookPaths;

 public:
  static ReadingStatsStore& getInstance() { return instance; }

  void addMinutes(uint16_t year, uint8_t month, uint8_t day, uint16_t minutes);
  void markBookFinished(const std::string& bookPath);

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

#define READING_STATS ReadingStatsStore::getInstance()
