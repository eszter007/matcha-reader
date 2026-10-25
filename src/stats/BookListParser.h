#pragma once

#include <BookStats.h>
#include <StreamingJsonParser.h>

#include <cstddef>
#include <cstdint>

// Streams one page of GET /api/v1/stats/books into a sink, book by book: begin, its days in the order the
// server sends them (by date), end. A book without a filename is skipped: it could never be matched to a
// file on the card. Relies on the server writing each item's filename and language before its stats, and
// the stats' counters before their daily list.
class BookListParser {
 public:
  struct Sink {
    void* ctx;
    bool (*begin)(void* ctx, uint32_t nameHash, bool finished, uint32_t sessions, const char* language);
    bool (*day)(void* ctx, const BookDay& day);
    bool (*end)(void* ctx);
  };

  explicit BookListParser(const Sink& sink);
  BookListParser(const BookListParser&) = delete;
  BookListParser& operator=(const BookListParser&) = delete;

  void feed(const char* data, size_t len) { parser.feed(data, len); }
  // The page parsed whole and the sink took every book.
  bool complete() const { return finished && sinkOk && !parser.hasError(); }
  // The cursor for the next page; empty on the last one.
  const char* next() const { return nextCursor; }
  int books() const { return bookCount; }

 private:
  enum class Key : uint8_t { NONE, FILENAME, LANGUAGE, SESSIONS, COMPLETED, DAILY, DATE, SECONDS, NEXT };
  static constexpr uint8_t ROOT_DEPTH = 1;
  static constexpr uint8_t ITEM_DEPTH = 3;
  static constexpr uint8_t STATS_DEPTH = 4;
  static constexpr uint8_t DAILY_DEPTH = 5;
  static constexpr uint8_t DAY_DEPTH = 6;

  static void onKey(void* ctx, const char* key, size_t len);
  static void onString(void* ctx, const char* value, size_t len);
  static void onNumber(void* ctx, const char* value, size_t len);
  static void onBool(void* ctx, bool value);
  static void onNull(void* ctx);
  static void onObjectStart(void* ctx);
  static void onObjectEnd(void* ctx);
  static void onArrayStart(void* ctx);
  static void onArrayEnd(void* ctx);
  void beginBook();

  StreamingJsonParser parser;
  Sink sink;
  uint8_t depth = 0;
  Key key = Key::NONE;
  bool finished = false;
  bool sinkOk = true;
  int bookCount = 0;
  char nextCursor[192] = {};
  // The item being read.
  uint32_t nameHash = 0;
  bool hasName = false;
  char language[16] = {};
  uint32_t sessions = 0;
  bool completed = false;
  bool begun = false;
  bool inDaily = false;
  BookDay day{};
  uint32_t seconds = 0;
};
