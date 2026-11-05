#include "ProgressChangesParser.h"

#include <cstdlib>
#include <cstring>

#include "util/RemoteProgress.h"

namespace {
bool equals(const char* value, const size_t len, const char* expected) {
  return len == strlen(expected) && memcmp(value, expected, len) == 0;
}

// A JSON number into a NUL-terminated buffer for strto*.
void copyNumber(const char* v, const size_t len, char (&buf)[32]) {
  const size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
  memcpy(buf, v, n);
  buf[n] = '\0';
}
}  // namespace

ProgressChangesParser::ProgressChangesParser(const Sink& sink)
    : parser(JsonCallbacks{this, onKey, onString, onNumber, onBool, onNull, onObjectStart, onObjectEnd, onArrayStart,
                           onArrayEnd}),
      sink(sink) {}

void ProgressChangesParser::addKey(const char* value, const size_t len) {
  const uint32_t k = remoteProgress::keyOf(value, len);
  if (k != 0 && keyCount < MAX_KEYS) keys[keyCount++] = k;
}

void ProgressChangesParser::onKey(void* ctx, const char* k, const size_t len) {
  auto* self = static_cast<ProgressChangesParser*>(ctx);
  self->key = Key::NONE;
  const uint8_t d = self->depth;
  if (d == ROOT_DEPTH && equals(k, len, "cursor")) self->key = Key::CURSOR;
  if (d == ROOT_DEPTH && equals(k, len, "more")) self->key = Key::MORE;
  if (d == ROOT_DEPTH && equals(k, len, "changes")) self->key = Key::CHANGES;
  if (d == ITEM_DEPTH && self->inChanges && equals(k, len, "document")) self->key = Key::DOCUMENT;
  if (d == ITEM_DEPTH && self->inChanges && equals(k, len, "aliases")) self->key = Key::ALIASES;
  if (d == ITEM_DEPTH && self->inChanges && equals(k, len, "percentage")) self->key = Key::PERCENTAGE;
}

void ProgressChangesParser::onString(void* ctx, const char* value, const size_t len) {
  auto* self = static_cast<ProgressChangesParser*>(ctx);
  if (self->key == Key::DOCUMENT || (self->inAliases && self->depth == ALIASES_DEPTH)) self->addKey(value, len);
  self->key = Key::NONE;
}

void ProgressChangesParser::onNumber(void* ctx, const char* value, const size_t len) {
  auto* self = static_cast<ProgressChangesParser*>(ctx);
  char buf[32];
  copyNumber(value, len, buf);
  if (self->key == Key::CURSOR) {
    self->nextCursor = strtoull(buf, nullptr, 10);
    self->hasCursor = true;
  }
  if (self->key == Key::PERCENTAGE) self->percentage = strtof(buf, nullptr);
  self->key = Key::NONE;
}

void ProgressChangesParser::onBool(void* ctx, const bool value) {
  auto* self = static_cast<ProgressChangesParser*>(ctx);
  if (self->key == Key::MORE) self->hasMore = value;
  self->key = Key::NONE;
}

void ProgressChangesParser::onNull(void* ctx) { static_cast<ProgressChangesParser*>(ctx)->key = Key::NONE; }

void ProgressChangesParser::onObjectStart(void* ctx) {
  auto* self = static_cast<ProgressChangesParser*>(ctx);
  self->depth++;
  if (self->inChanges && self->depth == ITEM_DEPTH) {
    self->keyCount = 0;
    self->percentage = -1;
  }
  self->key = Key::NONE;
}

void ProgressChangesParser::onObjectEnd(void* ctx) {
  auto* self = static_cast<ProgressChangesParser*>(ctx);
  // A change without a usable hash or percentage is skipped, never read as "not finished".
  if (self->inChanges && self->depth == ITEM_DEPTH && self->keyCount > 0 && self->percentage >= 0) {
    self->changeCount++;
    self->sinkOk = self->sinkOk && self->sink.change(self->sink.ctx, self->keys, self->keyCount, self->percentage);
  }
  if (self->depth > 0 && --self->depth == 0) self->finished = true;
  self->key = Key::NONE;
}

void ProgressChangesParser::onArrayStart(void* ctx) {
  auto* self = static_cast<ProgressChangesParser*>(ctx);
  self->depth++;
  if (self->depth == CHANGES_DEPTH && self->key == Key::CHANGES) self->inChanges = true;
  if (self->depth == ALIASES_DEPTH && self->inChanges && self->key == Key::ALIASES) self->inAliases = true;
  self->key = Key::NONE;
}

void ProgressChangesParser::onArrayEnd(void* ctx) {
  auto* self = static_cast<ProgressChangesParser*>(ctx);
  if (self->depth == CHANGES_DEPTH) self->inChanges = false;
  if (self->depth == ALIASES_DEPTH) self->inAliases = false;
  if (self->depth > 0) self->depth--;
  self->key = Key::NONE;
}
