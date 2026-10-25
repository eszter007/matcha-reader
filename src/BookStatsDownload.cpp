#include "BookStatsDownload.h"

#include <Arduino.h>
#include <Logging.h>
#include <SdSystemDir.h>

#include <algorithm>
#include <cstring>

namespace bookdownload {
namespace {
constexpr uint8_t MAGIC[4] = {'B', 'K', 'D', 'L'};
constexpr uint8_t VERSION = 1;
// magic(4) version(1) flags(1)
constexpr size_t HEADER_BYTES = 6;
constexpr uint8_t FLAG_INCREMENT = 1;
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

bool openChecked(const std::string& file, HalFile& f, bool* increment = nullptr) {
  uint8_t head[HEADER_BYTES];
  if (!Storage.exists(file.c_str()) || !Storage.openFileForRead("BKDL", file.c_str(), f) ||
      f.read(head, sizeof(head)) != sizeof(head) || memcmp(head, MAGIC, 4) != 0 || head[4] != VERSION) {
    return false;
  }
  if (increment) *increment = (head[5] & FLAG_INCREMENT) != 0;
  return true;
}

std::string combinedPath() { return path() + ".tmp"; }
}  // namespace

std::string newPath() { return sdsystem::path("bookstats_download.new"); }
std::string path() { return sdsystem::path("bookstats_download.bin"); }

bool Writer::begin(const std::string& path, const bool increment) {
  target = path;
  const std::string tmp = target + ".part";
  ok = Storage.openFileForWrite("BKDL", tmp.c_str(), file);
  if (!ok) return false;
  uint8_t head[HEADER_BYTES];
  memcpy(head, MAGIC, 4);
  head[4] = VERSION;
  head[5] = increment ? FLAG_INCREMENT : 0;
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
  const std::string tmp = target + ".part";
  ok = file.close() && ok;
  if (!ok) {
    Storage.remove(tmp.c_str());
    return false;
  }
  return Storage.replaceFile(tmp.c_str(), target.c_str());
}

void Writer::abort() {
  file.close();
  Storage.remove((target + ".part").c_str());
  ok = false;
}

bool forEach(const std::string& file, std::vector<BookDay>& days, void* ctx, const BookFn fn) {
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

namespace {
struct CombineCtx {
  std::vector<uint32_t> changed;  // the increment's books, whose old entries are left out
  Writer* out = nullptr;
  bool ok = true;
};

void copyBook(void* p, const Book& book, const std::vector<BookDay>& days) {
  auto& c = *static_cast<CombineCtx*>(p);
  c.ok = c.ok && c.out->beginBook(book.nameHash, book.finished, book.sessions, book.language);
  for (const auto& d : days) c.ok = c.ok && c.out->addDay(d);
  c.ok = c.ok && c.out->endBook();
}
}  // namespace

bool combine(std::string& combined) {
  HalFile f;
  bool increment = false;
  if (!openChecked(newPath(), f, &increment)) return false;
  f.close();
  if (!increment) {
    combined = newPath();
    return true;
  }
  CombineCtx ctx;
  std::vector<BookDay> days;
  if (!forEach(newPath(), days, &ctx, [](void* p, const Book& book, const std::vector<BookDay>&) {
        static_cast<CombineCtx*>(p)->changed.push_back(book.nameHash);
      })) {
    return false;
  }
  Writer out;
  combined = combinedPath();
  if (!out.begin(combined, false)) return false;
  ctx.out = &out;
  // A missing or damaged old list keeps what it had up to there: the next whole download repairs it.
  forEach(path(), days, &ctx, [](void* p, const Book& book, const std::vector<BookDay>& bookDays) {
    const auto& changed = static_cast<CombineCtx*>(p)->changed;
    if (std::find(changed.begin(), changed.end(), book.nameHash) == changed.end()) copyBook(p, book, bookDays);
  });
  forEach(newPath(), days, &ctx, copyBook);
  if (!ctx.ok || !out.commit()) {
    out.abort();
    return false;
  }
  return true;
}

bool keep(const std::string& combined) {
  if (combined != newPath() && !Storage.replaceFile(combined.c_str(), path().c_str())) return false;
  if (combined == newPath()) return Storage.replaceFile(newPath().c_str(), path().c_str());
  Storage.remove(newPath().c_str());
  return true;
}

void discard(const std::string& combined) {
  if (combined != newPath()) Storage.remove(combined.c_str());
}

}  // namespace bookdownload
