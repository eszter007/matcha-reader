#pragma once
#include <GfxRenderer.h>

#include <cstdint>

// Cards shared by the Insights, per-book and per-language stats screens.
// Free functions over a renderer: nothing to allocate, nothing to keep alive.
namespace StatsWidgets {

// Shared so the screens line up pixel for pixel.
constexpr int CARD_PAD = 16;
constexpr int CARD_RADIUS = 12;
constexpr int CARD_GAP = 10;
constexpr int TILE_HEIGHT = 80;
// Hold Back this long to go home instead of one step back. Matches ReaderUtils::GO_HOME_MS.
constexpr unsigned long HOME_HOLD_MS = 1000;

struct Today {
  uint16_t year;
  uint8_t month, day;
  int dow;  // 0=Mon..6=Sun (ISO)
};

// Local-midnight day boundary, matching how the readers record stats.
Today getToday();

int daysInMonth(uint16_t y, uint8_t m);
// Day of week for the 1st of a month (0=Mon..6=Sun ISO).
int firstDowOfMonth(uint16_t y, uint8_t m);
// Resolved at call time: tr() reads the I18N singleton, which is not ready at static init.
const char* monthName(int month);
const char* dayLabel(int dow);

// Month name cut to 3 CODE POINTS for a button hint ("January" -> "Jan"). Code points, not
// bytes: a byte-wise cut splits Cyrillic and Japanese mid-character. Shorter names pass through.
// Done at runtime rather than as 12 more strings per language (384 translations).
const char* monthAbbrev(int month, char* out, size_t outSize);

void stepMonth(uint16_t& year, uint8_t& month, int delta);

// Flame + streak, minutes this week, Mon-Sun read/not-read row. Returns height consumed.
int drawStreakCard(const GfxRenderer& renderer, int x, int y, int w, int streak, uint16_t weekMinutes,
                   const bool weekDays[7], int todayDow);

struct Tile {
  const char* value;
  const char* label;
  const uint8_t* icon;
  bool bigIcon;  // flame is 32px and offset differently from the 24px set
};

// 2x2 grid. Returns height consumed.
int drawTileGrid(const GfxRenderer& renderer, int x, int y, int w, const Tile tiles[4]);

// Day data source. Function pointers, not std::function or a virtual: called from render(),
// and the two stores share no base class.
struct MonthSource {
  const void* ctx;
  void (*monthStatus)(const void* ctx, uint16_t year, uint8_t month, bool out[32]);
  int (*daysReadInMonth)(const void* ctx, uint16_t year, uint8_t month);
};

// Month card: title with chevrons, subtitle, weekday headers, grid. Returns height consumed.
// `today` is outlined when it falls in the drawn month.
int drawMonthCalendar(const GfxRenderer& renderer, int x, int y, int w, uint16_t calYear, uint8_t calMonth,
                      const Today& today, const MonthSource& source);

}  // namespace StatsWidgets
