#pragma once

#include <ReadingStatsStore.h>
#include <StreamingJsonParser.h>

#include <cstddef>
#include <cstdint>
#include <vector>

// Streams GET /api/v1/stats/overview?group_by=language into digests: out[0] for all languages, then one
// per language group. `out` must be reserved by the caller; groups past its capacity are dropped.
class OverviewParser {
 public:
  explicit OverviewParser(std::vector<StatsDigest>& out);

  OverviewParser(const OverviewParser&) = delete;
  OverviewParser& operator=(const OverviewParser&) = delete;

  void feed(const char* data, size_t len) { parser.feed(data, len); }
  // The whole document parsed without error, with the top-level digest in it.
  bool complete() const { return finished && !parser.hasError() && !out.empty() && out[0].anchorDay > 0; }

 private:
  enum class Key : uint8_t { NONE, GROUPS, ANCHOR, DAYS, HISTORY, READING_DAYS, LONGEST, FINISHED, SECONDS, WEEK };
  // Depths of the objects this reads: the answer, its groups, one group.
  static constexpr uint8_t ROOT_DEPTH = 1;
  static constexpr uint8_t GROUPS_DEPTH = 2;
  static constexpr uint8_t GROUP_DEPTH = 3;

  static void onKey(void* ctx, const char* key, size_t len);
  static void onString(void* ctx, const char* value, size_t len);
  static void onNumber(void* ctx, const char* value, size_t len);
  static void onOther(void* ctx);
  static void onBool(void* ctx, bool);
  static void onObjectStart(void* ctx);
  static void onObjectEnd(void* ctx);
  static void onArrayStart(void* ctx);
  static void onArrayEnd(void* ctx);
  // The digest a field at the current depth belongs to, or nullptr.
  StatsDigest* target();

  StreamingJsonParser parser;
  std::vector<StatsDigest>& out;
  uint8_t depth = 0;
  Key key = Key::NONE;
  bool inGroups = false;
  bool inGroup = false;  // inside a group this kept
  bool finished = false;
  char pendingCode[4] = {};  // the group key just read
};
