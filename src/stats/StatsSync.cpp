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

#include "BookListParser.h"
#include "BookStatsDownload.h"
#include "CrossPointSettings.h"
#include "DailyStatsParser.h"
#include "OverviewParser.h"
#include "ProgressChangesParser.h"
#include "util/FinishedIndex.h"

namespace {
// Same floor as KOReaderSyncClient and clippingSync.
constexpr size_t MIN_FREE_FOR_TLS = 35000;
constexpr size_t MIN_BLOCK_FOR_TLS = 20000;
constexpr size_t MAX_RESPONSE = 512;
// Enough batches for the full 730-day window.
constexpr int MAX_BATCHES = (statsSync::HISTORY_DAYS + statsSync::DAYS_PER_UPLOAD - 1) / statsSync::DAYS_PER_UPLOAD;

// Points the client at `path` with the account's headers.
bool begin(freeink::SecureHttpClient& http, const char* path) {
  if (!http.begin(KOREADER_STORE.getBaseUrl() + path)) {
    LOG_ERR("STATS", "Invalid sync URL");
    return false;
  }
  for (const auto& header : KOREADER_STORE.getCustomHeaders()) {
    if (!header.name.empty()) http.addHeader(header.name, header.value);
  }
  http.addHeader("x-auth-user", KOREADER_STORE.getUsername());
  http.addHeader("x-auth-key", KOREADER_STORE.getMd5Password());
  return true;
}

// A crosspoint-sync account and a trusted clock; `local` is today. Without the clock "today" is
// unknown, and the history bitmap would be anchored wrong.
bool ready(tm& local) {
  if (KOREADER_STORE.getServerType() != KOReaderServerType::CROSSPOINT || !KOREADER_STORE.hasCredentials()) {
    return false;
  }
  const time_t now = static_cast<time_t>(trustedtime::trustedNow());
  if (now == 0) {
    LOG_ERR("STATS", "Clock not trusted; skipping stats sync");
    return false;
  }
  localtime_r(&now, &local);
  return true;
}

bool heapForTls() {
  const auto heap = HalMemory::getDefaultHeap();
  if (heap.freeBytes >= MIN_FREE_FOR_TLS && heap.largestBlockBytes >= MIN_BLOCK_FOR_TLS) return true;
  LOG_ERR("STATS", "Not enough heap for TLS: %zu free, %zu block", heap.freeBytes, heap.largestBlockBytes);
  return false;
}

// PUT a JSON body on the shared connection; true when the server answered 200 and `ackKey` equals `expected`.
bool put(freeink::SecureHttpClient& http, const char* path, const std::string& body, const char* ackKey,
         const int expected) {
  if (!begin(http, path)) return false;
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
  std::string language;
  int32_t lastReadDay;  // ReadingStatsStore's day number
  bool finished;
};

// Books per page of the book list; the response is streamed, so this only trades requests for page size.
constexpr int BOOKS_PER_PAGE = 50;
// Pages read at most: 2000 books.
constexpr int MAX_BOOK_PAGES = 40;

// A query value: everything but unreserved characters percent-encoded (the cursor carries a ':').
std::string urlEncode(const std::string& v) {
  static constexpr char DIGITS[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(v.size() * 3);
  for (const char c : v) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
        c == '_' || c == '~') {
      out += c;
    } else {
      out += '%';
      out += DIGITS[static_cast<uint8_t>(c) >> 4];
      out += DIGITS[static_cast<uint8_t>(c) & 15];
    }
  }
  return out;
}

// Minutes UTC is ahead of local time, as JS getTimezoneOffset() gives it: the server's `tz`.
int utcOffsetMinutes() {
  const time_t now = time(nullptr);
  tm local{};
  tm utc{};
  localtime_r(&now, &local);
  gmtime_r(&now, &utc);
  const int32_t days = statsSync::daysSince2000(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday) -
                       statsSync::daysSince2000(utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday);
  return -((days * 24 + local.tm_hour - utc.tm_hour) * 60 + local.tm_min - utc.tm_min);
}

