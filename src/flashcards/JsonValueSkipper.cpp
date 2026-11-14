#include "JsonValueSkipper.h"

#include <cstring>

JsonValueSkipper::JsonValueSkipper(const char* key) { keys_[0] = key; }

JsonValueSkipper::JsonValueSkipper(const char* const* keys) {
  for (size_t i = 0; i < MAX_KEYS && keys[i]; i++) keys_[i] = keys[i];
}

void JsonValueSkipper::take(const char c) {
  if (key_ != 0) return;
  hash_ = (hash_ ^ static_cast<uint8_t>(c)) * 16777619u;
  valueBytes_++;
  if (sink_) decode(c);
}

bool JsonValueSkipper::feed(const char* in, const size_t len, char* out, size_t& outLen, const size_t outCap) {
  for (size_t i = 0; i < len; i++) {
    const char c = in[i];
    bool emit = true;
    switch (state_) {
      case State::Value:
        if (c == '\\') {
          take(c);
          state_ = State::ValueEsc;
          emit = false;
        } else if (c == '"') {
          state_ = State::Out;
        } else {
          take(c);
          emit = false;
        }
        break;
      case State::ValueEsc:
        take(c);
        state_ = State::Value;
        emit = false;
        break;
      case State::Str:
        if (c == '\\') {
          nameFits_ = false;  // an escaped name is never one of ours
          state_ = State::StrEsc;
        } else if (c == '"') {
          state_ = State::Out;
          if (nameFits_) {
            name_[nameLen_] = '\0';
            for (size_t k = 0; k < MAX_KEYS && keys_[k]; k++) {
              if (strcmp(name_, keys_[k]) == 0) {
                state_ = State::AfterKey;
                key_ = static_cast<uint8_t>(k);
              }
            }
          }
        } else if (nameLen_ + 1 < sizeof(name_)) {
          name_[nameLen_++] = c;
        } else {
          nameFits_ = false;
        }
        break;
      case State::StrEsc:
        state_ = State::Str;
        break;
      case State::AfterKey:
      case State::AfterColon:
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') break;
        if (state_ == State::AfterKey && c == ':') {
          state_ = State::AfterColon;
        } else if (state_ == State::AfterColon && c == '"') {
          state_ = State::Value;
        } else {
          state_ = State::Out;
          if (c == '"') {
            state_ = State::Str;
            nameLen_ = 0;
            nameFits_ = true;
          }
        }
        break;
      case State::Out:
        if (c == '"') {
          state_ = State::Str;
          nameLen_ = 0;
          nameFits_ = true;
        }
        break;
    }
    if (!emit) continue;
    if (outLen >= outCap) return false;
    out[outLen++] = c;
  }
  return true;
}

