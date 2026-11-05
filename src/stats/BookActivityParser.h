#pragma once

#include <StreamingJsonParser.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "BookStats.h"

// Streams GET /api/v1/stats/activity/books into a sink, book by book. A book is passed once its object
// ends: the server writes `requested` (the query's hashes that name it) last.
class BookActivityParser {
 public:
  static constexpr size_t MAX_KEYS = 8;
  // Days kept per book, oldest first (as the server sends them), which bounds the heap while a book is read.
  static constexpr size_t MAX_DAYS = 600;
  struct Sink {
    void* ctx;
    // keys: remoteProgress::keyOf() of each requested hash. startedAt / finishedAt: unix seconds, -1 for
    // none. days: a day with only a position sync has minutes 0. False stops taking books.
    bool (*book)(void* ctx, const uint32_t* keys, size_t keyCount, int64_t startedAt, int64_t finishedAt,
                 const BookDay* days, size_t dayCount);
  };

  // reserve() aborts on failure under -fno-exceptions: check the heap for MAX_DAYS days first.
  explicit BookActivityParser(const Sink& sink);
  BookActivityParser(const BookActivityParser&) = delete;
  BookActivityParser& operator=(const BookActivityParser&) = delete;

  void feed(const char* data, size_t len) { parser.feed(data, len); }
  // The answer parsed whole and the sink took every book.
  bool complete() const { return finished && sinkOk && !parser.hasError(); }
  int books() const { return bookCount; }

 private:
  enum class Key : uint8_t { NONE, BOOKS, STARTED, FINISHED, DAYS, REQUESTED, DAY, SECONDS, SYNCS };
  static constexpr uint8_t ROOT_DEPTH = 1;
  static constexpr uint8_t BOOKS_DEPTH = 2;
  static constexpr uint8_t BOOK_DEPTH = 3;
  static constexpr uint8_t LIST_DEPTH = 4;  // the book's `days` and `requested` arrays
  static constexpr uint8_t DAY_DEPTH = 5;

  static void onKey(void* ctx, const char* key, size_t len);
  static void onString(void* ctx, const char* value, size_t len);
  static void onNumber(void* ctx, const char* value, size_t len);
  static void onBool(void* ctx, bool value);
  static void onNull(void* ctx);
  static void onObjectStart(void* ctx);
  static void onObjectEnd(void* ctx);
  static void onArrayStart(void* ctx);
  static void onArrayEnd(void* ctx);

  StreamingJsonParser parser;
  Sink sink;
  uint8_t depth = 0;
  Key key = Key::NONE;
  bool finished = false;
  bool sinkOk = true;
  bool inBooks = false;
  bool inDays = false;
  bool inRequested = false;
  int bookCount = 0;
  // The book being read.
  uint32_t keys[MAX_KEYS] = {};
  size_t keyCount = 0;
  int64_t startedAt = -1;
  int64_t finishedAt = -1;
  std::vector<BookDay> days;
  // The day being read.
  BookDay day{};
  uint32_t seconds = 0;
  uint32_t syncs = 0;
};
