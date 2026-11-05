#include "BookActivity.h"

#include <Arduino.h>
#include <Logging.h>
#include <SdSystemDir.h>

#include <cstring>

namespace bookactivity {
namespace {
constexpr uint8_t MAGIC[4] = {'B', 'K', 'A', 'C'};
constexpr uint8_t VERSION = 1;
constexpr size_t HEADER_BYTES = 5;  // magic(4) version(1)
// nameHash(4) started(4) finished(4) day count(2); a date is year(2) month(1) day(1).
constexpr size_t BOOK_BYTES = 14;
constexpr size_t DAY_BYTES = 6;  // date(4) minutes(2)

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
void putDate(uint8_t* p, const BookDay& d) {
  putU16(p, d.year);
  p[2] = d.month;
  p[3] = d.day;
}
BookDay getDate(const uint8_t* p) { return {getU16(p), p[2], p[3], 0}; }

std::string partPath() { return path() + ".part"; }
}  // namespace

std::string path() { return sdsystem::path("book_activity.bin"); }

bool Writer::begin() {
  ok = Storage.openFileForWrite("BKAC", partPath().c_str(), file);
  if (!ok) return false;
  uint8_t head[HEADER_BYTES];
  memcpy(head, MAGIC, 4);
  head[4] = VERSION;
  ok = file.write(head, sizeof(head)) == sizeof(head);
  return ok;
}

bool Writer::addBook(const Book& book, const BookDay* days, size_t count) {
  if (!ok) return false;
  if (count > BookStats::MAX_DAYS) count = BookStats::MAX_DAYS;  // the oldest are kept: the start matters most
  uint8_t head[BOOK_BYTES];
  putU32(head, book.nameHash);
  putDate(head + 4, book.started);
  putDate(head + 8, book.finished);
  putU16(head + 12, static_cast<uint16_t>(count));
  ok = file.write(head, sizeof(head)) == sizeof(head);
  for (size_t i = 0; ok && i < count; i++) {
    uint8_t rec[DAY_BYTES];
    putDate(rec, days[i]);
    putU16(rec + 4, days[i].minutes);
    ok = file.write(rec, DAY_BYTES) == DAY_BYTES;
  }
  return ok;
}

bool Writer::commit() {
  ok = file.close() && ok;
  if (!ok) {
    Storage.remove(partPath().c_str());
    return false;
  }
  return Storage.replaceFile(partPath().c_str(), path().c_str());
}

void Writer::abort() {
  file.close();
  Storage.remove(partPath().c_str());
  ok = false;
}

bool find(const std::string& bookPath, Book& book, std::vector<BookDay>& days) {
  const uint32_t hash = bookNameHash(bookPath.data(), bookPath.size());
  HalFile f;
  uint8_t head[HEADER_BYTES];
  if (!Storage.exists(path().c_str()) || !Storage.openFileForRead("BKAC", path().c_str(), f) ||
      f.read(head, sizeof(head)) != sizeof(head) || memcmp(head, MAGIC, 4) != 0 || head[4] != VERSION) {
    return false;
  }
  while (f.position() < f.size()) {
    uint8_t rec[BOOK_BYTES];
    if (f.read(rec, sizeof(rec)) != sizeof(rec)) return false;
    const uint16_t count = getU16(rec + 12);
    if (count > BookStats::MAX_DAYS) return false;  // damaged
    if (getU32(rec) != hash) {
      if (!f.seekCur(static_cast<int64_t>(count) * DAY_BYTES)) return false;
      continue;
    }
    book.nameHash = hash;
    book.started = getDate(rec + 4);
    book.finished = getDate(rec + 8);
    // reserve() aborts on failure under -fno-exceptions.
    if (ESP.getMaxAllocHeap() < count * sizeof(BookDay) + 4096) {
      LOG_ERR("BKAC", "Not enough heap for %u days", static_cast<unsigned>(count));
      return false;
    }
    days.clear();
    days.reserve(count);
    for (uint16_t i = 0; i < count; i++) {
      uint8_t d[DAY_BYTES];
      if (f.read(d, DAY_BYTES) != DAY_BYTES) return false;
      BookDay day = getDate(d);
      day.minutes = getU16(d + 4);
      days.push_back(day);
    }
    return true;
  }
  return false;
}

}  // namespace bookactivity
