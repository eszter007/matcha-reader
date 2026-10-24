#include "StatsSync.h"

#include <BookStats.h>
#include <HalMemory.h>
#include <HalStorage.h>
#include <KOReaderCredentialStore.h>
#include <KOReaderDocumentId.h>
#include <Logging.h>
#include <Memory.h>
#include <ReadingStatsStore.h>
#include <SecureHttpClient.h>
#include <TrustedTime.h>

#include <algorithm>
#include <ctime>

namespace {
// Same floor as KOReaderSyncClient and clippingSync.
constexpr size_t MIN_FREE_FOR_TLS = 35000;
constexpr size_t MIN_BLOCK_FOR_TLS = 20000;
constexpr size_t MAX_RESPONSE = 512;
// Enough batches for the full 730-day window.
constexpr int MAX_BATCHES = (statsSync::HISTORY_DAYS + statsSync::DAYS_PER_UPLOAD - 1) / statsSync::DAYS_PER_UPLOAD;

// PUT a JSON body on the shared connection; true when the server answered 200 and `ackKey` equals `expected`.
bool put(freeink::SecureHttpClient& http, const char* path, const std::string& body, const char* ackKey,
         const int expected) {
  if (!http.begin(KOREADER_STORE.getBaseUrl() + path)) {
    LOG_ERR("STATS", "Invalid sync URL");
    return false;
  }
  for (const auto& header : KOREADER_STORE.getCustomHeaders()) {
    if (!header.name.empty()) http.addHeader(header.name, header.value);
  }
  http.addHeader("x-auth-user", KOREADER_STORE.getUsername());
  http.addHeader("x-auth-key", KOREADER_STORE.getMd5Password());
  http.addHeader("Content-Type", "application/json");
  std::string response;
  response.reserve(MAX_RESPONSE);
  const int status = http.sendRequest("PUT", reinterpret_cast<const uint8_t*>(body.data()), body.size(),
                                      [&](const uint8_t* data, const size_t length) {
                                        if (response.size() + length > MAX_RESPONSE) return false;
                                        response.append(reinterpret_cast<const char*>(data), length);
                                        return true;
                                      });
  if (status != 200 || !http.responseComplete()) {
    LOG_ERR("STATS", "%s: HTTP %d", path, status);
    return false;
  }
  JsonDocument ack;
  if (deserializeJson(ack, response) || ack[ackKey].as<int>() != expected) {
    LOG_ERR("STATS", "%s: not acknowledged", path);
    return false;
  }
  return true;
}

struct PendingBook {
  std::string path;
  int32_t lastReadDay;  // ReadingStatsStore's day number
  bool finished;
};

// The hash progress sync uses for this book.
std::string documentHashFor(const std::string& path) {
  return KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME
             ? KOReaderDocumentId::calculateFromFilename(path)
             : KOReaderDocumentId::calculate(path);
}

// A deleted book or a manga folder has no document hash and is skipped. Any other failure is a read
// error that the next sync retries.
bool unidentifiable(const std::string& path) {
  if (!Storage.exists(path.c_str())) return true;
  HalFile file = Storage.open(path.c_str());
  return file && file.isDirectory();
}

// One /stats/books body, built with no TLS session open: a book's history can take up to 12 KB.
// `next` moves past the books handled; false on a read error, which stops the upload.
bool buildBooks(const std::vector<PendingBook>& books, size_t& next, const std::string& openPath,
                const std::string& openHash, std::string& body, int& count) {
  JsonDocument doc;
  count = 0;
  for (; next < books.size() && count < static_cast<int>(statsSync::BOOKS_PER_UPLOAD); next++) {
    const PendingBook& book = books[next];
    const std::string hash = book.path == openPath ? openHash : documentHashFor(book.path);
    if (hash.empty()) {
      if (unidentifiable(book.path)) continue;
      LOG_ERR("STATS", "No document hash for %s", book.path.c_str());
      return false;
    }
    // load() reserves the whole history and aborts on failure under -fno-exceptions.
    if (HalMemory::getDefaultHeap().largestBlockBytes < BookStats::MAX_DAYS * sizeof(BookDay) + 1024) {
      LOG_ERR("STATS", "Not enough heap for book stats");
      return false;
    }
    BookStats stats;
    if (!stats.load(book.path.c_str())) return false;
    if (stats.getDays().empty()) continue;
    statsSync::addBook(doc, hash, stats.getDays(), stats.getSessions(), book.finished);
    count++;
  }
  body.clear();
  if (count > 0) serializeJson(doc, body);
  return true;
}
}  // namespace

