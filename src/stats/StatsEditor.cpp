#include "StatsEditor.h"

#include <KOReaderCredentialStore.h>
#include <Logging.h>

#include <algorithm>

#include "BookStats.h"
#include "ReadingStatsStore.h"
#include "StatsSync.h"

namespace statsEditor {

int32_t lockedThroughDay() { return std::max(KOREADER_STORE.getStatsUploadedDay(), KOREADER_STORE.getStatsBooksDay()); }

Result setBookDay(const char* bookPath, const uint16_t year, const uint8_t month, const uint8_t day,
                  const uint16_t minutes) {
  if (!bookPath || !*bookPath) return Result::Failed;
  if (year < 2020 || month < 1 || month > 12 || day < 1 || day > 31) return Result::BadDate;
  const int32_t locked = lockedThroughDay();
  // Day 0 is never a reading day (the store starts in 2020), so 0 means nothing has been sent.
  if (locked > 0 && statsSync::daysSince2000(year, month, day) <= locked) return Result::Locked;

  // Fold the journal in first, so the correction lands on the tables the next replay starts from.
  if (!READING_STATS_STORE.compact()) LOG_ERR("STED", "Stats journal not folded in; editing anyway");

  uint16_t oldMinutes = 0;
  if (!BookStats::setDayMinutes(bookPath, year, month, day, minutes, oldMinutes)) return Result::Failed;
  const int32_t delta = static_cast<int32_t>(minutes) - static_cast<int32_t>(oldMinutes);
  if (delta == 0) return Result::Ok;

  ReadingStatsStore::Loaded stats;
  if (!stats.loaded()) {
    LOG_ERR("STED", "Reading stats unavailable; book day changed, totals not");
    return Result::Failed;
  }
  READING_STATS_STORE.adjustBookDay(bookPath, year, month, day, delta);
  if (!READING_STATS_STORE.saveToFile()) return Result::Failed;
  LOG_INF("STED", "%s %04u-%02u-%02u: %u -> %u min", bookPath, year, month, day, oldMinutes, minutes);
  return Result::Ok;
}

}  // namespace statsEditor