namespace {
void appendUtf8(char* s, size_t& w, const uint32_t cp) {
  if (cp < 0x80) {
    s[w++] = static_cast<char>(cp);
  } else if (cp < 0x800) {
    s[w++] = static_cast<char>(0xC0 | (cp >> 6));
    s[w++] = static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    s[w++] = static_cast<char>(0xE0 | (cp >> 12));
    s[w++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    s[w++] = static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    s[w++] = static_cast<char>(0xF0 | (cp >> 18));
    s[w++] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    s[w++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    s[w++] = static_cast<char>(0x80 | (cp & 0x3F));
  }
}

bool hex4(const char* s, const size_t size, const size_t at, uint32_t& out) {
  if (at + 4 > size) return false;
  out = 0;
  for (size_t i = at; i < at + 4; i++) {
    const char c = s[i];
    out <<= 4;
    if (c >= '0' && c <= '9') {
      out |= c - '0';
    } else if (c >= 'a' && c <= 'f') {
      out |= c - 'a' + 10;
    } else if (c >= 'A' && c <= 'F') {
      out |= c - 'A' + 10;
    } else {
      return false;
    }
  }
  return true;
}
}  // namespace

// Every escape is at least as long as what it decodes to, so the text is rewritten in place.
static size_t unescapeInPlace(char* text, const size_t size) {
  size_t w = 0;
  for (size_t r = 0; r < size; r++) {
    const char c = text[r];
    if (c != '\\' || r + 1 >= size) {
      text[w++] = c;
      continue;
    }
    const char e = text[++r];
    switch (e) {
      case 'b':
        text[w++] = '\b';
        break;
      case 'f':
        text[w++] = '\f';
        break;
      case 'n':
        text[w++] = '\n';
        break;
      case 'r':
        text[w++] = '\r';
        break;
      case 't':
        text[w++] = '\t';
        break;
      case 'u': {
        uint32_t cp = 0;
        if (!hex4(text, size, r + 1, cp)) break;
        r += 4;
        uint32_t low = 0;
        if (cp >= 0xD800 && cp < 0xDC00 && r + 2 < size && text[r + 1] == '\\' && text[r + 2] == 'u' &&
            hex4(text, size, r + 3, low) && low >= 0xDC00 && low < 0xE000) {
          cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
          r += 6;
        }
        appendUtf8(text, w, cp);
        break;
      }
      default:  // \" \\ \/
        text[w++] = e;
        break;
    }
  }
  return w;
}

void takeJsonStrings(char* json, const size_t len, const char* const* keys, char** values, const size_t count) {
  size_t starts[4] = {}, ends[4] = {};
  const size_t n = count < 4 ? count : 4;
  for (size_t k = 0; k < n; k++) values[k] = nullptr;
  bool inString = false;
  size_t strStart = 0;
  int pending = -1;  // a key just closed, waiting for ':' and its value
  for (size_t i = 0; i < len; i++) {
    const char c = json[i];
    if (inString) {
      if (c == '\\') {
        i++;
      } else if (c == '"') {
        inString = false;
        pending = -1;
        for (size_t k = 0; k < n; k++) {
          const size_t keyLen = strlen(keys[k]);
          if (i - strStart == keyLen && memcmp(json + strStart, keys[k], keyLen) == 0) pending = static_cast<int>(k);
        }
      }
      continue;
    }
    if (c == '"') {
      if (pending >= 0 && starts[pending] == 0 && i > 0) {
        // The value of a wanted member: find its end without reading it as a key.
        size_t j = i + 1;
        while (j < len && json[j] != '"') j += json[j] == '\\' ? 2 : 1;
        if (j >= len) return;
        starts[pending] = i + 1;
        ends[pending] = j;
        pending = -1;
        i = j;
        continue;
      }
      inString = true;
      strStart = i + 1;
    } else if (c != ':' && c != ' ' && c != '\t' && c != '\n' && c != '\r') {
      pending = -1;
    }
  }
  for (size_t k = 0; k < n; k++) {
    if (starts[k] == 0) continue;
    const size_t w = unescapeInPlace(json + starts[k], ends[k] - starts[k]);
    json[starts[k] + w] = '\0';
    values[k] = json + starts[k];
  }
}

// Escapes arrive split across network chunks: one is held until it is complete.
void JsonValueSkipper::decode(const char c) {
  if (escapeLen_ == 0 && c != '\\') {
    sink_(sinkCtx_, c);
    return;
  }
  escape_[escapeLen_++] = c;
  if (escapeLen_ == 2 && escape_[1] != 'u') return flushEscape();
  if (escapeLen_ == 6) {
    uint32_t cp = 0;
    if (!hex4(escape_, 6, 2, cp) || cp < 0xD800 || cp >= 0xDC00) return flushEscape();
    return;  // a high surrogate: its pair may follow
  }
  if ((escapeLen_ == 7 && c != '\\') || (escapeLen_ == 8 && c != 'u')) {
    // Not a pair after all: the high surrogate alone, then what followed it.
    const uint8_t rest = static_cast<uint8_t>(escapeLen_ - 6);
    const char tail[2] = {escape_[6], escape_[7]};
    escapeLen_ = 6;
    flushEscape();
    for (uint8_t i = 0; i < rest; i++) decode(tail[i]);
    return;
  }
  if (escapeLen_ == sizeof(escape_)) flushEscape();
}

void JsonValueSkipper::flushEscape() {
  const size_t n = unescapeInPlace(escape_, escapeLen_);
  escapeLen_ = 0;
  for (size_t i = 0; i < n; i++) sink_(sinkCtx_, escape_[i]);
}

void JsonValueSkipper::finish() {
  if (sink_ && escapeLen_ > 0) flushEscape();
}
