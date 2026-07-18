#include "ReadingStatsActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>

#include <cmath>
#include <cstdio>
#include <ctime>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ReadingStatsStore.h"
#include "components/UITheme.h"
#include "components/icons/flame.h"
#include "components/icons/stats_icons.h"
#include "fontIds.h"

namespace {
struct Today {
  uint16_t year;
  uint8_t month, day;
  int dow;  // 0=Mon..6=Sun (ISO)
};

Today getToday() {
  // Local-midnight day boundary, matching how the readers record stats.
  time_t now = HalClock::localEpoch(SETTINGS.clockUtcOffsetQ);
  const struct tm* t = gmtime(&now);
  return {static_cast<uint16_t>(t->tm_year + 1900), static_cast<uint8_t>(t->tm_mon + 1),
          static_cast<uint8_t>(t->tm_mday), (t->tm_wday + 6) % 7};
}

int daysInMonth(uint16_t y, uint8_t m) {
  static constexpr int dm[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m == 2 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0))) return 29;
  return dm[m];
}

// Day of week for the 1st of a month (0=Mon..6=Sun ISO)
int firstDowOfMonth(uint16_t y, uint8_t m) {
  // Tomohiko Sakamoto's algorithm returns 0=Sun
  static constexpr int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  int yy = y;
  if (m < 3) yy--;
  int dow = (yy + yy / 4 - yy / 100 + yy / 400 + t[m - 1] + 1) % 7;
  return (dow + 6) % 7;  // convert to ISO Mon=0
}

// tr() reads from the I18N singleton, so these must be resolved at call time
// (not static-initialized at namespace scope, before I18N is guaranteed ready).
const char* monthName(int month) {
  static constexpr StrId ids[] = {StrId::STR_MONTH_JAN, StrId::STR_MONTH_FEB, StrId::STR_MONTH_MAR,
                                  StrId::STR_MONTH_APR, StrId::STR_MONTH_MAY, StrId::STR_MONTH_JUN,
                                  StrId::STR_MONTH_JUL, StrId::STR_MONTH_AUG, StrId::STR_MONTH_SEP,
                                  StrId::STR_MONTH_OCT, StrId::STR_MONTH_NOV, StrId::STR_MONTH_DEC};
  return I18n::getInstance().get(ids[month - 1]);
}

const char* dayLabel(int dow) {
  static constexpr StrId ids[] = {StrId::STR_DAY_MON, StrId::STR_DAY_TUE, StrId::STR_DAY_WED, StrId::STR_DAY_THU,
                                  StrId::STR_DAY_FRI, StrId::STR_DAY_SAT, StrId::STR_DAY_SUN};
  return I18n::getInstance().get(ids[dow]);
}
}  // namespace

void ReadingStatsActivity::onEnter() {
  Activity::onEnter();
  READING_STATS.loadFromFile();
  const Today today = getToday();
  calYear = today.year;
  calMonth = today.month;
  requestUpdate();
}

void ReadingStatsActivity::onExit() { Activity::onExit(); }

void ReadingStatsActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  // Left/Right to navigate calendar months
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    if (calMonth == 1) {
      calMonth = 12;
      calYear--;
    } else
      calMonth--;
    requestUpdate();
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    if (calMonth == 12) {
      calMonth = 1;
      calYear++;
    } else
      calMonth++;
    requestUpdate();
  }
  // Up/Down to scroll
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down}, [this] {
    if (scrollOffset < maxScrollOffset) {
      scrollOffset += 40;
      if (scrollOffset > maxScrollOffset) scrollOffset = maxScrollOffset;
      requestUpdate();
    }
  });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up}, [this] {
    if (scrollOffset > 0) {
      scrollOffset -= 40;
      if (scrollOffset < 0) scrollOffset = 0;
      requestUpdate();
    }
  });
}

void ReadingStatsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto& theme = UITheme::getInstance();
  auto metrics = theme.getMetrics();
  Rect screen = theme.getScreenSafeArea(renderer, true, false);

  // The header's underline sits a few px above the header rect's bottom edge.
  const int headerLineY = screen.y + metrics.topPadding + metrics.headerHeight - 3;
  const int headerBottom = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentTop = headerBottom - scrollOffset;

  // Draw header AFTER content so it covers scrolled text underneath.
  // Content is drawn first, then the header area is cleared and redrawn on top.
  const int cardMargin = 20;
  const int cardX = screen.x + cardMargin;
  const int cardW = screen.width - 2 * cardMargin;
  const int cardPad = 16;
  const int cardRadius = 12;

  const Today today = getToday();
  const int streak = READING_STATS.getStreak(today.year, today.month, today.day);
  const uint16_t weekMinutes = READING_STATS.getMinutesThisWeek(today.year, today.month, today.day);
  bool weekDays[7] = {};
  READING_STATS.getWeekStatus(today.year, today.month, today.day, today.dow, weekDays);

  int y = contentTop + 8;

  // ==================== STREAK WIDGET ====================
  const int iconSize = 32;
  const int smallLH = renderer.getLineHeight(SMALL_FONT_ID);
  const int circleSize = 24;
  const int streakH = cardPad + iconSize + 4 + smallLH + 16 + smallLH + 8 + circleSize + cardPad;

  renderer.drawRoundedRect(cardX, y, cardW, streakH, 2, cardRadius, true);

  // Flame + streak
  char streakBuf[32];
  snprintf(streakBuf, sizeof(streakBuf), tr(STR_STREAK_FORMAT), streak);
  const int streakTextW = renderer.getTextWidth(UI_12_FONT_ID, streakBuf, EpdFontFamily::BOLD);
  const int row1TotalW = iconSize + 8 + streakTextW;
  const int row1X = cardX + (cardW - row1TotalW) / 2;
  const int row1Y = y + cardPad;
  renderer.drawIcon(FlameIcon, row1X, row1Y, iconSize, iconSize);
  renderer.drawText(UI_12_FONT_ID, row1X + iconSize + 8, row1Y + (iconSize - renderer.getLineHeight(UI_12_FONT_ID)) / 2,
                    streakBuf, true, EpdFontFamily::BOLD);

  // Minutes this week
  char weekBuf[48];
  snprintf(weekBuf, sizeof(weekBuf), tr(STR_WEEK_MINUTES_READ_FORMAT), weekMinutes,
           weekMinutes == 1 ? tr(STR_MINUTE) : tr(STR_MINUTES));
  const int weekTextW = renderer.getTextWidth(SMALL_FONT_ID, weekBuf);
  const int row2Y = row1Y + iconSize + 4;
  renderer.drawText(SMALL_FONT_ID, cardX + (cardW - weekTextW) / 2, row2Y, weekBuf, true);

  // Separator
  const int sepY = row2Y + smallLH + 8;
  renderer.drawLine(cardX + cardPad, sepY, cardX + cardW - cardPad, sepY, true);

  // Day labels + circles (Mon-Sun)
  const int daySpacing = (cardW - 2 * cardPad) / 7;
  const int labelsY = sepY + 12;
  const int circlesY = labelsY + smallLH + 6;

  for (int i = 0; i < 7; i++) {
    const int cx = cardX + cardPad + daySpacing / 2 + i * daySpacing;
    const bool isToday = (i == today.dow);
    const char* label = dayLabel(i);
    const int labelW =
        renderer.getTextWidth(SMALL_FONT_ID, label, isToday ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    renderer.drawText(SMALL_FONT_ID, cx - labelW / 2, labelsY, label, true,
                      isToday ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    const int ix = cx - circleSize / 2;
    if (weekDays[i]) {
      renderer.drawIcon(CircleCheckIcon, ix, circlesY, circleSize, circleSize);
    } else {
      renderer.drawIcon(CircleEmptyIcon, ix, circlesY, circleSize, circleSize);
    }
  }

  y += streakH + 16;

  // ==================== 4 STAT CARDS (2x2) ====================
  const int cardGap = 10;
  const int halfW = (cardW - cardGap) / 2;
  const int statCardH = 80;
  const int iconSm = 24;

  struct StatCard {
    const char* value;
    const char* label;
    const uint8_t* icon;
  };

  const int booksFinished = READING_STATS.getBooksFinished();
  const int daysRead = READING_STATS.getDaysRead();
  const uint32_t totalMin = READING_STATS.getTotalMinutes();
  const int longestStreak = READING_STATS.getLongestStreak();

  char booksBuf[16], daysBuf[16], timeBuf[16], streakLBuf[16];
  snprintf(booksBuf, sizeof(booksBuf), "%d", booksFinished);
  snprintf(daysBuf, sizeof(daysBuf), "%d", daysRead);
  if (totalMin >= 60)
    snprintf(timeBuf, sizeof(timeBuf), "%dh", static_cast<int>(totalMin / 60));
  else
    snprintf(timeBuf, sizeof(timeBuf), "%dm", static_cast<int>(totalMin));
  snprintf(streakLBuf, sizeof(streakLBuf), "%d", longestStreak);

  StatCard cards[4] = {
      {booksBuf, tr(STR_STAT_BOOKS_FINISHED), BookOpenIcon24},
      {daysBuf, tr(STR_STAT_DAYS_READ), CalendarIcon24},
      {timeBuf, tr(STR_STAT_TOTAL_TIME), ClockIcon24},
      {streakLBuf, tr(STR_STAT_LONGEST_STREAK), FlameIcon},
  };

  for (int i = 0; i < 4; i++) {
    const int col = i % 2;
    const int row = i / 2;
    const int cx = cardX + col * (halfW + cardGap);
    const int cy = y + row * (statCardH + cardGap);

    renderer.drawRoundedRect(cx, cy, halfW, statCardH, 2, cardRadius, true);

    const int topPad = 16;
    if (i < 3) {
      renderer.drawIcon(cards[i].icon, cx + halfW - iconSm - topPad, cy + topPad, iconSm, iconSm);
    } else {
      renderer.drawIcon(cards[i].icon, cx + halfW - 32 - topPad + 4, cy + topPad - 2, 32, 32);
    }

    const int valueY = cy + topPad + (iconSm - renderer.getLineHeight(UI_12_FONT_ID)) / 2;
    renderer.drawText(UI_12_FONT_ID, cx + 10, valueY, cards[i].value, true, EpdFontFamily::BOLD);
    renderer.drawText(SMALL_FONT_ID, cx + 10, valueY + renderer.getLineHeight(UI_12_FONT_ID) + 2, cards[i].label, true);
  }

  y += 2 * (statCardH + cardGap) + 8;

  // ==================== CALENDAR ====================
  const int dim = daysInMonth(calYear, calMonth);
  const int firstDow = firstDowOfMonth(calYear, calMonth);
  const int calRows = (firstDow + dim + 6) / 7;
  const int cellSize = (cardW - 2 * cardPad) / 7;
  const int calTitleH = renderer.getLineHeight(UI_12_FONT_ID);
  const int calSubH = smallLH;
  const int calRowH = cellSize - 2;
  const int calH = 12 + calTitleH + calSubH + 12 + smallLH + calRows * calRowH + 10;

  renderer.drawRoundedRect(cardX, y, cardW, calH, 2, cardRadius, true);

  // Month/year header with chevrons — same font as streak
  char monthBuf[32];
  snprintf(monthBuf, sizeof(monthBuf), "%s %d", monthName(calMonth), calYear);
  const int monthW = renderer.getTextWidth(UI_12_FONT_ID, monthBuf, EpdFontFamily::BOLD);
  const int monthX = cardX + (cardW - monthW) / 2;
  const int monthY = y + 12;
  renderer.drawText(UI_12_FONT_ID, monthX, monthY, monthBuf, true, EpdFontFamily::BOLD);

  // Chevrons centered vertically in the title+subtitle block (same style as shelves)
  const int chevCenterY = monthY + (calTitleH + calSubH) / 2;
  const int chevSz = 6;
  // Left < (doubled for thickness)
  renderer.drawLine(cardX + cardPad + chevSz, chevCenterY - chevSz, cardX + cardPad, chevCenterY, true);
  renderer.drawLine(cardX + cardPad, chevCenterY, cardX + cardPad + chevSz, chevCenterY + chevSz, true);
  renderer.drawLine(cardX + cardPad + chevSz + 1, chevCenterY - chevSz, cardX + cardPad + 1, chevCenterY, true);
  renderer.drawLine(cardX + cardPad + 1, chevCenterY, cardX + cardPad + chevSz + 1, chevCenterY + chevSz, true);
  // Right > (doubled for thickness)
  const int rChevX = cardX + cardW - cardPad - chevSz;
  renderer.drawLine(rChevX, chevCenterY - chevSz, rChevX + chevSz, chevCenterY, true);
  renderer.drawLine(rChevX + chevSz, chevCenterY, rChevX, chevCenterY + chevSz, true);
  renderer.drawLine(rChevX - 1, chevCenterY - chevSz, rChevX + chevSz - 1, chevCenterY, true);
  renderer.drawLine(rChevX + chevSz - 1, chevCenterY, rChevX - 1, chevCenterY + chevSz, true);

  // Days read count
  const int daysReadMonth = READING_STATS.getDaysReadInMonth(calYear, calMonth);
  char daysReadBuf[32];
  snprintf(daysReadBuf, sizeof(daysReadBuf), tr(STR_DAYS_READ_IN_MONTH_FORMAT), daysReadMonth);
  const int drW = renderer.getTextWidth(SMALL_FONT_ID, daysReadBuf);
  renderer.drawText(SMALL_FONT_ID, cardX + (cardW - drW) / 2, monthY + calTitleH + 2, daysReadBuf, true);

  // Day-of-week headers
  const int headerY = monthY + calTitleH + calSubH + 12;
  for (int i = 0; i < 7; i++) {
    const int cx = cardX + cardPad + i * cellSize + cellSize / 2;
    const char* label = dayLabel(i);
    const int lw = renderer.getTextWidth(SMALL_FONT_ID, label);
    renderer.drawText(SMALL_FONT_ID, cx - lw / 2, headerY, label, true);
  }

  // Calendar grid
  bool monthStatus[32] = {};
  READING_STATS.getMonthStatus(calYear, calMonth, monthStatus);
  const int gridY = headerY + smallLH;
  const int circR = (cellSize - 6) / 2;

  for (int d = 1; d <= dim; d++) {
    const int pos = firstDow + d - 1;
    const int col = pos % 7;
    const int row = pos / 7;
    const int cx = cardX + cardPad + col * cellSize + cellSize / 2;
    const int cy = gridY + row * calRowH + calRowH / 2;

    char dayBuf[4];
    snprintf(dayBuf, sizeof(dayBuf), "%d", d);
    const int dw = renderer.getTextWidth(SMALL_FONT_ID, dayBuf);
    const int dy = cy - smallLH / 2;

    const bool isToday = (calYear == today.year && calMonth == today.month && d == today.day);

    if (monthStatus[d]) {
      // Filled circle with white text
      renderer.fillRoundedRect(cx - circR, cy - circR, circR * 2, circR * 2, circR, Color::Black);
      renderer.drawText(SMALL_FONT_ID, cx - dw / 2, dy, dayBuf, false, EpdFontFamily::BOLD);
    } else if (isToday) {
      // Outline circle for today
      renderer.drawRoundedRect(cx - circR, cy - circR, circR * 2, circR * 2, 1, circR, true);
      renderer.drawText(SMALL_FONT_ID, cx - dw / 2, dy, dayBuf, true, EpdFontFamily::BOLD);
    } else {
      renderer.drawText(SMALL_FONT_ID, cx - dw / 2, dy, dayBuf, true);
    }
  }

  // Compute max scroll: content bottom (y + calH) minus the visible area.
  const int contentEndY = y + calH + 10;                                     // 10px bottom margin
  const int visibleHeight = renderer.getScreenHeight() - headerBottom - 50;  // 50 for button hints
  maxScrollOffset = contentEndY - headerBottom - visibleHeight + scrollOffset;
  if (maxScrollOffset < 0) maxScrollOffset = 0;

  // Redraw header on top of scrolled content so text doesn't bleed through.
  // Clear only up to the header line, then redraw the header (which draws the line).
  renderer.fillRect(0, 0, screen.width, headerLineY, false);
  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_STATS));

  // Button hints
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
