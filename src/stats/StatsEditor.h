#pragma once

#include <cstdint>

// Corrections to recorded reading time from the web Edit Insights page. Only days the sync server has not
// received yet can be changed: it keeps the larger value per day, so a day already uploaded would
// come back on the next download.
namespace statsEditor {

// The last day sent to the sync server, in days since 2000-01-01; 0 when nothing was sent.
int32_t lockedThroughDay();

// Sets one book's minutes on one day and corrects the overall totals by the difference.
enum class Result : uint8_t { Ok, Locked, BadDate, Failed };
Result setBookDay(const char* bookPath, uint16_t year, uint8_t month, uint8_t day, uint16_t minutes);

}  // namespace statsEditor
