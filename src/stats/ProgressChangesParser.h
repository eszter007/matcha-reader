#pragma once

#include <StreamingJsonParser.h>

#include <cstddef>
#include <cstdint>

// Streams one page of GET /api/v1/progress/changes: for each document, the keys it is known by (its hash
// and its aliases, see remoteProgress::keyOf) and its newest percentage.
class ProgressChangesParser {
 public:
  static constexpr size_t MAX_KEYS = 8;  // a document and up to 7 aliases; more are dropped
  struct Sink {
    void* ctx;
    // False stops taking changes: the page then does not count as complete.
    bool (*change)(void* ctx, const uint32_t* keys, size_t count, float percentage);
  };

  explicit ProgressChangesParser(const Sink& sink);
  ProgressChangesParser(const ProgressChangesParser&) = delete;
  ProgressChangesParser& operator=(const ProgressChangesParser&) = delete;

  void feed(const char* data, size_t len) { parser.feed(data, len); }
  // The page parsed whole and the sink took every change.
  bool complete() const { return finished && sinkOk && hasCursor && !parser.hasError(); }
  uint64_t cursor() const { return nextCursor; }
  bool more() const { return hasMore; }
  int changes() const { return changeCount; }

 private:
  enum class Key : uint8_t { NONE, CURSOR, MORE, CHANGES, DOCUMENT, ALIASES, PERCENTAGE };
  static constexpr uint8_t ROOT_DEPTH = 1;
  static constexpr uint8_t CHANGES_DEPTH = 2;
  static constexpr uint8_t ITEM_DEPTH = 3;
  static constexpr uint8_t ALIASES_DEPTH = 4;

  static void onKey(void* ctx, const char* key, size_t len);
  static void onString(void* ctx, const char* value, size_t len);
  static void onNumber(void* ctx, const char* value, size_t len);
  static void onBool(void* ctx, bool value);
  static void onNull(void* ctx);
  static void onObjectStart(void* ctx);
  static void onObjectEnd(void* ctx);
  static void onArrayStart(void* ctx);
  static void onArrayEnd(void* ctx);
  void addKey(const char* value, size_t len);

  StreamingJsonParser parser;
  Sink sink;
  uint8_t depth = 0;
  Key key = Key::NONE;
  bool finished = false;
  bool sinkOk = true;
  bool hasCursor = false;
  bool hasMore = false;
  bool inChanges = false;
  bool inAliases = false;
  int changeCount = 0;
  uint64_t nextCursor = 0;
  // The change being read.
  uint32_t keys[MAX_KEYS] = {};
  size_t keyCount = 0;
  float percentage = -1;
};
