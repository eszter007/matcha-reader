#include "BookStats.h"

#include <HalStorage.h>
#include <Logging.h>
#include <SdSystemDir.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <numeric>
#include <tuple>

namespace {
// Resolved at use: the folder is "/.system" or "/system" depending on what the card has.
static std::string statsDir() { return sdsystem::path("bookstats"); }
constexpr uint8_t MAGIC[4] = {'B', 'K', 'S', 'T'};
// v2 dropped lastFlushMinutes when sessions moved from flush-gap inference to recordOpen.
// v3 appended the lookup and saved-sentence counters to the header; v2 files still load.
constexpr uint8_t BOOKSTATS_VERSION = 3;
// magic(4) + version(1) + sessions(4) + dayCount(4) + pathLen(2)
constexpr size_t V2_HEADER_BYTES = 15;
// + lookups(4) + sentencesSaved(4)
constexpr size_t HEADER_BYTES = V2_HEADER_BYTES + 8;
constexpr size_t DAY_RECORD_BYTES = 6;
// After the day records: year(2) + month(1) + day(1) of the first finish. Older readers stop
// before it and drop it on their next save.
constexpr size_t FINISH_BYTES = 4;

// The render task records a finish while the main loop flushes minutes into the same file.
std::mutex& fileMutex() {
  static std::mutex m;
  return m;
}
constexpr uint16_t MAX_PATH_LEN = 500;  // guards reserve() against a corrupt length field

// Packed through a byte buffer, not a struct cast: ESP32-C3 faults on unaligned multi-byte
// loads and a record at an arbitrary file offset has no alignment guarantee.
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

// Chronological order; the tuple compare is equivalent for valid dates.
bool byDate(const BookDay& a, const BookDay& b) {
  return std::tie(a.year, a.month, a.day) < std::tie(b.year, b.month, b.day);
}

int daysInMonthOf(const uint16_t y, const uint8_t m) {
  static constexpr int dm[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m < 1 || m > 12) return 0;
  if (m == 2 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0))) return 29;
  return dm[m];
}
}  // namespace

std::string BookStats::filePathFor(const char* path) {
  // Same 32-bit path hash the library index and cache directories use.
  const auto h = static_cast<uint32_t>(std::hash<std::string>{}(std::string(path ? path : "")));
  char buf[64];
  snprintf(buf, sizeof(buf), "%s/%08lx.bin", statsDir().c_str(), static_cast<unsigned long>(h));
  return std::string(buf);
}

