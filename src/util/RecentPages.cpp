#include "RecentPages.h"

#include <cstdlib>

namespace {
constexpr uint8_t FILE_VERSION = 1;

void put16(uint8_t* p, const uint16_t v) {
  p[0] = v & 0xFF;
  p[1] = (v >> 8) & 0xFF;
}

void put32(uint8_t* p, const uint32_t v) {
  for (int i = 0; i < 4; i++) p[i] = (v >> (8 * i)) & 0xFF;
}

uint16_t get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

uint32_t get32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

bool isLastPages(const RecentPages::Place& place) { return place.page + 2 >= place.totalPages; }
}  // namespace

bool RecentPages::isNear(const Place& a, const Place& b) {
  if (a.spineIndex == b.spineIndex) return std::abs(static_cast<int>(a.page) - static_cast<int>(b.page)) <= 2;
  // A page turn across a chapter boundary: the end of one spine item and the start of the next.
  if (b.spineIndex == a.spineIndex + 1) return b.page == 0 && isLastPages(a);
  if (a.spineIndex == b.spineIndex + 1) return a.page == 0 && isLastPages(b);
  return false;
}

void RecentPages::removeAt(const int index) {
  for (int i = index; i + 1 < count_; i++) places_[i] = places_[i + 1];
  count_--;
}

void RecentPages::note(const Place& place, const unsigned long nowMs) {
  if (count_ > 0 && isNear(places_[0], place)) {
    const bool samePage = places_[0].spineIndex == place.spineIndex && places_[0].page == place.page;
    Place updated = place;
    // A save made while the section was released has no offset; keep the one this page had.
    if (samePage && !updated.hasOffset) {
      updated.hasOffset = places_[0].hasOffset;
      updated.offset = places_[0].offset;
    }
    if (!samePage) newestTurned_ = true;
    places_[0] = updated;
    return;
  }
  if (count_ > 0 && !newestTurned_ && nowMs - newestSinceMs_ < PASS_THROUGH_MS) removeAt(0);
  // Coming back to an older place moves it to the front instead of listing it twice.
  for (int i = 0; i < count_;) {
    if (isNear(places_[i], place)) {
      removeAt(i);
    } else {
      i++;
    }
  }
  if (count_ == MAX_PLACES) count_--;
  for (int i = count_; i > 0; i--) places_[i] = places_[i - 1];
  places_[0] = place;
  count_++;
  newestSinceMs_ = nowMs;
  newestTurned_ = false;
}

size_t RecentPages::serialize(uint8_t* out, const size_t size) const {
  const size_t needed = 2 + count_ * PLACE_BYTES;
  if (count_ == 0 || size < needed) return 0;
  out[0] = FILE_VERSION;
  out[1] = count_;
  for (int i = 0; i < count_; i++) {
    const Place& p = places_[i];
    uint8_t* r = out + 2 + i * PLACE_BYTES;
    put16(r, p.spineIndex);
    put16(r + 2, p.page);
    put16(r + 4, p.totalPages);
    put16(r + 6, p.bookPage);
    r[8] = p.percent;
    r[9] = p.hasOffset ? 1 : 0;
    put32(r + 10, p.offset);
    put16(r + 14, p.year);
    r[16] = p.month;
    r[17] = p.day;
    r[18] = p.hour;
    r[19] = p.minute;
  }
  return needed;
}

bool RecentPages::deserialize(const uint8_t* data, const size_t size, const int spineCount) {
  count_ = 0;
  if (size < 2 || data[0] != FILE_VERSION) return false;
  const int count = data[1];
  if (count < 1 || count > MAX_PLACES || size != 2 + count * PLACE_BYTES) return false;
  Place loaded[MAX_PLACES];
  for (int i = 0; i < count; i++) {
    const uint8_t* r = data + 2 + i * PLACE_BYTES;
    Place& p = loaded[i];
    p.spineIndex = get16(r);
    p.page = get16(r + 2);
    p.totalPages = get16(r + 4);
    p.bookPage = get16(r + 6);
    p.percent = r[8] > 100 ? 100 : r[8];
    p.hasOffset = r[9] != 0;
    p.offset = get32(r + 10);
    p.year = get16(r + 14);
    p.month = r[16];
    p.day = r[17];
    p.hour = r[18];
    p.minute = r[19];
    if (p.spineIndex >= spineCount) return false;
    // A date that cannot be shown is dropped, not the place.
    if (p.month < 1 || p.month > 12 || p.day < 1 || p.day > 31 || p.hour > 23 || p.minute > 59) p.year = 0;
  }
  for (int i = 0; i < count; i++) places_[i] = loaded[i];
  count_ = static_cast<uint8_t>(count);
  // Places from an earlier session were read, not passed through.
  newestTurned_ = true;
  return true;
}

int RecentPages::dayNumber(int year, const int month, const int day) {
  // Howard Hinnant's days_from_civil.
  year -= month <= 2 ? 1 : 0;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const int yoe = year - era * 400;
  const int doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}
