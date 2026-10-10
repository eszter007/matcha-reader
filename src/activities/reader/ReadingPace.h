#pragma once

#include <cstdint>
#include <string>

// Reading speed for the status bar's time left: the forward pages read in a book, with the seconds
// and the book progress (basis points) they took. Kept per book in its cache, so an estimate is
// there from the first page of the next session. Totals are halved once they grow large, so recent
// reading weighs more than last month's.
class ReadingPace {
 public:
  void addPage(uint32_t seconds, int progressDeltaBp);
  // Estimated seconds for `pages` more pages / `bp` more book progress; -1 until enough is known.
  int secondsForPages(int pages) const;
  int secondsForProgress(int bp) const;

  void load(const std::string& path);
  void save(const std::string& path) const;

 private:
  static constexpr uint32_t MIN_PAGES = 5;
  static constexpr uint32_t DECAY_AT_PAGES = 400;

  uint32_t pages_ = 0;
  uint32_t seconds_ = 0;
  uint32_t progressBp_ = 0;
};
