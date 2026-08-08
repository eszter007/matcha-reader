#pragma once
#include <GfxRenderer.h>

#include <cstdint>

// Card widgets shared by the overall Insights screen and the per-book stats screen.
//
// Extracted rather than copied: the two screens show the same 2x2 tile grid and the same month
// calendar with different copy behind them. Kept as free functions over a renderer (no widget
// objects, no virtuals) so there is nothing to allocate and nothing to keep alive.
namespace StatsWidgets {

// Card geometry, shared so both screens line up pixel for pixel.
constexpr int CARD_PAD = 16;
constexpr int CARD_RADIUS = 12;
constexpr int CARD_GAP = 10;
constexpr int TILE_HEIGHT = 80;
// Hold Back this long on any stats screen to go straight home rather than one step back.
// Matches ReaderUtils::GO_HOME_MS so the gesture feels the same everywhere.
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
// tr() reads from the I18N singleton, so both resolve at call time rather than at static init.
const char* monthName(int month);
const char* dayLabel(int dow);

// Month name shortened to at most 3 CHARACTERS for a button hint ("January" -> "Jan").
// Counts UTF-8 code points, not bytes: a byte-wise cut would split Cyrillic or Japanese mid
// character and render as garbage. Names already shorter (Japanese "1月") are returned intact.
// Runtime abbreviation rather than 12 more strings per language -- three characters is the
// near-universal convention for exactly this, and 384 translations is a poor trade for it.
// month is 1-12; the result is written into out and returned for convenient inline use.
const char* monthAbbrev(int month, char* out, size_t outSize);

// The month before / after the given one, wrapping across the year boundary.
void stepMonth(uint16_t& year, uint8_t& month, int delta);

// Draws the streak card: flame + streak, minutes this week, and the Mon-Sun read/not-read row.
// Returns the height consumed. Shared so the per-language screen matches Insights exactly.
int drawStreakCard(GfxRenderer& renderer, int x, int y, int w, int streak, uint16_t weekMinutes, const bool weekDays[7],
                   int todayDow);

struct Tile {
  const char* value;
  const char* label;
  const uint8_t* icon;
  // The flame icon is only drawn at 32px and sits slightly differently from the 24px set.
  bool bigIcon;
};

// Draws four tiles as a 2x2 grid. Returns the height consumed.
int drawTileGrid(GfxRenderer& renderer, int x, int y, int w, const Tile tiles[4]);

// Where a calendar gets its day data. A function-pointer pair rather than a std::function or a
// virtual: this is called from render() and neither store shares a base class.
struct MonthSource {
  const void* ctx;
  void (*monthStatus)(const void* ctx, uint16_t year, uint8_t month, bool out[32]);
  int (*daysReadInMonth)(const void* ctx, uint16_t year, uint8_t month);
};

// Draws the month card: title with chevrons, "days read" subtitle, weekday headers, and the
// grid. Returns the height consumed. `today` is outlined when it falls in the drawn month.
int drawMonthCalendar(GfxRenderer& renderer, int x, int y, int w, uint16_t calYear, uint8_t calMonth,
                      const Today& today, const MonthSource& source);

}  // namespace StatsWidgets
