#pragma once

#include <ReadingStatsStore.h>
#include <StreamingJsonParser.h>

#include <cstddef>
#include <cstdint>
#include <vector>

// Streams GET /api/v1/stats/global and keeps the daily minutes of one device: this reader's own
// uploads. `out` must be reserved by the caller; days past its capacity are dropped, never grown into.
// Relies on the server writing each device's `device_id` before its `stats`.
class DailyStatsParser {
 public:
  // firstDay/lastDay: the days kept, as days since 2000-01-01, both included.
  DailyStatsParser(const char* deviceId, std::vector<DailyReading>& out, int32_t firstDay, int32_t lastDay);

  DailyStatsParser(const DailyStatsParser&) = delete;
  DailyStatsParser& operator=(const DailyStatsParser&) = delete;

  void feed(const char* data, size_t len) { parser.feed(data, len); }
  // The whole document parsed without error.
  bool complete() const { return finished && !parser.hasError(); }
  // The server's time for this answer, to pass as `since` next time; 0 from a server without it.
  uint32_t until() const { return untilTime; }

 private:
  enum class Key : uint8_t { NONE, DEVICE_ID, DAILY, DATE, SECONDS, UNTIL };
  static constexpr uint8_t ROOT_DEPTH = 1;
  // Depths of the containers this reads: root, devices, a device, its stats, daily, a day.
  static constexpr uint8_t DEVICE_DEPTH = 3;
  static constexpr uint8_t STATS_DEPTH = 4;
  static constexpr uint8_t DAILY_DEPTH = 5;
  static constexpr uint8_t DAY_DEPTH = 6;

  static void onKey(void* ctx, const char* key, size_t len);
  static void onString(void* ctx, const char* value, size_t len);
  static void onNumber(void* ctx, const char* value, size_t len);
  static void onOther(void* ctx);
  static void onBool(void* ctx, bool);
  static void onObjectStart(void* ctx);
  static void onObjectEnd(void* ctx);
  static void onArrayStart(void* ctx);
  static void onArrayEnd(void* ctx);

  StreamingJsonParser parser;
  const char* deviceId;
  std::vector<DailyReading>& out;
  int32_t firstDay;
  int32_t lastDay;
  uint8_t depth = 0;
  Key key = Key::NONE;
  bool ownDevice = false;
  bool inDaily = false;
  bool finished = false;
  uint32_t untilTime = 0;
  DailyReading day{};
  uint32_t seconds = 0;
};
