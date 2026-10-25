#include "DailyStatsParser.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iterator>

#include "StatsSync.h"

namespace {
bool equals(const char* value, const size_t len, const char* expected) {
  return len == strlen(expected) && memcmp(value, expected, len) == 0;
}

// "YYYY-MM-DD"; false for anything else.
bool parseDate(const char* v, const size_t len, DailyReading& out) {
  static constexpr size_t DIGITS[] = {0, 1, 2, 3, 5, 6, 8, 9};
  if (len != 10 || v[4] != '-' || v[7] != '-' ||
      !std::all_of(std::begin(DIGITS), std::end(DIGITS), [v](const size_t i) { return v[i] >= '0' && v[i] <= '9'; })) {
    return false;
  }
  const auto num = [v](const size_t at, const size_t n) {
    int x = 0;
    for (size_t i = 0; i < n; i++) x = x * 10 + (v[at + i] - '0');
    return x;
  };
  out.year = static_cast<uint16_t>(num(0, 4));
  out.month = static_cast<uint8_t>(num(5, 2));
  out.day = static_cast<uint8_t>(num(8, 2));
  return out.month >= 1 && out.month <= 12 && out.day >= 1 && out.day <= 31;
}
}  // namespace

DailyStatsParser::DailyStatsParser(const char* deviceId, std::vector<DailyReading>& out, const int32_t firstDay,
                                   const int32_t lastDay)
    : parser(JsonCallbacks{this, onKey, onString, onNumber, onBool, onOther, onObjectStart, onObjectEnd, onArrayStart,
                           onArrayEnd}),
      deviceId(deviceId),
      out(out),
      firstDay(firstDay),
      lastDay(lastDay) {}

void DailyStatsParser::onKey(void* ctx, const char* k, const size_t len) {
  auto* self = static_cast<DailyStatsParser*>(ctx);
  self->key = Key::NONE;
  if (self->depth == ROOT_DEPTH && equals(k, len, "until")) self->key = Key::UNTIL;
  if (self->depth == DEVICE_DEPTH && equals(k, len, "device_id")) self->key = Key::DEVICE_ID;
  if (self->depth == STATS_DEPTH && self->ownDevice && equals(k, len, "daily")) self->key = Key::DAILY;
  if (self->depth == DAY_DEPTH && self->inDaily && equals(k, len, "date")) self->key = Key::DATE;
  if (self->depth == DAY_DEPTH && self->inDaily && equals(k, len, "seconds")) self->key = Key::SECONDS;
}

void DailyStatsParser::onString(void* ctx, const char* value, const size_t len) {
  auto* self = static_cast<DailyStatsParser*>(ctx);
  if (self->key == Key::DEVICE_ID) self->ownDevice = equals(value, len, self->deviceId);
  if (self->key == Key::DATE && !parseDate(value, len, self->day)) self->day = {};
  self->key = Key::NONE;
}

void DailyStatsParser::onNumber(void* ctx, const char* value, const size_t len) {
  auto* self = static_cast<DailyStatsParser*>(ctx);
  if (self->key == Key::SECONDS || self->key == Key::UNTIL) {
    char buf[16];
    const size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    memcpy(buf, value, n);
    buf[n] = '\0';
    const auto v = static_cast<uint32_t>(strtoul(buf, nullptr, 10));
    if (self->key == Key::SECONDS) self->seconds = v;
    if (self->key == Key::UNTIL) self->untilTime = v;
  }
  self->key = Key::NONE;
}

void DailyStatsParser::onOther(void* ctx) { static_cast<DailyStatsParser*>(ctx)->key = Key::NONE; }
void DailyStatsParser::onBool(void* ctx, bool) { onOther(ctx); }

void DailyStatsParser::onObjectStart(void* ctx) {
  auto* self = static_cast<DailyStatsParser*>(ctx);
  self->depth++;
  if (self->inDaily && self->depth == DAY_DEPTH) {
    self->day = {};
    self->seconds = 0;
  }
  self->key = Key::NONE;
}

void DailyStatsParser::onObjectEnd(void* ctx) {
  auto* self = static_cast<DailyStatsParser*>(ctx);
  if (self->inDaily && self->depth == DAY_DEPTH && self->day.month != 0 && self->seconds >= 60) {
    const int32_t d = statsSync::daysSince2000(self->day.year, self->day.month, self->day.day);
    if (d >= self->firstDay && d <= self->lastDay && self->out.size() < self->out.capacity()) {
      const uint32_t minutes = self->seconds / 60;
      self->day.minutesRead = static_cast<uint16_t>(minutes > UINT16_MAX ? UINT16_MAX : minutes);
      self->out.push_back(self->day);
    }
  }
  if (self->depth == DEVICE_DEPTH) self->ownDevice = false;
  if (self->depth > 0 && --self->depth == 0) self->finished = true;
  self->key = Key::NONE;
}

void DailyStatsParser::onArrayStart(void* ctx) {
  auto* self = static_cast<DailyStatsParser*>(ctx);
  self->depth++;
  if (self->depth == DAILY_DEPTH && self->key == Key::DAILY) self->inDaily = true;
  self->key = Key::NONE;
}

void DailyStatsParser::onArrayEnd(void* ctx) {
  auto* self = static_cast<DailyStatsParser*>(ctx);
  if (self->depth == DAILY_DEPTH) self->inDaily = false;
  if (self->depth > 0) self->depth--;
  self->key = Key::NONE;
}