bool BookStats::load(const char* path) {
  bookPath = path ? path : "";
  sessions = 0;
  lookups = 0;
  sentencesSaved = 0;
  days.clear();
  finished = {};
  if (bookPath.empty()) return false;

  const std::string file = filePathFor(path);
  if (!Storage.exists(file.c_str())) return true;  // no history yet -- the normal first-open case

  HalFile f;
  if (!Storage.openFileForRead("BSTAT", file.c_str(), f)) return false;

  uint8_t head[HEADER_BYTES];
  if (f.read(head, V2_HEADER_BYTES) != V2_HEADER_BYTES) return false;
  if (memcmp(head, MAGIC, 4) != 0 || head[4] < 2 || head[4] > BOOKSTATS_VERSION) {
    // Start clean rather than error: an unreadable stats cache must not leave the screen
    // permanently blank. The next save takes the file over.
    LOG_DBG("BSTAT", "ignoring unreadable/old %s", file.c_str());
    return true;
  }
  const bool hasCounters = head[4] >= 3;
  if (hasCounters && f.read(head + V2_HEADER_BYTES, HEADER_BYTES - V2_HEADER_BYTES) != HEADER_BYTES - V2_HEADER_BYTES) {
    return false;
  }
  const uint32_t storedSessions = getU32(head + 5);
  const uint32_t dayCount = getU32(head + 9);
  const uint16_t pathLen = getU16(head + 13);
  if (pathLen > MAX_PATH_LEN) {
    LOG_ERR("BSTAT", "implausible header in %s", file.c_str());
    return false;
  }

  std::string storedPath(pathLen, '\0');
  if (pathLen && f.read(reinterpret_cast<uint8_t*>(&storedPath[0]), pathLen) != pathLen) return false;
  if (storedPath != bookPath) {
    // Hash collision: start clean rather than credit this book with another's history.
    LOG_ERR("BSTAT", "hash collision on %s", file.c_str());
    return true;
  }

  sessions = storedSessions;
  if (hasCounters) {
    lookups = getU32(head + 15);
    sentencesSaved = getU32(head + 19);
  }
  // Keep the most recent MAX_DAYS. The skipped ones are still consumed: the finish day follows them.
  const size_t keep = std::min<size_t>(dayCount, MAX_DAYS);
  for (size_t i = 0; i + keep < dayCount; i++) {
    uint8_t discard[DAY_RECORD_BYTES];
    if (f.read(discard, DAY_RECORD_BYTES) != DAY_RECORD_BYTES) break;
  }
  days.reserve(keep);
  bool complete = true;
  for (size_t i = 0; i < keep; i++) {
    uint8_t rec[DAY_RECORD_BYTES];
    // Truncated mid-write: keep what parsed; the next save rewrites the file whole.
    if (f.read(rec, DAY_RECORD_BYTES) != DAY_RECORD_BYTES) {
      complete = false;
      break;
    }
    BookDay d{};
    d.year = getU16(rec);
    d.month = rec[2];
    d.day = rec[3];
    d.minutes = getU16(rec + 4);
    if (d.year < 2020) continue;  // recorded before the clock was set; misdated
    days.push_back(d);
  }
  uint8_t fin[FINISH_BYTES];
  if (complete && f.read(fin, FINISH_BYTES) == FINISH_BYTES && getU16(fin) >= 2020) {
    finished = {getU16(fin), fin[2], fin[3], 0};
  }
  // save() trims by keeping the tail, so the order has to hold for a file we did not write.
  // Normally one comparison pass.
  if (!std::is_sorted(days.begin(), days.end(), byDate)) std::sort(days.begin(), days.end(), byDate);
  return true;
}

