#include "BookActivityParser.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iterator>

#include "StatsJson.h"
#include "util/RemoteProgress.h"

namespace {
using statsjson::equals;

int64_t number(const char* v, const size_t len) {
  char buf[24];
  const size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
  memcpy(buf, v, n);
  buf[n] = '\0';
  return strtoll(buf, nullptr, 10);
}

using statsjson::parseDate;
}  // namespace

BookActivityParser::BookActivityParser(const Sink& sink)
    : parser(JsonCallbacks{this, onKey, onString, onNumber, onBool, onNull, onObjectStart, onObjectEnd, onArrayStart,
                           onArrayEnd}),
      sink(sink) {
  days.reserve(MAX_DAYS);
}

void BookActivityParser::onKey(void* ctx, const char* k, const size_t len) {
  auto* self = static_cast<BookActivityParser*>(ctx);
  self->key = Key::NONE;
  const uint8_t d = self->depth;
  if (d == ROOT_DEPTH && equals(k, len, "books")) self->key = Key::BOOKS;
  if (d == BOOK_DEPTH && self->inBooks) {
    if (equals(k, len, "started_at")) self->key = Key::STARTED;
    if (equals(k, len, "finished_at")) self->key = Key::FINISHED;
    if (equals(k, len, "days")) self->key = Key::DAYS;
    if (equals(k, len, "requested")) self->key = Key::REQUESTED;
  }
  if (d == DAY_DEPTH && self->inDays) {
    if (equals(k, len, "day")) self->key = Key::DAY;
    if (equals(k, len, "seconds")) self->key = Key::SECONDS;
    if (equals(k, len, "syncs")) self->key = Key::SYNCS;
  }
}

void BookActivityParser::onString(void* ctx, const char* value, const size_t len) {
  auto* self = static_cast<BookActivityParser*>(ctx);
  if (self->key == Key::DAY && !parseDate(value, len, self->day)) self->day = {};
  if (self->inRequested && self->depth == LIST_DEPTH && self->keyCount < MAX_KEYS) {
    const uint32_t k = remoteProgress::keyOf(value, len);
    if (k != 0) self->keys[self->keyCount++] = k;
  }
  self->key = Key::NONE;
}

void BookActivityParser::onNumber(void* ctx, const char* value, const size_t len) {
  auto* self = static_cast<BookActivityParser*>(ctx);
  const int64_t v = number(value, len);
  if (self->key == Key::STARTED) self->startedAt = v > 0 ? v : -1;  // 0: the server knows no start
  if (self->key == Key::FINISHED) self->finishedAt = v > 0 ? v : -1;
  if (self->key == Key::SECONDS) self->seconds = v > 0 ? static_cast<uint32_t>(v) : 0;
  if (self->key == Key::SYNCS) self->syncs = v > 0 ? static_cast<uint32_t>(v) : 0;
  self->key = Key::NONE;
}

void BookActivityParser::onBool(void* ctx, bool) { static_cast<BookActivityParser*>(ctx)->key = Key::NONE; }

void BookActivityParser::onNull(void* ctx) { static_cast<BookActivityParser*>(ctx)->key = Key::NONE; }

void BookActivityParser::onObjectStart(void* ctx) {
  auto* self = static_cast<BookActivityParser*>(ctx);
  self->depth++;
  if (self->inBooks && self->depth == BOOK_DEPTH) {
    self->keyCount = 0;
    self->startedAt = -1;
    self->finishedAt = -1;
    self->days.clear();
  }
  if (self->inDays && self->depth == DAY_DEPTH) {
    self->day = {};
    self->seconds = 0;
    self->syncs = 0;
  }
  self->key = Key::NONE;
}

void BookActivityParser::onObjectEnd(void* ctx) {
  auto* self = static_cast<BookActivityParser*>(ctx);
  // A reading day: read with recorded time, or opened (a sync). A day that only marks a chosen start or
  // finish is not one; the start comes from started_at.
  if (self->inDays && self->depth == DAY_DEPTH && self->day.month != 0 && (self->seconds > 0 || self->syncs > 0) &&
      self->days.size() < MAX_DAYS) {
    const uint32_t minutes = self->seconds / 60;
    self->day.minutes = static_cast<uint16_t>(minutes > UINT16_MAX ? UINT16_MAX : minutes);
    self->days.push_back(self->day);
  }
  if (self->inBooks && self->depth == BOOK_DEPTH && self->keyCount > 0) {
    self->bookCount++;
    self->sinkOk = self->sinkOk && self->sink.book(self->sink.ctx, self->keys, self->keyCount, self->startedAt,
                                                   self->finishedAt, self->days.data(), self->days.size());
  }
  if (self->depth > 0 && --self->depth == 0) self->finished = true;
  self->key = Key::NONE;
}

void BookActivityParser::onArrayStart(void* ctx) {
  auto* self = static_cast<BookActivityParser*>(ctx);
  self->depth++;
  if (self->depth == BOOKS_DEPTH && self->key == Key::BOOKS) self->inBooks = true;
  if (self->inBooks && self->depth == LIST_DEPTH && self->key == Key::DAYS) self->inDays = true;
  if (self->inBooks && self->depth == LIST_DEPTH && self->key == Key::REQUESTED) self->inRequested = true;
  self->key = Key::NONE;
}

void BookActivityParser::onArrayEnd(void* ctx) {
  auto* self = static_cast<BookActivityParser*>(ctx);
  if (self->depth == BOOKS_DEPTH) self->inBooks = false;
  if (self->depth == LIST_DEPTH) {
    self->inDays = false;
    self->inRequested = false;
  }
  if (self->depth > 0) self->depth--;
  self->key = Key::NONE;
}
