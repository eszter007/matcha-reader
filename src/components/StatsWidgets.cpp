#include "StatsWidgets.h"

#include <HalClock.h>
#include <I18n.h>

#include <cstdio>
#include <cstring>
#include <ctime>

#include "CrossPointSettings.h"
#include "components/icons/flame.h"
#include "components/icons/stats_icons.h"
#include "fontIds.h"

namespace StatsWidgets {

Today getToday() {
  // gmtime_r, not gmtime: the shared static buffer is unsafe with the render task also
  // converting time.
  const time_t now = HalClock::localEpoch(SETTINGS.clockUtcOffsetQ);
  struct tm t = {};
  gmtime_r(&now, &t);
  return {static_cast<uint16_t>(t.tm_year + 1900), static_cast<uint8_t>(t.tm_mon + 1), static_cast<uint8_t>(t.tm_mday),
          (t.tm_wday + 6) % 7};
}

int daysInMonth(const uint16_t y, const uint8_t m) {
  static constexpr int dm[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m == 2 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0))) return 29;
  return dm[m];
}

int firstDowOfMonth(const uint16_t y, const uint8_t m) {
  // Tomohiko Sakamoto's algorithm returns 0=Sun
  static constexpr int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  int yy = y;
  if (m < 3) yy--;
  const int dow = (yy + yy / 4 - yy / 100 + yy / 400 + t[m - 1] + 1) % 7;
  return (dow + 6) % 7;  // convert to ISO Mon=0
}

const char* monthName(const int month) {
  static constexpr StrId ids[] = {StrId::STR_MONTH_JAN, StrId::STR_MONTH_FEB, StrId::STR_MONTH_MAR,
                                  StrId::STR_MONTH_APR, StrId::STR_MONTH_MAY, StrId::STR_MONTH_JUN,
                                  StrId::STR_MONTH_JUL, StrId::STR_MONTH_AUG, StrId::STR_MONTH_SEP,
                                  StrId::STR_MONTH_OCT, StrId::STR_MONTH_NOV, StrId::STR_MONTH_DEC};
  return I18n::getInstance().get(ids[month - 1]);
}

const char* dayLabel(const int dow) {
  static constexpr StrId ids[] = {StrId::STR_DAY_MON, StrId::STR_DAY_TUE, StrId::STR_DAY_WED, StrId::STR_DAY_THU,
                                  StrId::STR_DAY_FRI, StrId::STR_DAY_SAT, StrId::STR_DAY_SUN};
  return I18n::getInstance().get(ids[dow]);
}

const char* monthAbbrev(const int month, char* out, const size_t outSize) {
  if (!out || outSize == 0) return "";  // outSize-1 below would wrap
  const char* full = monthName(month);
  // unsigned char*: `char` is signed here, and masking a sign-extended byte works only by luck.
  const auto* u = reinterpret_cast<const unsigned char*>(full);
  size_t bytes = 0, chars = 0;
  while (u[bytes] && chars < 3) {
    bytes++;  // then skip continuation bytes (10xxxxxx)
    while ((u[bytes] & 0xC0) == 0x80) bytes++;
    chars++;
  }
  if (bytes >= outSize) bytes = outSize - 1;
  memcpy(out, full, bytes);
  out[bytes] = '\0';
  return out;
}

void stepMonth(uint16_t& year, uint8_t& month, const int delta) {
  int m = static_cast<int>(month) + delta;
  while (m < 1) {
    m += 12;
    year--;
  }
  while (m > 12) {
    m -= 12;
    year++;
  }
  month = static_cast<uint8_t>(m);
}

