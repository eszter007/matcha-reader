#include "BookListParser.h"

#include <cstdlib>
#include <cstring>

namespace {
bool equals(const char* value, const size_t len, const char* expected) {
  return len == strlen(expected) && memcmp(value, expected, len) == 0;
}

uint32_t number(const char* v, const size_t len) {
  char buf[16];
  const size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
  memcpy(buf, v, n);
  buf[n] = '\0';
  return static_cast<uint32_t>(strtoul(buf, nullptr, 10));
}

bool parseDate(const char* v, const size_t len, BookDay& out) {
  if (len != 10 || v[4] != '-' || v[7] != '-') return false;
  for (const size_t i : {0, 1, 2, 3, 5, 6, 8, 9}) {
    if (v[i] < '0' || v[i] > '9') return false;
  }
  out.year = static_cast<uint16_t>((v[0] - '0') * 1000 + (v[1] - '0') * 100 + (v[2] - '0') * 10 + (v[3] - '0'));
  out.month = static_cast<uint8_t>((v[5] - '0') * 10 + (v[6] - '0'));
  out.day = static_cast<uint8_t>((v[8] - '0') * 10 + (v[9] - '0'));
  return out.month >= 1 && out.month <= 12 && out.day >= 1 && out.day <= 31;
}
}  // namespace

BookListParser::BookListParser(const Sink& sink)
    : parser(JsonCallbacks{this, onKey, onString, onNumber, onBool, onNull, onObjectStart, onObjectEnd, onArrayStart,
                           onArrayEnd}),
      sink(sink) {}

void BookListParser::beginBook() {
  if (begun || !hasName) return;
  begun = true;
  bookCount++;
  sinkOk = sinkOk && sink.begin(sink.ctx, nameHash, completed, sessions, language);
}

void BookListParser::onKey(void* ctx, const char* k, const size_t len) {
  auto* self = static_cast<BookListParser*>(ctx);
  self->key = Key::NONE;
  const uint8_t d = self->depth;
  if (d == ROOT_DEPTH && equals(k, len, "next")) self->key = Key::NEXT;
  if (d == ITEM_DEPTH && equals(k, len, "filename")) self->key = Key::FILENAME;
  if (d == ITEM_DEPTH && equals(k, len, "language")) self->key = Key::LANGUAGE;
  if (d == STATS_DEPTH && equals(k, len, "sessions")) self->key = Key::SESSIONS;
  if (d == STATS_DEPTH && equals(k, len, "completed")) self->key = Key::COMPLETED;
  if (d == STATS_DEPTH && equals(k, len, "daily")) self->key = Key::DAILY;
  if (d == DAY_DEPTH && self->inDaily && equals(k, len, "date")) self->key = Key::DATE;
  if (d == DAY_DEPTH && self->inDaily && equals(k, len, "seconds")) self->key = Key::SECONDS;
}

void BookListParser::onString(void* ctx, const char* value, const size_t len) {
  auto* self = static_cast<BookListParser*>(ctx);
  switch (self->key) {
    case Key::NEXT: {
      const size_t n = len < sizeof(self->nextCursor) - 1 ? len : sizeof(self->nextCursor) - 1;
      memcpy(self->nextCursor, value, n);
      self->nextCursor[n] = '\0';
      break;
    }
    case Key::FILENAME:
      self->hasName = len > 0;
      self->nameHash = bookNameHash(value, len);
      break;
    case Key::LANGUAGE: {
      const size_t n = len < sizeof(self->language) - 1 ? len : sizeof(self->language) - 1;
      memcpy(self->language, value, n);
      self->language[n] = '\0';
      break;
    }
    case Key::DATE:
      if (!parseDate(value, len, self->day)) self->day = {};
      break;
    default:
      break;
  }
  self->key = Key::NONE;
}

void BookListParser::onNumber(void* ctx, const char* value, const size_t len) {
  auto* self = static_cast<BookListParser*>(ctx);
  if (self->key == Key::SESSIONS) self->sessions = number(value, len);
  if (self->key == Key::SECONDS) self->seconds = number(value, len);
  self->key = Key::NONE;
}

void BookListParser::onBool(void* ctx, const bool value) {
  auto* self = static_cast<BookListParser*>(ctx);
  if (self->key == Key::COMPLETED) self->completed = value;
  self->key = Key::NONE;
}

void BookListParser::onNull(void* ctx) {
  auto* self = static_cast<BookListParser*>(ctx);
  if (self->key == Key::NEXT) self->nextCursor[0] = '\0';
  self->key = Key::NONE;
}

void BookListParser::onObjectStart(void* ctx) {
  auto* self = static_cast<BookListParser*>(ctx);
  self->depth++;
  if (self->depth == ITEM_DEPTH) {
    self->nameHash = 0;
    self->hasName = false;
    self->language[0] = '\0';
    self->sessions = 0;
    self->completed = false;
    self->begun = false;
  }
  if (self->inDaily && self->depth == DAY_DEPTH) {
    self->day = {};
    self->seconds = 0;
  }
  self->key = Key::NONE;
}

void BookListParser::onObjectEnd(void* ctx) {
  auto* self = static_cast<BookListParser*>(ctx);
  if (self->inDaily && self->depth == DAY_DEPTH && self->day.month != 0 && self->seconds >= 60) {
    self->beginBook();
    if (self->begun) {
      const uint32_t minutes = self->seconds / 60;
      self->day.minutes = static_cast<uint16_t>(minutes > UINT16_MAX ? UINT16_MAX : minutes);
      self->sinkOk = self->sinkOk && self->sink.day(self->sink.ctx, self->day);
    }
  }
  if (self->depth == ITEM_DEPTH) {
    self->beginBook();  // a book with no days still brings its sessions and finished state
    if (self->begun) self->sinkOk = self->sinkOk && self->sink.end(self->sink.ctx);
  }
  if (self->depth > 0 && --self->depth == 0) self->finished = true;
  self->key = Key::NONE;
}

void BookListParser::onArrayStart(void* ctx) {
  auto* self = static_cast<BookListParser*>(ctx);
  self->depth++;
  if (self->depth == DAILY_DEPTH && self->key == Key::DAILY) self->inDaily = true;
  self->key = Key::NONE;
}

void BookListParser::onArrayEnd(void* ctx) {
  auto* self = static_cast<BookListParser*>(ctx);
  if (self->depth == DAILY_DEPTH) self->inDaily = false;
  if (self->depth > 0) self->depth--;
  self->key = Key::NONE;
}
