#include "BookStatsDownload.h"

#include <Arduino.h>
#include <Logging.h>
#include <SdSystemDir.h>

#include <cstring>

namespace bookdownload {
namespace {
constexpr uint8_t MAGIC[4] = {'B', 'K', 'D', 'L'};
constexpr uint8_t VERSION = 1;
constexpr size_t DAY_BYTES = 6;

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

// One book's header: nameHash(4) finished(1) sessions(4) language length(1) and bytes, day count(2).
bool readBook(HalFile& f, Book& book) {
  uint8_t head[10];
  if (f.read(head, sizeof(head)) != sizeof(head)) return false;
  book.nameHash = getU32(head);
  book.finished = head[4] != 0;
  book.sessions = getU32(head + 5);
  const uint8_t langLen = head[9];
  if (langLen > MAX_LANGUAGE) return false;
  memset(book.language, 0, sizeof(book.language));
  if (langLen && f.read(reinterpret_cast<uint8_t*>(book.language), langLen) != langLen) return false;
  uint8_t count[2];
  if (f.read(count, 2) != 2) return false;
  book.dayCount = getU16(count);
  return book.dayCount <= BookStats::MAX_DAYS;
}

bool readDays(HalFile& f, const uint16_t count, std::vector<BookDay>& days) {
  days.clear();
  // reserve() aborts on failure under -fno-exceptions.
  if (ESP.getMaxAllocHeap() < count * sizeof(BookDay) + 4096) {
    LOG_ERR("BKDL", "Not enough heap for %u days", static_cast<unsigned>(count));
    return false;
  }
  days.reserve(count);
  for (uint16_t i = 0; i < count; i++) {
    uint8_t rec[DAY_BYTES];
    if (f.read(rec, DAY_BYTES) != DAY_BYTES) return false;
    days.push_back({getU16(rec), rec[2], rec[3], getU16(rec + 4)});
  }
  return true;
}

bool openChecked(const std::string& file, HalFile& f) {
  uint8_t head[5];
  return Storage.exists(file.c_str()) && Storage.openFileForRead("BKDL", file.c_str(), f) &&
         f.read(head, sizeof(head)) == sizeof(head) && memcmp(head, MAGIC, 4) == 0 && head[4] == VERSION;
}
}  // namespace

std::string newPath() { return sdsystem::path("bookstats_download.new"); }
std::string path() { return sdsystem::path("bookstats_download.bin"); }

bool Writer::begin() {
  const std::string tmp = newPath() + ".tmp";
  ok = Storage.openFileForWrite("BKDL", tmp.c_str(), file);
  if (!ok) return false;
  uint8_t head[5];
  memcpy(head, MAGIC, 4);
  head[4] = VERSION;
  ok = file.write(head, sizeof(head)) == sizeof(head);
  return ok;
}

bool Writer::beginBook(const uint32_t hash, const bool finished, const uint32_t sessions, const char* language) {
  if (!ok) return false;
  const size_t langLen = language ? strnlen(language, MAX_LANGUAGE) : 0;
  uint8_t head[10];
  putU32(head, hash);
  head[4] = finished ? 1 : 0;
  putU32(head + 5, sessions);
  head[9] = static_cast<uint8_t>(langLen);
  ok = file.write(head, sizeof(head)) == sizeof(head) &&
       (langLen == 0 || file.write(reinterpret_cast<const uint8_t*>(language), langLen) == langLen);
  // The day count is patched in by endBook(), once the days are counted.
  countAt = file.position();
  count = 0;
  const uint8_t zero[2] = {0, 0};
  ok = ok && file.write(zero, 2) == 2;
  return ok;
}

bool Writer::addDay(const BookDay& day) {
  if (!ok || count >= BookStats::MAX_DAYS) return ok;  // past the limit BookStats keeps: dropped
  uint8_t rec[DAY_BYTES];
  putU16(rec, day.year);
  rec[2] = day.month;
  rec[3] = day.day;
  putU16(rec + 4, day.minutes);
  ok = file.write(rec, DAY_BYTES) == DAY_BYTES;
  count++;
  return ok;
}

bool Writer::endBook() {
  if (!ok) return false;
  const size_t end = file.position();
  uint8_t n[2];
  putU16(n, count);
  ok = file.seek(countAt) && file.write(n, 2) == 2 && file.seek(end);
  return ok;
}

bool Writer::commit() {
  const std::string tmp = newPath() + ".tmp";
  ok = file.close() && ok;
  if (!ok) {
    Storage.remove(tmp.c_str());
    return false;
  }
  return Storage.replaceFile(tmp.c_str(), newPath().c_str());
}

void Writer::abort() {
  file.close();
  Storage.remove((newPath() + ".tmp").c_str());
  ok = false;
}

bool forEach(const std::string& file, std::vector<BookDay>& days, void* ctx,
             void (*fn)(void* ctx, const Book& book, const std::vector<BookDay>& days)) {
  HalFile f;
  if (!openChecked(file, f)) return false;
  Book book;
  while (f.position() < f.size()) {
    if (!readBook(f, book) || !readDays(f, book.dayCount, days)) return false;
    fn(ctx, book, days);
  }
  return true;
}

bool find(const uint32_t hash, Book& book, std::vector<BookDay>& days) {
  HalFile f;
  if (!openChecked(path(), f)) return false;
  while (f.position() < f.size()) {
    if (!readBook(f, book)) return false;
    if (book.nameHash == hash) return readDays(f, book.dayCount, days);
    if (!f.seekCur(static_cast<int64_t>(book.dayCount) * DAY_BYTES)) return false;
  }
  return false;
}

}  // namespace bookdownload
