#include "ReadingStatsActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>

#include <algorithm>
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

// startActivityForResult, not replace, so this screen keeps its scroll and month.
void ReadingStatsActivity::openLanguageStats() {
  startActivityForResult(std::make_unique<LanguageStatsActivity>(renderer, mappedInput), [](const ActivityResult&) {});
}

// Shared by both stats screens in spirit, but each owns its own calendar rect.
bool ReadingStatsActivity::stepMonthFromTap() {
  if (monthNav.next.width == 0) return false;
  int tx = 0;
  int ty = 0;
  if (!mappedInput.wasScreenTapped(tx, ty)) return false;
  const auto hit = [&](const Rect& r) { return tx >= r.x && tx < r.x + r.width && ty >= r.y && ty < r.y + r.height; };
  if (hit(monthNav.prev)) {
    StatsWidgets::stepMonth(calYear, calMonth, -1);
  } else if (hit(monthNav.next)) {
    StatsWidgets::stepMonth(calYear, calMonth, +1);
  } else {
    return false;
  }
  requestUpdate();
  return true;
}

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
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    openLanguageStats();
    return;
  }
  // Same destination for the Details button. Touch boards have no front buttons,
  // so the Confirm above reaches them only through the power click, and only when
  // the user has bound it to Confirm -- which is not the default. Without this the
  // language screen was effectively unreachable on an X4 Pro.
  if (detailsButton.width > 0) {
    int tx = 0;
    int ty = 0;
    if (mappedInput.wasScreenTapped(tx, ty) && tx >= detailsButton.x && tx < detailsButton.x + detailsButton.width &&
        ty >= detailsButton.y && ty < detailsButton.y + detailsButton.height) {
      openLanguageStats();
      return;
    }
  }
  // Tapping a chevron steps the month, the touch equivalent of the Left/Right keys
  // below -- which resolve to front buttons a touch board does not have.
  if (stepMonthFromTap()) return;
  // Left/Right to navigate calendar months
  if (mappedInput.wasReleased(MappedInputManager::Button::ScreenLeft)) {
    StatsWidgets::stepMonth(calYear, calMonth, -1);
    requestUpdate();
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::ScreenRight)) {
    StatsWidgets::stepMonth(calYear, calMonth, +1);
    requestUpdate();
  }
  // Swipe to scroll, a page at a time — the same gesture and direction the
  // lists use (swipe up to go forward). The keys below stay the fine control.
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    const int delta = swipe == MappedInputManager::SwipeDir::Up ? scrollPageHeight : -scrollPageHeight;
    const int target = std::clamp(scrollOffset + delta, 0, maxScrollOffset);
    if (target != scrollOffset) {
      scrollOffset = target;
      requestUpdate();
    }
    return;
  }

  // Up/Down to scroll
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::ScreenDown}, [this] {
    if (scrollOffset < maxScrollOffset) {
      scrollOffset += 40;
      if (scrollOffset > maxScrollOffset) scrollOffset = maxScrollOffset;
      requestUpdate();
    }
  });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::ScreenUp}, [this] {
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
  y += StatsWidgets::drawMonthCalendar(renderer, cardX, y, cardW, calYear, calMonth, today, source, &monthNav);

  // ==================== DETAILS BUTTON ====================
  // Touch boards only. They have no front buttons, so the Confirm the hints row
  // names is a key that does not exist there -- and drawButtonHints() draws nothing
  // on touch anyway, so the hint itself never appears. Button boards keep Confirm
  // and would only get a duplicate control. Drawn inside the scrolled content, so
  // it sits under the calendar rather than floating over it.
  detailsButton = Rect{};
  if (mappedInput.hasTouch()) {
    constexpr int buttonHeight = 48;
    constexpr int buttonGap = 12;
    detailsButton = Rect{cardX, y + buttonGap, cardW, buttonHeight};
    renderer.drawRoundedRect(detailsButton.x, detailsButton.y, detailsButton.width, detailsButton.height, 2,
                             StatsWidgets::CARD_RADIUS, true);
    const char* label = tr(STR_VIEW_DETAILS);
    const int labelWidth = renderer.getTextWidth(UI_12_FONT_ID, label);
    const int labelHeight = renderer.getLineHeight(UI_12_FONT_ID);
    renderer.drawText(UI_12_FONT_ID, detailsButton.x + (detailsButton.width - labelWidth) / 2,
                      detailsButton.y + (detailsButton.height - labelHeight) / 2, label, true);
    y += buttonGap + buttonHeight;
  }

  // Compute max scroll: content bottom minus the visible area.
  const int contentEndY = y + 10;                                            // 10px bottom margin
  const int visibleHeight = renderer.getScreenHeight() - headerBottom - 50;  // 50 for button hints
  maxScrollOffset = contentEndY - headerBottom - visibleHeight + scrollOffset;
  if (maxScrollOffset < 0) maxScrollOffset = 0;
  scrollPageHeight = visibleHeight;

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