void statsSync::run(const std::string& bookPath, const std::string& documentHash) {
  if (KOREADER_STORE.getServerType() != KOReaderServerType::CROSSPOINT || !KOREADER_STORE.hasCredentials()) return;
  // Without a trusted clock "today" is unknown, and the history bitmap would be anchored wrong.
  const time_t now = static_cast<time_t>(trustedtime::trustedNow());
  if (now == 0) {
    LOG_ERR("STATS", "Clock not trusted; skipping stats upload");
    return;
  }
  tm local{};
  localtime_r(&now, &local);

  // The stats tables are read before any TLS: they can be tens of KB and are gone by then. The day and
  // book lists are reserved first, so they sit below the tables instead of splitting the heap they free.
  Totals totals;
  totals.today = daysSince2000(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
  std::vector<Day> days;
  // reserve() aborts on failure under -fno-exceptions, so check the block first.
  if (HalMemory::getDefaultHeap().largestBlockBytes < HISTORY_DAYS * sizeof(Day) + 1024) {
    LOG_ERR("STATS", "Not enough heap for the day list");
    return;
  }
  days.reserve(HISTORY_DAYS);
  std::vector<PendingBook> books;
  if (HalMemory::getDefaultHeap().largestBlockBytes < ReadingStatsStore::MAX_BOOKS * sizeof(PendingBook) + 1024) {
    LOG_ERR("STATS", "Not enough heap for the book list");
    return;
  }
  books.reserve(ReadingStatsStore::MAX_BOOKS);
  const int32_t booksFrom = KOREADER_STORE.getStatsBooksDay();
  {
    ReadingStatsStore::Loaded stats;
    if (!stats.loaded()) {
      LOG_ERR("STATS", "Reading stats unavailable");
      return;
    }
    for (const auto& d : READING_STATS_STORE.getDays()) {
      const int32_t day = daysSince2000(d.year, d.month, d.day);
      totals.seconds += d.minutesRead * 60u;
      // A day after today comes from a clock jump; uploading it would park the marker in the future.
      if (d.minutesRead > 0 && day <= totals.today && totals.today - day < HISTORY_DAYS && days.size() < HISTORY_DAYS) {
        days.push_back({day, d.minutesRead * 60u});
      }
    }
    totals.booksFinished = READING_STATS_STORE.getBooksFinished();
    totals.longestStreak = READING_STATS_STORE.getLongestStreak();
    // Books read on or after the marker's day (it may have grown since) and the open book.
    for (const auto& b : READING_STATS_STORE.getBooks()) {
      if (b.lastReadDay >= booksFrom || b.path == bookPath) {
        books.push_back({b.path, b.lastReadDay, READING_STATS_STORE.isFinished(b.path)});
      }
    }
  }
  // Oldest first, so the marker can follow each accepted batch.
  std::sort(books.begin(), books.end(),
            [](const PendingBook& a, const PendingBook& b) { return a.lastReadDay < b.lastReadDay; });

  auto http = makeUniqueNoThrow<freeink::SecureHttpClient>();
  const auto heap = HalMemory::getDefaultHeap();
  if (!http || heap.freeBytes < MIN_FREE_FOR_TLS || heap.largestBlockBytes < MIN_BLOCK_FOR_TLS) {
    LOG_ERR("STATS", "Not enough heap for TLS: %zu free, %zu block", heap.freeBytes, heap.largestBlockBytes);
    return;
  }
  http->setInsecure();
  // Book stats first. A failed batch does not hold back the totals; the next sync starts again from it.
  std::string body;
  bool ok = true;
  bool booksDone = false;
  int32_t booksDay = booksFrom;
  for (size_t next = 0; next < books.size();) {
    int count = 0;
    if (!buildBooks(books, next, bookPath, documentHash, body, count) ||
        (count > 0 && !put(*http, "/api/v1/stats/books", body, "accepted", count))) {
      LOG_ERR("STATS", "Book stats incomplete");
      ok = false;
      break;
    }
    http->end();  // the next body is built without TLS buffers on the heap
    // max: the open book can be older than the marker.
    booksDay = std::max(booksDay, books[next - 1].lastReadDay);
    booksDone = true;
  }

  // Days from the last one uploaded (resent: today's minutes grow) onwards, 20 per request.
  const int32_t uploaded = KOREADER_STORE.getStatsUploadedDay();
  size_t from = std::lower_bound(days.begin(), days.end(), uploaded,
                                 [](const Day& d, const int32_t day) { return d.day < day; }) -
                days.begin();
  const size_t start = from;
  for (int batch = 0; batch < MAX_BATCHES; batch++) {
    const size_t count = std::min(DAYS_PER_UPLOAD, days.size() - from);
    body.clear();
    {
      JsonDocument doc;
      buildGlobal(doc, days, totals, from, count);
      serializeJson(doc, body);
    }
    if (!put(*http, "/api/v1/stats/global", body, "accepted_daily", static_cast<int>(count))) {
      ok = false;
      break;
    }
    from += count;
    if (from >= days.size()) break;
  }
  // Free the TLS session before the progress request that follows.
  http->end();
  http.reset();
  // One SD write: a day or book sent twice is harmless, the server keeps the larger value or the newest snapshot.
  if (from > start) KOREADER_STORE.setStatsUploadedDay(days[from - 1].day);
  if (booksDone) KOREADER_STORE.setStatsBooksDay(booksDay);
  if (from > start || booksDone) KOREADER_STORE.saveToFile();
  if (ok) LOG_INF("STATS", "Reading stats uploaded");
}