int drawStreakCard(GfxRenderer& renderer, const int x, const int y, const int w, const int streak,
                   const uint16_t weekMinutes, const bool weekDays[7], const int todayDow) {
  const int iconSize = 32;
  const int smallLH = renderer.getLineHeight(SMALL_FONT_ID);
  const int circleSize = 24;
  const int streakH = CARD_PAD + iconSize + 4 + smallLH + 16 + smallLH + 8 + circleSize + CARD_PAD;

  renderer.drawRoundedRect(x, y, w, streakH, 2, CARD_RADIUS, true);

  char streakBuf[32];
  snprintf(streakBuf, sizeof(streakBuf), tr(STR_STREAK_FORMAT), streak);
  const int streakTextW = renderer.getTextWidth(UI_12_FONT_ID, streakBuf, EpdFontFamily::BOLD);
  const int row1TotalW = iconSize + 8 + streakTextW;
  const int row1X = x + (w - row1TotalW) / 2;
  const int row1Y = y + CARD_PAD;
  renderer.drawIcon(FlameIcon, row1X, row1Y, iconSize);
  renderer.drawText(UI_12_FONT_ID, row1X + iconSize + 8, row1Y + (iconSize - renderer.getLineHeight(UI_12_FONT_ID)) / 2,
                    streakBuf, true, EpdFontFamily::BOLD);

  char weekBuf[48];
  snprintf(weekBuf, sizeof(weekBuf), tr(STR_WEEK_MINUTES_READ_FORMAT), weekMinutes,
           weekMinutes == 1 ? tr(STR_MINUTE) : tr(STR_MINUTES));
  const int weekTextW = renderer.getTextWidth(SMALL_FONT_ID, weekBuf);
  const int row2Y = row1Y + iconSize + 4;
  renderer.drawText(SMALL_FONT_ID, x + (w - weekTextW) / 2, row2Y, weekBuf, true);

  const int sepY = row2Y + smallLH + 8;
  renderer.drawLine(x + CARD_PAD, sepY, x + w - CARD_PAD, sepY, true);

  const int daySpacing = (w - 2 * CARD_PAD) / 7;
  const int labelsY = sepY + 12;
  const int circlesY = labelsY + smallLH + 6;
  for (int i = 0; i < 7; i++) {
    const int cx = x + CARD_PAD + daySpacing / 2 + i * daySpacing;
    const bool isToday = (i == todayDow);
    const char* label = dayLabel(i);
    const int labelW =
        renderer.getTextWidth(SMALL_FONT_ID, label, isToday ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    renderer.drawText(SMALL_FONT_ID, cx - labelW / 2, labelsY, label, true,
                      isToday ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    renderer.drawIcon(weekDays[i] ? CircleCheckIcon : CircleEmptyIcon, cx - circleSize / 2, circlesY, circleSize);
  }
  return streakH;
}

int drawTileGrid(GfxRenderer& renderer, const int x, const int y, const int w, const Tile tiles[4]) {
  const int halfW = (w - CARD_GAP) / 2;
  const int iconSm = 24;

  for (int i = 0; i < 4; i++) {
    const int col = i % 2;
    const int row = i / 2;
    const int cx = x + col * (halfW + CARD_GAP);
    const int cy = y + row * (TILE_HEIGHT + CARD_GAP);

    renderer.drawRoundedRect(cx, cy, halfW, TILE_HEIGHT, 2, CARD_RADIUS, true);

    const int topPad = 16;
    if (tiles[i].bigIcon) {
      renderer.drawIcon(tiles[i].icon, cx + halfW - 32 - topPad + 4, cy + topPad - 2, 32);
    } else {
      renderer.drawIcon(tiles[i].icon, cx + halfW - iconSm - topPad, cy + topPad, iconSm);
    }

    const int valueY = cy + topPad + (iconSm - renderer.getLineHeight(UI_12_FONT_ID)) / 2;
    renderer.drawText(UI_12_FONT_ID, cx + 10, valueY, tiles[i].value, true, EpdFontFamily::BOLD);
    renderer.drawText(SMALL_FONT_ID, cx + 10, valueY + renderer.getLineHeight(UI_12_FONT_ID) + 2, tiles[i].label, true);
  }

  return 2 * (TILE_HEIGHT + CARD_GAP);
}

int drawMonthCalendar(GfxRenderer& renderer, const int x, const int y, const int w, const uint16_t calYear,
                      const uint8_t calMonth, const Today& today, const MonthSource& source) {
  const int smallLH = renderer.getLineHeight(SMALL_FONT_ID);
  const int dim = daysInMonth(calYear, calMonth);
  const int firstDow = firstDowOfMonth(calYear, calMonth);
  const int calRows = (firstDow + dim + 6) / 7;
  const int cellSize = (w - 2 * CARD_PAD) / 7;
  const int calTitleH = renderer.getLineHeight(UI_12_FONT_ID);
  const int calSubH = smallLH;
  const int calRowH = cellSize - 2;
  const int calH = 12 + calTitleH + calSubH + 12 + smallLH + calRows * calRowH + 10;

  renderer.drawRoundedRect(x, y, w, calH, 2, CARD_RADIUS, true);

  // Month/year header with chevrons
  char monthBuf[32];
  snprintf(monthBuf, sizeof(monthBuf), "%s %d", monthName(calMonth), calYear);
  const int monthW = renderer.getTextWidth(UI_12_FONT_ID, monthBuf, EpdFontFamily::BOLD);
  const int monthX = x + (w - monthW) / 2;
  const int monthY = y + 12;
  renderer.drawText(UI_12_FONT_ID, monthX, monthY, monthBuf, true, EpdFontFamily::BOLD);

  // Drawn twice, a pixel apart, to thicken a 1px line without a stroke-width API.
  const int chevCenterY = monthY + (calTitleH + calSubH) / 2;
  const int chevSz = 6;
  renderer.drawLine(x + CARD_PAD + chevSz, chevCenterY - chevSz, x + CARD_PAD, chevCenterY, true);
  renderer.drawLine(x + CARD_PAD, chevCenterY, x + CARD_PAD + chevSz, chevCenterY + chevSz, true);
  renderer.drawLine(x + CARD_PAD + chevSz + 1, chevCenterY - chevSz, x + CARD_PAD + 1, chevCenterY, true);
  renderer.drawLine(x + CARD_PAD + 1, chevCenterY, x + CARD_PAD + chevSz + 1, chevCenterY + chevSz, true);
  const int rChevX = x + w - CARD_PAD - chevSz;
  renderer.drawLine(rChevX, chevCenterY - chevSz, rChevX + chevSz, chevCenterY, true);
  renderer.drawLine(rChevX + chevSz, chevCenterY, rChevX, chevCenterY + chevSz, true);
  renderer.drawLine(rChevX - 1, chevCenterY - chevSz, rChevX + chevSz - 1, chevCenterY, true);
  renderer.drawLine(rChevX + chevSz - 1, chevCenterY, rChevX - 1, chevCenterY + chevSz, true);

  // Days read count
  const int daysReadMonth = source.daysReadInMonth(source.ctx, calYear, calMonth);
  char daysReadBuf[32];
  snprintf(daysReadBuf, sizeof(daysReadBuf), tr(STR_DAYS_READ_IN_MONTH_FORMAT), daysReadMonth);
  const int drW = renderer.getTextWidth(SMALL_FONT_ID, daysReadBuf);
  renderer.drawText(SMALL_FONT_ID, x + (w - drW) / 2, monthY + calTitleH + 2, daysReadBuf, true);

  // Day-of-week headers
  const int headerY = monthY + calTitleH + calSubH + 12;
  for (int i = 0; i < 7; i++) {
    const int cx = x + CARD_PAD + i * cellSize + cellSize / 2;
    const char* label = dayLabel(i);
    const int lw = renderer.getTextWidth(SMALL_FONT_ID, label);
    renderer.drawText(SMALL_FONT_ID, cx - lw / 2, headerY, label, true);
  }

  // Calendar grid
  bool monthStatus[32] = {};
  source.monthStatus(source.ctx, calYear, calMonth, monthStatus);
  const int gridY = headerY + smallLH;
  const int circR = (cellSize - 6) / 2;

  for (int d = 1; d <= dim; d++) {
    const int pos = firstDow + d - 1;
    const int col = pos % 7;
    const int row = pos / 7;
    const int cx = x + CARD_PAD + col * cellSize + cellSize / 2;
    const int cy = gridY + row * calRowH + calRowH / 2;

    char dayBuf[4];
    snprintf(dayBuf, sizeof(dayBuf), "%d", d);
    const int dw = renderer.getTextWidth(SMALL_FONT_ID, dayBuf);
    const int dy = cy - smallLH / 2;

    const bool isToday = (calYear == today.year && calMonth == today.month && d == today.day);

    if (monthStatus[d]) {
      renderer.fillRoundedRect(cx - circR, cy - circR, circR * 2, circR * 2, circR, Color::Black);
      renderer.drawText(SMALL_FONT_ID, cx - dw / 2, dy, dayBuf, false, EpdFontFamily::BOLD);
    } else if (isToday) {
      renderer.drawRoundedRect(cx - circR, cy - circR, circR * 2, circR * 2, 1, circR, true);
      renderer.drawText(SMALL_FONT_ID, cx - dw / 2, dy, dayBuf, true, EpdFontFamily::BOLD);
    } else {
      renderer.drawText(SMALL_FONT_ID, cx - dw / 2, dy, dayBuf, true);
    }
  }

  return calH;
}

}  // namespace StatsWidgets