// The account's numbers as the server's Stats page counts them, every device and every source, for
// Insights' "All devices" view. Display only: never merged into this reader's stats. Best effort: on
// failure the view keeps the last answer and its baselines, which still belong together.
void downloadOverview(freeink::SecureHttpClient& http, const std::vector<AccountBaseline>& baselines) {
  // All languages plus the server's 20 groups, about 2.7 KB.
  std::vector<StatsDigest> digests;
  if (HalMemory::getDefaultHeap().largestBlockBytes < 21 * sizeof(StatsDigest) + 1024) {
    LOG_ERR("STATS", "Not enough heap for the account overview");
    return;
  }
  digests.reserve(21);
  auto parser = makeUniqueNoThrow<OverviewParser>(digests);
  // Two years of reading days, as the calendar pages back through: 730 bits of the 736 a digest holds.
  static_assert(sizeof(StatsDigest::history) * 8 >= 730, "history too small");
  char path[96];
  snprintf(path, sizeof(path), "/api/v1/stats/overview?tz=%d&days=730&group_by=language&week_start=1",
           utcOffsetMinutes());
  if (!parser || !begin(http, path)) return;
  const int status = http.sendRequest("GET", nullptr, 0, [&](const uint8_t* data, const size_t length) {
    parser->feed(reinterpret_cast<const char*>(data), length);
    return true;
  });
  if (status != 200 || !http.responseComplete() || !parser->complete()) {
    LOG_ERR("STATS", "Account overview download failed: HTTP %d", status);
    return;
  }
  parser.reset();
  if (!ReadingStatsStore::saveAccount(digests, baselines, statsSync::accountKey())) {
    LOG_ERR("STATS", "Account overview not saved");
    return;
  }
  LOG_INF("STATS", "Downloaded the account overview, %zu languages", digests.size() - 1);
}

// This reader's own rows of the server's book list, page by page into the download file
// (bookdownload). True with nothing written when the server has no book list yet.
bool downloadBooks(freeink::SecureHttpClient& http) {
  // Only what changed since the last download, when the list it has is whole and merged; else all of it.
  const uint32_t since =
      Storage.exists(bookdownload::path().c_str()) && !Storage.exists(bookdownload::newPath().c_str())
          ? KOREADER_STORE.getStatsBooksSince()
          : 0;
  bookdownload::Writer writer;
  if (!writer.begin(bookdownload::newPath(), since > 0)) return false;
  const BookListParser::Sink sink{
      &writer,
      [](void* w, const uint32_t hash, const bool finished, const uint32_t sessions, const char* language) {
        return static_cast<bookdownload::Writer*>(w)->beginBook(hash, finished, sessions, language);
      },
      [](void* w, const BookDay& day) { return static_cast<bookdownload::Writer*>(w)->addDay(day); },
      [](void* w) { return static_cast<bookdownload::Writer*>(w)->endBook(); }};
  std::string cursor;
  int books = 0;
  uint32_t until = 0;
  for (int page = 0; page < MAX_BOOK_PAGES; page++) {
    // Heap, not stack: the parser holds a 2 KB token buffer. A fresh one per page.
    auto parser = makeUniqueNoThrow<BookListParser>(sink);
    char path[128];
    snprintf(path, sizeof(path), "/api/v1/stats/books?device_id=%s&limit=%d&since=%lu%s", statsSync::DEVICE_ID,
             BOOKS_PER_PAGE, static_cast<unsigned long>(since), cursor.empty() ? "" : "&cursor=");
    if (!parser || !begin(http, (std::string(path) + urlEncode(cursor)).c_str())) {
      writer.abort();
      return false;
    }
    const int status = http.sendRequest("GET", nullptr, 0, [&](const uint8_t* data, const size_t length) {
      parser->feed(reinterpret_cast<const char*>(data), length);
      return true;
    });
    if (status == 404 && page == 0) {
      LOG_INF("STATS", "Server has no book list; per-book stats not downloaded");
      writer.abort();
      return true;
    }
    if (status != 200 || !http.responseComplete() || !parser->complete()) {
      LOG_ERR("STATS", "Book list download failed: HTTP %d", status);
      writer.abort();
      return false;
    }
    books += parser->books();
    // The first page's time: anything uploaded while the rest were read comes again next time.
    if (page == 0) until = parser->until();
    cursor = parser->next();
    if (cursor.empty()) break;
  }
  if (!writer.commit()) {
    LOG_ERR("STATS", "Downloaded book stats not saved");
    return false;
  }
  KOREADER_STORE.setStatsBooksSince(until);
  KOREADER_STORE.saveToFile();
  LOG_INF("STATS", "Downloaded stats of %d books%s", books, since > 0 ? " changed since the last refresh" : "");
  return true;
}

// Changes per page of the progress feed; the server also stops a page before 8 KB.
constexpr int CHANGES_PER_PAGE = 20;
// Pages read per sync at most: the first one goes through every book on the account, and what is left
// comes with the next sync.
constexpr int MAX_CHANGE_PAGES = 50;

