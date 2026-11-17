#pragma once

#include <cstddef>
#include <cstdint>

// The places most recently read in one book, newest first. Turning pages moves the newest place
// along; any other move (a jump, a link, a sync, reopening somewhere else) starts a new place.
// The newest place is the page on screen; the ones after it are "Last 5 pages read".
class RecentPages {
 public:
  // The page on screen plus the five before it.
  static constexpr int MAX_PLACES = 6;
  // A place left this soon without a page turn was only passed through (scrubbing Go to %,
  // a footnote). The next move replaces it, so it cannot push a real reading place out.
  static constexpr unsigned long PASS_THROUGH_MS = 10000;

  struct Place {
    uint16_t spineIndex = 0;
    uint16_t page = 0;        // 0-based page within the spine item
    uint16_t totalPages = 0;  // the spine item's page count when recorded
    uint16_t bookPage = 0;    // 1-based page in the whole book; 0 = unknown
    uint8_t percent = 0;
    bool hasOffset = false;
    uint32_t offset = 0;  // visible-text offset in the spine item; survives re-pagination
    // Local date and time last read here; year 0 = the clock was not set.
    uint16_t year = 0;
    uint8_t month = 0;
    uint8_t day = 0;
    uint8_t hour = 0;
    uint8_t minute = 0;
  };

  // Records the page on screen. nowMs is millis().
  void note(const Place& place, unsigned long nowMs);
  int count() const { return count_; }
  const Place& at(const int index) const { return places_[index]; }

  // [version u8] [count u8] then count x PLACE_BYTES, little-endian.
  static constexpr size_t PLACE_BYTES = 20;
  static constexpr size_t MAX_SERIALIZED_SIZE = 2 + MAX_PLACES * PLACE_BYTES;
  // Returns the bytes written, 0 when empty or out does not fit.
  size_t serialize(uint8_t* out, size_t size) const;
  // Rejects the whole file when any part is wrong; the history then starts empty.
  bool deserialize(const uint8_t* data, size_t size, int spineCount);

  // Days since 1970-01-01 for a proleptic Gregorian date, for "today"/"yesterday" checks.
  static int dayNumber(int year, int month, int day);

 private:
  static bool isNear(const Place& a, const Place& b);
  void removeAt(int index);

  Place places_[MAX_PLACES];
  uint8_t count_ = 0;
  unsigned long newestSinceMs_ = 0;
  // Whether the newest place has seen a page turn since it was entered.
  bool newestTurned_ = true;
};
