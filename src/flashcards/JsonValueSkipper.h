#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Takes one string member out of a JSON reply while it streams in, e.g. the "css" of
// cards.render, which repeats the note type's whole style sheet in every card: the reply keeps
// `"css":""`, and the value's raw (still escaped) bytes are hashed and, when asked, kept apart.
// The value is assumed to be a string; any member with that name, at any depth, is taken.
class JsonValueSkipper {
 public:
  // `key` is the member name without quotes, at most 15 bytes.
  explicit JsonValueSkipper(const char* key);

  // Copies `in` to `out` without the value's bytes. False when `out` would pass `outCap`.
  bool feed(const char* in, size_t len, char* out, size_t& outLen, size_t outCap);

  // FNV-1a of the value's raw bytes, and their count, after the whole reply was fed.
  uint32_t hash() const { return hash_; }
  size_t valueBytes() const { return valueBytes_; }

  // Hands each byte of the value, decoded, to `out` as it arrives.
  using Sink = void (*)(void* ctx, char c);
  void decodeTo(Sink out, void* ctx) {
    sink_ = out;
    sinkCtx_ = ctx;
  }
  // Passes on an escape still incomplete at the end of the value (a malformed reply).
  void finish();

 private:
  enum class State : uint8_t { Out, Str, StrEsc, AfterKey, AfterColon, Value, ValueEsc };
  void take(char c);

  char key_[16];
  uint8_t keyLen_;
  uint8_t matched_ = 0;
  bool keyMatches_ = false;
  State state_ = State::Out;
  uint32_t hash_ = 2166136261u;
  size_t valueBytes_ = 0;
  Sink sink_ = nullptr;
  void* sinkCtx_ = nullptr;
  char escape_[12];  // a \uXXXX\uXXXX pair at most
  uint8_t escapeLen_ = 0;
  void decode(char c);
  void flushEscape();
};

// Finds the string values of up to `count` members of a JSON text by name and decodes each in
// place, null-terminated, so no copy is made. Unfound members are left nullptr. The text is
// changed: parse anything else from it first.
void takeJsonStrings(char* json, size_t len, const char* const* keys, char** values, size_t count);