// Books finished on any device, from the server's progress feed into the finished index (the Library's
// "Read" badges), loaded by the caller before TLS took its buffers. Only what changed since the last pull.
// What is applied is saved with its cursor, so a failure costs only the pages after it, which come again
// next time.
void pullFinished(freeink::SecureHttpClient& http, finishedIndex::Feed& feed) {
  const uint64_t start = feed.cursor();
  const ProgressChangesParser::Sink sink{&feed, [](void* f, const uint32_t* keys, const size_t count, const float pct) {
                                           auto* index = static_cast<finishedIndex::Feed*>(f);
                                           for (size_t i = 0; i < count; i++) {
                                             if (!index->set(keys[i], pct >= finishedIndex::FINISHED_AT)) return false;
                                           }
                                           return true;
                                         }};
  int changes = 0;
  for (int page = 0; page < MAX_CHANGE_PAGES; page++) {
    // Heap, not stack: the parser holds a 2 KB token buffer. A fresh one per page.
    auto parser = makeUniqueNoThrow<ProgressChangesParser>(sink);
    char path[96];
    snprintf(path, sizeof(path), "/api/v1/progress/changes?since=%llu&limit=%d",
             static_cast<unsigned long long>(feed.cursor()), CHANGES_PER_PAGE);
    if (!parser || !begin(http, path)) break;
    const int status = http.sendRequest("GET", nullptr, 0, [&](const uint8_t* data, const size_t length) {
      parser->feed(reinterpret_cast<const char*>(data), length);
      return true;
    });
    if (status == 404) {
      LOG_INF("STATS", "Server has no progress feed; finished books not synced");
      break;
    }
    if (status != 200 || !http.responseComplete() || !parser->complete()) {
      LOG_ERR("STATS", "Progress feed stopped: HTTP %d", status);
      break;
    }
    feed.advance(parser->cursor());
    changes += parser->changes();
    if (!parser->more()) break;
  }
  if (feed.overflowed()) LOG_ERR("STATS", "Finished index full; some finished books left out");
  if (feed.cursor() != start && !feed.save()) {
    LOG_ERR("STATS", "Finished index not saved");
    return;
  }
  LOG_INF("STATS", "Progress feed: %d changes", changes);
}

// Where the upload stands: the book being sent and how many of its new days already went.
struct BookCursor {
  size_t book = 0;
  size_t day = 0;
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

// One /stats/books body and the matching /documents body, built with no TLS session open: a book's
// history can take up to 12 KB. Each book sends its days from `fromDay` (ReadingStatsStore's day number)
// on, 20 per item. `at` moves past what went in; `doneDay` rises to the last read day of each book
// finished here. False on a read error, which stops the upload.
bool buildBatch(const std::vector<PendingBook>& books, BookCursor& at, const int32_t fromDay,
                const std::string& openPath, const std::string& openHash, std::string& statsBody, std::string& docsBody,
                int& items, int& docs, int32_t& doneDay) {
  JsonDocument statsDoc;
  JsonDocument docsDoc;
  JsonArray docItems = docsDoc["items"].to<JsonArray>();
  items = 0;
  docs = 0;
  while (at.book < books.size() && items < static_cast<int>(statsSync::ITEMS_PER_UPLOAD)) {
    const PendingBook& book = books[at.book];
    const std::string hash = book.path == openPath ? openHash : documentHashFor(book.path);
    if (hash.empty()) {
      if (!unidentifiable(book.path)) {
        LOG_ERR("STATS", "No document hash for %s", book.path.c_str());
        return false;
      }
      at = {at.book + 1, 0};
      continue;
    }
    // load() reserves the whole history and aborts on failure under -fno-exceptions.
    if (HalMemory::getDefaultHeap().largestBlockBytes < BookStats::MAX_DAYS * sizeof(BookDay) + 1024) {
      LOG_ERR("STATS", "Not enough heap for book stats");
      return false;
    }
    BookStats stats;
    if (!stats.load(book.path.c_str())) return false;
    const auto& days = stats.getDays();
    if (days.empty()) {
      at = {at.book + 1, 0};
      continue;
    }
    // Days not sent yet: from the marker's day on (all of them the first time).
    const auto firstNew = std::find_if(days.begin(), days.end(), [fromDay](const BookDay& d) {
      return ReadingStatsStore::dayNumber(d.year, d.month, d.day) >= fromDay;
    });
    // Filename and language, so the server can filter and a restore can find the file: once, with the
    // book's first upload (all its days new), not every time it is read.
    if (at.day == 0 && firstNew == days.begin()) {
      JsonObject meta = docItems.add<JsonObject>();
      meta["document"] = hash;
      const size_t slash = book.path.find_last_of('/');
      meta["filename"] = slash == std::string::npos ? book.path : book.path.substr(slash + 1);
      if (!book.language.empty()) meta["language"] = book.language;
      docs++;
    }
    const size_t newDays = static_cast<size_t>(days.end() - firstNew);
    BookDay finishDay{};
    const bool hasFinishDay = stats.getFinishDay(finishDay.year, finishDay.month, finishDay.day);
    // One item even with no new day: the totals still change with each session.
    do {
      const size_t count = std::min(statsSync::DAYS_PER_ITEM, newDays - std::min(at.day, newDays));
      statsSync::addBook(statsDoc, hash, days, stats.getSessions(), book.finished,
                         count > 0 ? &*(firstNew + static_cast<std::ptrdiff_t>(at.day)) : nullptr, count,
                         hasFinishDay ? &finishDay : nullptr);
      items++;
      at.day += count;
    } while (at.day < newDays && items < static_cast<int>(statsSync::ITEMS_PER_UPLOAD));
    if (at.day < newDays) break;  // the rest of this book goes in the next batch
    doneDay = std::max(doneDay, book.lastReadDay);
    at = {at.book + 1, 0};
  }
  statsBody.clear();
  docsBody.clear();
  if (items > 0) serializeJson(statsDoc, statsBody);
  if (docs > 0) serializeJson(docsDoc, docsBody);
  return true;
}
}  // namespace

