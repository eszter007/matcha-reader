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
#include "components/StatsWidgets.h"
#include "components/UITheme.h"
#include "components/icons/flame.h"
#include "components/icons/stats_icons.h"
#include "fontIds.h"

namespace {
using StatsWidgets::dayLabel;
using StatsWidgets::getToday;
using Today = StatsWidgets::Today;

// Adapters that let StatsWidgets read the global store without knowing the type. The per-book
// screen supplies the same pair over a BookStats.
void overallMonthStatus(const void*, const uint16_t year, const uint8_t month, bool out[32]) {
  READING_STATS_STORE.getMonthStatus(year, month, out);
}
int overallDaysReadInMonth(const void*, const uint16_t year, const uint8_t month) {
  return READING_STATS_STORE.getDaysReadInMonth(year, month);
}
}  // namespace

void ReadingStatsActivity::onEnter() {
  Activity::onEnter();
  READING_STATS_STORE.loadFromFile();
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
  const int streak = READING_STATS_STORE.getStreak(today.year, today.month, today.day);
  const uint16_t weekMinutes = READING_STATS_STORE.getMinutesThisWeek(today.year, today.month, today.day);
  bool weekDays[7] = {};
  READING_STATS_STORE.getWeekStatus(today.year, today.month, today.day, today.dow, weekDays);

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
  renderer.drawIcon(FlameIcon, row1X, row1Y, iconSize);
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
      renderer.drawIcon(CircleCheckIcon, ix, circlesY, circleSize);
    } else {
      renderer.drawIcon(CircleEmptyIcon, ix, circlesY, circleSize);
    }
  }

  y += streakH + 16;

  // ==================== 4 STAT CARDS (2x2) ====================
  const int booksFinished = READING_STATS_STORE.getBooksFinished();
  const int daysRead = READING_STATS_STORE.getDaysRead();
  const uint32_t totalMin = READING_STATS_STORE.getTotalMinutes();
  const int longestStreak = READING_STATS_STORE.getLongestStreak();

  char booksBuf[16], daysBuf[16], timeBuf[16], streakLBuf[16];
  snprintf(booksBuf, sizeof(booksBuf), "%d", booksFinished);
  snprintf(daysBuf, sizeof(daysBuf), "%d", daysRead);
  if (totalMin >= 60)
    snprintf(timeBuf, sizeof(timeBuf), "%dh", static_cast<int>(totalMin / 60));
  else
    snprintf(timeBuf, sizeof(timeBuf), "%dm", static_cast<int>(totalMin));
  snprintf(streakLBuf, sizeof(streakLBuf), "%d", longestStreak);

  const StatsWidgets::Tile tiles[4] = {
      {booksBuf, tr(STR_STAT_BOOKS_FINISHED), BookOpenIcon24, false},
      {daysBuf, tr(STR_STAT_DAYS_READ), CalendarIcon24, false},
      {timeBuf, tr(STR_STAT_TOTAL_TIME), ClockIcon24, false},
      {streakLBuf, tr(STR_STAT_LONGEST_STREAK), FlameIcon, true},
  };
  y += StatsWidgets::drawTileGrid(renderer, cardX, y, cardW, tiles) + 8;

  // ==================== CALENDAR ====================
  const StatsWidgets::MonthSource source{nullptr, overallMonthStatus, overallDaysReadInMonth};
  y += StatsWidgets::drawMonthCalendar(renderer, cardX, y, cardW, calYear, calMonth, today, source);

  // Compute max scroll: content bottom minus the visible area.
  const int contentEndY = y + 10;                                            // 10px bottom margin
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
