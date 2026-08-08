#include "BookStats.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>
#include <cstring>
#include <functional>

namespace {
constexpr const char* STATS_DIR = "/system/bookstats";
// "BKST" -- a file whose first four bytes are anything else is not ours; refuse it rather than
// reading a stale format's fields as ours.
constexpr uint8_t MAGIC[4] = {'B', 'K', 'S', 'T'};
// v2 drops the lastFlushMinutes field: sessions are counted by recordOpen (one per opening of
// the book) rather than inferred from gaps between flushes, so nothing needs the timestamp any
// more. A v1 file is treated as no history rather than as an error -- see load().
constexpr uint8_t BOOKSTATS_VERSION = 2;
// magic(4) + version(1) + sessions(4) + dayCount(4) + pathLen(2)
constexpr size_t HEADER_BYTES = 15;
constexpr size_t DAY_RECORD_BYTES = 6;
// Same bound the reading-stats loader puts on a path, for the same reason: a corrupt length
// field must not talk us into a large allocation.
constexpr uint16_t MAX_PATH_LEN = 500;
// A book read every day for eight years still fits well inside this. It exists only so a
// corrupt dayCount cannot drive an unbounded reserve().
constexpr uint32_t MAX_DAYS_SANE = 20000;

// Fields are packed and unpacked through a byte buffer rather than by casting the buffer to a
// struct: ESP32-C3 faults on unaligned multi-byte loads, and a record read at an arbitrary file
// offset has no alignment guarantee.
void putU16(uint8_t* p, const uint16_t v) { memcpy(p, &v, 2); }
void putU32(uint8_t* p, const uint32_t v) { memcpy(p, &v, 4); }
uint16_t getU16(const uint8_t* p) {
  uint16_t v;
  memcpy(&v, p, 2);
  return v;
}
uint32_t getU32(const uint8_t* p) {
  uint32_t v;
  memcpy(&v, p, 4);
  return v;
}

int daysInMonthOf(const uint16_t y, const uint8_t m) {
  static constexpr int dm[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m < 1 || m > 12) return 0;
  if (m == 2 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0))) return 29;
  return dm[m];
}
}  // namespace

std::string BookStats::filePathFor(const char* path) {
  // Same 32-bit path hash the library index and cache directories already use, so a book's
  // identity is consistent across the firmware.
  const auto h = static_cast<uint32_t>(std::hash<std::string>{}(std::string(path ? path : "")));
  char buf[64];
  snprintf(buf, sizeof(buf), "%s/%08lx.bin", STATS_DIR, static_cast<unsigned long>(h));
  return std::string(buf);
}

bool BookStats::load(const char* path) {
  bookPath = path ? path : "";
  sessions = 0;
  days.clear();
  if (bookPath.empty()) return false;

  const std::string file = filePathFor(path);
  if (!Storage.exists(file.c_str())) return true;  // no history yet -- the normal first-open case

  HalFile f;
  if (!Storage.openFileForRead("BSTAT", file.c_str(), f)) return false;

  uint8_t head[HEADER_BYTES];
  if (f.read(head, sizeof(head)) != sizeof(head)) return false;
  if (memcmp(head, MAGIC, 4) != 0 || head[4] != BOOKSTATS_VERSION) {
    // Not ours, or an older layout. Start clean instead of returning an error: this is a stats
    // cache, and a stuck unreadable file would leave the screen permanently blank with no way
    // to recover. The next save takes the file over.
    LOG_DBG("BSTAT", "ignoring unreadable/old %s", file.c_str());
    return true;
  }
  const uint32_t storedSessions = getU32(head + 5);
  const uint32_t dayCount = getU32(head + 9);
  const uint16_t pathLen = getU16(head + 13);
  if (pathLen > MAX_PATH_LEN || dayCount > MAX_DAYS_SANE) {
    LOG_ERR("BSTAT", "implausible header in %s", file.c_str());
    return false;
  }

  std::string storedPath(pathLen, '\0');
  if (pathLen && f.read(reinterpret_cast<uint8_t*>(&storedPath[0]), pathLen) != pathLen) return false;
  if (storedPath != bookPath) {
    // Two books whose paths hash alike. Start this one clean rather than crediting it with the
    // other's history -- wrong numbers are worse than absent ones, and the next save takes the
    // file over.
    LOG_ERR("BSTAT", "hash collision on %s", file.c_str());
    return true;
  }

  sessions = storedSessions;
  days.reserve(dayCount);
  for (uint32_t i = 0; i < dayCount; i++) {
    uint8_t rec[DAY_RECORD_BYTES];
    // A short read means the file was truncated mid-write. Keep what parsed and stop; the days
    // already read are real, and the next save rewrites the file whole.
    if (f.read(rec, DAY_RECORD_BYTES) != DAY_RECORD_BYTES) break;
    BookDay d{};
    d.year = getU16(rec);
    d.month = rec[2];
    d.day = rec[3];
    d.minutes = getU16(rec + 4);
    // Same unset-clock garbage ReadingStatsStore drops: days recorded before the RTC was set
    // are misdated and would pollute the calendar.
    if (d.year < 2020) continue;
    days.push_back(d);
  }
  return true;
}