bool statsSync::run(const std::string& bookPath, const std::string& documentHash) {
  tm local{};
  if (!ready(local)) return false;

  // The stats tables are read before any TLS: they can be tens of KB and are gone by then. The day and
  // book lists are reserved first, so they sit below the tables instead of splitting the heap they free.
  Totals totals;
  totals.today = daysSince2000(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
  std::vector<Day> days;
  // reserve() aborts on failure under -fno-exceptions, so check the block first.
  if (HalMemory::getDefaultHeap().largestBlockBytes < HISTORY_DAYS * sizeof(Day) + 1024) {
    LOG_ERR("STATS", "Not enough heap for the day list");
    return false;
  }
  days.reserve(HISTORY_DAYS);
  std::vector<PendingBook> books;
  if (HalMemory::getDefaultHeap().largestBlockBytes < ReadingStatsStore::MAX_BOOKS * sizeof(PendingBook) + 1024) {
    LOG_ERR("STATS", "Not enough heap for the book list");
    return false;
  }
  books.reserve(ReadingStatsStore::MAX_BOOKS);
  std::vector<AccountBaseline> baselines;
  const int32_t booksFrom = KOREADER_STORE.getStatsBooksDay();
  {
    ReadingStatsStore::Loaded stats;
    if (!stats.loaded()) {
      LOG_ERR("STATS", "Reading stats unavailable");
      return false;
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
    // What is uploaded below: the overview fetched after it counts exactly this much of this reader.
    READING_STATS_STORE.accountBaselines(baselines);
    // Books read on or after the marker's day (it may have grown since) and the open book.
    for (const auto& b : READING_STATS_STORE.getBooks()) {
      if (b.lastReadDay >= booksFrom || b.path == bookPath) {
        books.push_back({b.path, b.language, b.lastReadDay, READING_STATS_STORE.isFinished(b.path)});
      }
    }
  }
  // Oldest first, so the marker can follow each accepted batch.
  std::sort(books.begin(), books.end(),
            [](const PendingBook& a, const PendingBook& b) { return a.lastReadDay < b.lastReadDay; });

  // The finished index too: loaded once TLS holds its buffers, it would need a large block next to them.
  finishedIndex::Feed feed;
  const bool feedReady = feed.begin(accountKey());
  if (!feedReady) LOG_ERR("STATS", "Not enough heap for the finished index");

  auto http = makeUniqueNoThrow<freeink::SecureHttpClient>();
  if (!http || !heapForTls()) return false;
  http->setInsecure();
  // Book stats first. A failed batch does not hold back the totals; the next sync starts again from it.
  std::string body;
  bool ok = true;
  int32_t booksDay = booksFrom;
  std::string docsBody;
  for (BookCursor at; at.book < books.size();) {
    int items = 0;
    int docs = 0;
    int32_t doneDay = booksDay;
    // Metadata first: a book's stats are only worth recording once the server can place it.
    if (!buildBatch(books, at, booksFrom, bookPath, documentHash, body, docsBody, items, docs, doneDay) ||
        (docs > 0 && !put(*http, "/api/v1/documents", docsBody, "accepted", docs)) ||
        (items > 0 && !put(*http, "/api/v1/stats/books", body, "accepted", items))) {
      LOG_ERR("STATS", "Book stats incomplete");
      ok = false;
      break;
    }
    // The next book batch is built without TLS buffers on the heap. The daily totals' small body is not:
    // after the last batch the connection carries straight on to them.
    if (at.book < books.size()) http->end();
    booksDay = doneDay;
  }
  const bool booksDone = booksDay != booksFrom;

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
  // Only after a complete upload: then the server holds everything the baselines count.
  if (ok) downloadOverview(*http, baselines);
  if (feedReady) pullFinished(*http, feed);
  // Free the TLS session before the progress request that follows.
  http->end();
  http.reset();
  // One SD write: a day or book sent twice is harmless, the server keeps the larger value or the newest snapshot.
  if (from > start) KOREADER_STORE.setStatsUploadedDay(days[from - 1].day);
  if (booksDone) KOREADER_STORE.setStatsBooksDay(booksDay);
  if (from > start || booksDone) KOREADER_STORE.saveToFile();
  if (ok) LOG_INF("STATS", "Reading stats uploaded");
  return ok;
}

bool statsSync::download() {
  tm local{};
  if (!ready(local)) return false;
  const int32_t today = daysSince2000(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
  std::vector<DailyReading> days;
  // reserve() aborts on failure under -fno-exceptions, so check the block first.
  if (HalMemory::getDefaultHeap().largestBlockBytes < HISTORY_DAYS * sizeof(DailyReading) + 1024) {
    LOG_ERR("STATS", "Not enough heap for the day list");
    return false;
  }
  days.reserve(HISTORY_DAYS);
  // Heap, not stack: the parser holds a 2 KB token buffer.
  auto parser = makeUniqueNoThrow<DailyStatsParser>(DEVICE_ID, days, today - HISTORY_DAYS + 1, today);
  auto http = makeUniqueNoThrow<freeink::SecureHttpClient>();
  if (!parser || !http || !heapForTls()) return false;
  http->setInsecure();
  // This reader's days only, and once a download is folded in, only those uploaded since the last one. An
  // unmerged download would be replaced by this one, and a missing stats file has lost what the last one
  // brought: either way all of them again.
  const uint32_t since = ReadingStatsStore::hasPendingDownload() || !ReadingStatsStore::hasStatsFile()
                             ? 0
                             : KOREADER_STORE.getStatsDaysSince();
  char path[96];
  snprintf(path, sizeof(path), "/api/v1/stats/global?device_id=%s&since=%lu", DEVICE_ID,
           static_cast<unsigned long>(since));
  if (!begin(*http, path)) return false;
  // Streamed: without the filters (an older server) it holds every device's whole history.
  const int status = http->sendRequest("GET", nullptr, 0, [&](const uint8_t* data, const size_t length) {
    parser->feed(reinterpret_cast<const char*>(data), length);
    return true;
  });
  const bool complete = status == 200 && http->responseComplete() && parser->complete();
  const uint32_t until = parser->until();
  parser.reset();
  if (!complete) {
    LOG_ERR("STATS", "Stats download failed: HTTP %d", status);
    return false;
  }
  if (!ReadingStatsStore::saveDownload(days)) {
    LOG_ERR("STATS", "Downloaded stats not saved");
    return false;
  }
  KOREADER_STORE.setStatsDaysSince(until);
  KOREADER_STORE.saveToFile();
  LOG_INF("STATS", "Downloaded %zu days of reading stats", days.size());
  std::vector<DailyReading>().swap(days);
  // Same connection for the book list: no further handshake.
  const bool books = downloadBooks(*http);
  http->end();
  return books;
}

uint32_t statsSync::accountKey() {
  if (!KOREADER_STORE.hasCredentials()) return 0;
  // FNV-1a over user@server: the overview is the account's, not this card's.
  const std::string account = KOREADER_STORE.getUsername() + "@" + KOREADER_STORE.getBaseUrl();
  uint32_t h = 2166136261u;
  for (const char c : account) h = (h ^ static_cast<uint8_t>(c)) * 16777619u;
  return h | 1;  // never 0, which means no account view
}

uint32_t statsSync::insightsAccountKey() { return SETTINGS.insightsAllDevices != 0 ? accountKey() : 0; }
