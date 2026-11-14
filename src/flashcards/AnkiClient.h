#pragma once

#include <ArduinoJson.h>
#include <Memory.h>
#include <SecureHttpClient.h>

#include <cstdint>
#include <string>

#include "JsonValueSkipper.h"

// JSON-RPC 2.0 client for AnkiConnect's API v1, which Anki desktop (with the AnkiConnect add-on)
// and the Matcha companion server both speak. One request at a time; every response goes through
// one bounded heap buffer, so a request costs at most capacity() bytes plus its parsed document.
class AnkiClient {
 public:
  // The largest response kept. Bigger ones (a card full of scripts and inline images) fail with
  // Error::Kind::TooLarge instead of taking the heap.
  static constexpr size_t MAX_RESPONSE = 48 * 1024;
  // With Wi-Fi up the largest free block can be smaller: begin() settles for down to this.
  static constexpr size_t MIN_RESPONSE = 16 * 1024;

  struct Error {
    enum class Kind : uint8_t {
      None,
      NoMemory,
      Network,
      Unauthorized,
      Forbidden,
      Http,
      TooLarge,
      BadReply,
      Rpc,
      DeckMissing,
      Cancelled
    };
    Kind kind = Kind::None;
    int code = 0;      // HTTP status, or the JSON-RPC error code
    std::string type;  // AnkiConnect's error.data.type, e.g. "notFound"
    std::string message;
  };

  // Allocates the response buffer. False when the heap cannot spare it.
  bool begin(const std::string& endpoint, const std::string& key);
  void end();

  // Calls `method` with `params` (an object). On success `result` holds the "result" member. An
  // optional ArduinoJson filter keeps only the fields the caller reads. An optional skipper takes
  // one string member out of the reply as it arrives, so it does not count against capacity().
  bool call(const char* method, const JsonDocument& params, JsonDocument& result, const JsonDocument* filter = nullptr,
            JsonValueSkipper* skip = nullptr);
  bool call(const char* method, JsonDocument& result) {
    JsonDocument none;
    none.to<JsonObject>();
    return call(method, none, result);
  }

  // A long operation (collection.sync, advanced.reviews.import): calls it, then polls
  // operations.get until it finishes. `result` holds the operation's result.
  bool callOperation(const char* method, const JsonDocument& params, JsonDocument& result, uint32_t timeoutMs);

  // Called while a reply arrives (the sync's only long waits), e.g. to read the buttons.
  void setPoll(void (*fn)(void* ctx), void* ctx) {
    poll = fn;
    pollCtx = ctx;
  }

  const Error& error() const { return error_; }
  // The response buffer begin() got: no reply, and so no card side, is longer.
  size_t capacity() const { return capacity_; }
  // The last reply's raw text, valid until the next call; callers may decode strings in place.
  char* replyText() { return buffer.get(); }
  size_t replyLength() const { return length; }
  // A one-line description of the last error for logs and screens.
  std::string describeError() const;

 private:
  freeink::SecureHttpClient http;
  std::string url;
  std::string auth;
  std::unique_ptr<char[]> buffer;
  size_t capacity_ = 0;
  size_t length = 0;
  bool overflow = false;
  uint32_t nextId = 1;
  void (*poll)(void* ctx) = nullptr;
  void* pollCtx = nullptr;
  Error error_;
};
