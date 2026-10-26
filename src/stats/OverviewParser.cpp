#include "OverviewParser.h"

#include <Util.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace {
bool equals(const char* value, const size_t len, const char* expected) {
  return len == strlen(expected) && memcmp(value, expected, len) == 0;
}
}  // namespace

OverviewParser::OverviewParser(std::vector<StatsDigest>& out)
    : parser(JsonCallbacks{this, onKey, onString, onNumber, onBool, onOther, onObjectStart, onObjectEnd, onArrayStart,
                           onArrayEnd}),
      out(out) {}

StatsDigest* OverviewParser::target() {
  if (depth == ROOT_DEPTH && !out.empty()) return &out[0];
  if (depth == GROUP_DEPTH && inGroup) return &out.back();
  return nullptr;
}

void OverviewParser::onKey(void* ctx, const char* k, const size_t len) {
  auto* self = static_cast<OverviewParser*>(ctx);
  self->key = Key::NONE;
  if (self->depth == GROUPS_DEPTH && self->inGroups) {
    // A group's key is its language; one too long for a code is skipped.
    memset(self->pendingCode, 0, sizeof(self->pendingCode));
    if (len >= 1 && len < sizeof(self->pendingCode)) memcpy(self->pendingCode, k, len);
    return;
  }
  if (self->depth == ROOT_DEPTH && equals(k, len, "groups")) {
    self->key = Key::GROUPS;
    return;
  }
  if (!self->target()) return;
  static constexpr struct {
    const char* name;
    Key key;
  } FIELDS[] = {{"anchor_day", Key::ANCHOR},      {"days", Key::DAYS},
                {"history_b64", Key::HISTORY},    {"reading_days", Key::READING_DAYS},
                {"longest_streak", Key::LONGEST}, {"books_finished", Key::FINISHED},
                {"seconds", Key::SECONDS},        {"week_seconds", Key::WEEK}};
  for (const auto& f : FIELDS) {
    if (equals(k, len, f.name)) self->key = f.key;
  }
}

void OverviewParser::onString(void* ctx, const char* value, const size_t len) {
  auto* self = static_cast<OverviewParser*>(ctx);
  StatsDigest* g = self->target();
  if (g && self->key == Key::HISTORY) {
    memset(g->history, 0, sizeof(g->history));
    // A bitmap larger than the record holds is dropped whole: days then counts none of it.
    if (freeink::content::base64Decode(value, len, g->history, sizeof(g->history)) < 0) g->days = 0;
  }
  self->key = Key::NONE;
}

void OverviewParser::onNumber(void* ctx, const char* value, const size_t len) {
  auto* self = static_cast<OverviewParser*>(ctx);
  StatsDigest* g = self->target();
  if (g && self->key != Key::NONE) {
    char buf[16];
    const size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    memcpy(buf, value, n);
    buf[n] = '\0';
    const auto v = static_cast<uint32_t>(strtoul(buf, nullptr, 10));
    const auto small = static_cast<uint16_t>(v > UINT16_MAX ? UINT16_MAX : v);
    switch (self->key) {
      case Key::ANCHOR:
        g->anchorDay = static_cast<int32_t>(v);
        break;
      case Key::DAYS:
        g->days = std::min<uint16_t>(small, sizeof(g->history) * 8);
        break;
      case Key::READING_DAYS:
        g->readingDays = v;
        break;
      case Key::LONGEST:
        g->longestStreak = small;
        break;
      case Key::FINISHED:
        g->booksFinished = small;
        break;
      case Key::SECONDS:
        g->minutes = v / 60;
        break;
      case Key::WEEK:
        g->weekMinutes = v / 60;
        break;
      default:
        break;
    }
  }
  self->key = Key::NONE;
}

void OverviewParser::onOther(void* ctx) { static_cast<OverviewParser*>(ctx)->key = Key::NONE; }
void OverviewParser::onBool(void* ctx, bool) { onOther(ctx); }

void OverviewParser::onObjectStart(void* ctx) {
  auto* self = static_cast<OverviewParser*>(ctx);
  self->depth++;
  if (self->depth == ROOT_DEPTH && self->out.empty() && self->out.capacity() > 0) self->out.push_back(StatsDigest{});
  if (self->depth == GROUPS_DEPTH && self->key == Key::GROUPS) self->inGroups = true;
  if (self->depth == GROUP_DEPTH && self->inGroups && self->pendingCode[0] && self->out.size() < self->out.capacity()) {
    StatsDigest g{};
    memcpy(g.code, self->pendingCode, sizeof(g.code));
    self->out.push_back(g);
    self->inGroup = true;
  }
  self->key = Key::NONE;
}

void OverviewParser::onObjectEnd(void* ctx) {
  auto* self = static_cast<OverviewParser*>(ctx);
  if (self->depth == GROUP_DEPTH) self->inGroup = false;
  if (self->depth == GROUPS_DEPTH) self->inGroups = false;
  if (self->depth > 0 && --self->depth == 0) self->finished = true;
  self->key = Key::NONE;
}

void OverviewParser::onArrayStart(void* ctx) {
  auto* self = static_cast<OverviewParser*>(ctx);
  self->depth++;
  self->key = Key::NONE;
}

void OverviewParser::onArrayEnd(void* ctx) {
  auto* self = static_cast<OverviewParser*>(ctx);
  if (self->depth > 0) self->depth--;
  self->key = Key::NONE;
}
