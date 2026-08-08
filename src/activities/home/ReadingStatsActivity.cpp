#include "ReadingStatsActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>

#include <cmath>
#include <cstdio>
#include <ctime>
#include <memory>

#include "CrossPointSettings.h"
#include "LanguageStatsActivity.h"
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

// Adapters letting StatsWidgets read the global store without knowing its type.
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
  // Tap leaves Insights, hold goes home; same gesture as the language screen.
  if (backLongPressFired) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Back)) backLongPressFired = false;
    return;
  }
  if (mappedInput.isPressed(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() >= StatsWidgets::HOME_HOLD_MS) {
    backLongPressFired = true;
    onGoHome();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < StatsWidgets::HOME_HOLD_MS) {
    finish();
    return;
  }
  // startActivityForResult, not replace, so this screen keeps its scroll and month.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    startActivityForResult(std::make_unique<LanguageStatsActivity>(renderer, mappedInput),
                           [](const ActivityResult&) {});
    return;
  }
  // Left/Right to navigate calendar months
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    StatsWidgets::stepMonth(calYear, calMonth, -1);
    requestUpdate();
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    StatsWidgets::stepMonth(calYear, calMonth, +1);
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
  y += StatsWidgets::drawStreakCard(renderer, cardX, y, cardW, streak, weekMinutes, weekDays, today.dow) + 16;

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
  // Hints name the months Left/Right land on.
  char prevBuf[16], nextBuf[16];
  uint16_t py = calYear, ny = calYear;
  uint8_t pm = calMonth, nm = calMonth;
  StatsWidgets::stepMonth(py, pm, -1);
  StatsWidgets::stepMonth(ny, nm, +1);
  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), tr(STR_DETAILS), StatsWidgets::monthAbbrev(pm, prevBuf, sizeof(prevBuf)),
                            StatsWidgets::monthAbbrev(nm, nextBuf, sizeof(nextBuf)));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