bool BookStats::save() const {
  if (bookPath.empty()) return false;
  Storage.mkdir(statsDir().c_str(), true);

  const std::string file = filePathFor(bookPath.c_str());
  HalFile f;
  if (!Storage.openFileForWrite("BSTAT", file.c_str(), f)) {
    LOG_ERR("BSTAT", "open for write failed: %s", file.c_str());
    return false;
  }

  const auto pathLen = static_cast<uint16_t>(bookPath.size() > MAX_PATH_LEN ? MAX_PATH_LEN : bookPath.size());
  // days never exceeds MAX_DAYS in memory; sorted (recordMinutes and load both keep it so), so
  // trimming the head would keep the most recent entries.
  const size_t dayCount = std::min<size_t>(days.size(), MAX_DAYS);
  const size_t firstDay = days.size() - dayCount;

  uint8_t head[HEADER_BYTES];
  memcpy(head, MAGIC, 4);
  head[4] = BOOKSTATS_VERSION;
  putU32(head + 5, sessions);
  putU32(head + 9, static_cast<uint32_t>(dayCount));
  putU16(head + 13, pathLen);
  putU32(head + 15, lookups);
  putU32(head + 19, sentencesSaved);
  if (f.write(head, sizeof(head)) != sizeof(head)) return false;
  if (f.write(bookPath.data(), pathLen) != pathLen) return false;

  for (size_t i = firstDay; i < days.size(); i++) {
    const auto& d = days[i];
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
  if (finished.year != 0) {
    uint8_t fin[FINISH_BYTES];
    putU16(fin, finished.year);
    fin[2] = finished.month;
    fin[3] = finished.day;
    if (f.write(fin, FINISH_BYTES) != FINISH_BYTES) return false;
  }
  f.close();
  return true;
}

void BookStats::recordMinutes(const uint16_t year, const uint8_t month, const uint8_t day, const uint16_t minutes) {
  if (year < 2020) return;  // clock not set; would misdate the calendar

  // Sorted insert, so save() can trim to the most recent entries. Almost always appends; only
  // a backwards clock jump lands elsewhere.
  for (auto it = days.rbegin(); it != days.rend(); ++it) {
    if (it->year == year && it->month == month && it->day == day) {
      // Saturating: a wrap would display a small plausible number instead of an obvious fault.
      const uint32_t sum = static_cast<uint32_t>(it->minutes) + minutes;
      it->minutes = static_cast<uint16_t>(sum > UINT16_MAX ? UINT16_MAX : sum);
      return;
    }
    if (byDate(*it, BookDay{year, month, day, 0})) {
      days.insert(it.base(), {year, month, day, minutes});
      return;
    }
  }
  days.insert(days.begin(), {year, month, day, minutes});
}

bool BookStats::recordOpen(const char* path) {
  if (!path || !*path) return false;
  std::lock_guard<std::mutex> lock(fileMutex());
  BookStats b;
  if (!b.load(path)) return false;
  b.sessions++;
  return b.save();
}

bool BookStats::addCounts(const char* path, const uint32_t addLookups, const uint32_t addSentences) {
  if (!path || !*path || (addLookups == 0 && addSentences == 0)) return false;
  std::lock_guard<std::mutex> lock(fileMutex());
  BookStats b;
  if (!b.load(path)) return false;
  b.lookups += addLookups;
  b.sentencesSaved += addSentences;
  return b.save();
}

bool BookStats::addMinutes(const char* path, const uint16_t year, const uint8_t month, const uint8_t day,
                           const uint16_t minutes) {
  if (!path || !*path) return false;
  std::lock_guard<std::mutex> lock(fileMutex());
  BookStats b;
  if (!b.load(path)) return false;
  b.recordMinutes(year, month, day, minutes);
  return b.save();
}

bool BookStats::recordFinish(const char* path, const uint16_t year, const uint8_t month, const uint8_t day,
                             const bool firstFinish, BookStats& out) {
  if (!path || !*path) return false;
  std::lock_guard<std::mutex> lock(fileMutex());
  if (!out.load(path)) return false;
  if (out.finished.year != 0) return true;
  const BookDay today{year, month, day, 0};
  if (firstFinish) {
    if (year < 2020) return true;  // clock not set: nothing worth keeping
    out.finished = today;
    return out.save();
  }
  // Finished before dates were kept, or unsure: the last reading day before this visit's reading.
  const auto last =
      std::find_if(out.days.rbegin(), out.days.rend(), [&today](const BookDay& d) { return byDate(d, today); });
  if (last != out.days.rend()) out.finished = *last;
  return true;
}

bool BookStats::getFinishDay(uint16_t& year, uint8_t& month, uint8_t& day) const {
  if (finished.year == 0) return false;
  year = finished.year;
  month = finished.month;
  day = finished.day;
  return true;
}

int BookStats::getDaySpan() const {
  if (days.empty()) return 0;
  // Days since 0000-03-01 (Howard Hinnant's days_from_civil), enough for a difference.
  const auto serial = [](const BookDay& d) {
    const int y = d.year - (d.month <= 2 ? 1 : 0);
    const int era = y / 400;
    const int yoe = y - era * 400;
    const int doy = (153 * (d.month + (d.month > 2 ? -3 : 9)) + 2) / 5 + d.day - 1;
    return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy;
  };
  return serial(days.back()) - serial(days.front()) + 1;
}

uint32_t BookStats::getTotalMinutes() const {
  return std::accumulate(days.begin(), days.end(), uint32_t{0},
                         [](const uint32_t sum, const BookDay& d) { return sum + d.minutes; });
}

uint32_t BookStats::getAverageSessionMinutes() const {
  if (sessions == 0) return 0;
  return (getTotalMinutes() + sessions / 2) / sessions;  // rounded
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