bool BookStats::save() const {
  if (bookPath.empty()) return false;
  Storage.mkdir(STATS_DIR, true);

  const std::string file = filePathFor(bookPath.c_str());
  HalFile f;
  if (!Storage.openFileForWrite("BSTAT", file.c_str(), f)) {
    LOG_ERR("BSTAT", "open for write failed: %s", file.c_str());
    return false;
  }

  const auto pathLen = static_cast<uint16_t>(bookPath.size() > MAX_PATH_LEN ? MAX_PATH_LEN : bookPath.size());
  uint8_t head[HEADER_BYTES];
  memcpy(head, MAGIC, 4);
  head[4] = BOOKSTATS_VERSION;
  putU32(head + 5, sessions);
  putU32(head + 9, static_cast<uint32_t>(days.size()));
  putU16(head + 13, pathLen);
  if (f.write(head, sizeof(head)) != sizeof(head)) return false;
  if (f.write(bookPath.data(), pathLen) != pathLen) return false;

  for (const auto& d : days) {
    uint8_t rec[DAY_RECORD_BYTES];
    putU16(rec, d.year);
    rec[2] = d.month;
    rec[3] = d.day;
    putU16(rec + 4, d.minutes);
    if (f.write(rec, DAY_RECORD_BYTES) != DAY_RECORD_BYTES) {
      LOG_ERR("BSTAT", "short write on %s", file.c_str());
      return false;
    }
  }
  f.close();
  return true;
}

void BookStats::recordMinutes(const uint16_t year, const uint8_t month, const uint8_t day, const uint16_t minutes) {
  if (year < 2020) return;  // clock not set -- recording it would misdate the calendar

  for (auto& d : days) {
    if (d.year != year || d.month != month || d.day != day) continue;
    // Saturating: a day's minutes are displayed, and wrapping to a small number would read as a
    // real value. 65535 minutes is 45 days, so this can only be reached by a corrupt clock.
    const uint32_t sum = static_cast<uint32_t>(d.minutes) + minutes;
    d.minutes = static_cast<uint16_t>(sum > UINT16_MAX ? UINT16_MAX : sum);
    return;
  }
  days.push_back({year, month, day, minutes});
}

bool BookStats::recordOpen(const char* path) {
  if (!path || !*path) return false;
  BookStats b;
  if (!b.load(path)) return false;
  b.sessions++;
  return b.save();
}

uint32_t BookStats::getTotalMinutes() const {
  uint32_t total = 0;
  for (const auto& d : days) total += d.minutes;
  return total;
}

uint32_t BookStats::getAverageSessionMinutes() const {
  if (sessions == 0) return 0;
  return (getTotalMinutes() + sessions / 2) / sessions;  // rounded, not truncated
}

void BookStats::getMonthStatus(const uint16_t year, const uint8_t month, bool out[32]) const {
  for (int i = 0; i < 32; i++) out[i] = false;
  for (const auto& d : days) {
    if (d.year == year && d.month == month && d.day >= 1 && d.day <= 31 && d.minutes > 0) out[d.day] = true;
  }
}

int BookStats::getDaysReadInMonth(const uint16_t year, const uint8_t month) const {
  bool status[32];
  getMonthStatus(year, month, status);
  int count = 0;
  const int dim = daysInMonthOf(year, month);
  for (int d = 1; d <= dim; d++) {
    if (status[d]) count++;
  }
  return count;
}
