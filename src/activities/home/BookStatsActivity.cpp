#include "BookStatsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "components/StatsWidgets.h"
#include "components/UITheme.h"
#include "components/icons/stats_icons.h"
#include "fontIds.h"

namespace {
// Adapters that let StatsWidgets read a BookStats without knowing the type.
void bookMonthStatus(const void* ctx, const uint16_t year, const uint8_t month, bool out[32]) {
  static_cast<const BookStats*>(ctx)->getMonthStatus(year, month, out);
}
int bookDaysReadInMonth(const void* ctx, const uint16_t year, const uint8_t month) {
  return static_cast<const BookStats*>(ctx)->getDaysReadInMonth(year, month);
}

// "3h" past an hour, "45m" below it -- the same rule the Insights total-time tile uses, so the
// two screens read alike.
void formatMinutes(char* buf, const size_t size, const uint32_t minutes) {
  if (minutes >= 60) {
    snprintf(buf, size, "%dh", static_cast<int>(minutes / 60));
  } else {
    snprintf(buf, size, "%dm", static_cast<int>(minutes));
  }
}
}  // namespace

void BookStatsActivity::onEnter() {
  Activity::onEnter();
  stats.load(bookPath.c_str());
  const StatsWidgets::Today today = StatsWidgets::getToday();
  calYear = today.year;
  calMonth = today.month;
  requestUpdate();
}

void BookStatsActivity::onExit() { Activity::onExit(); }

void BookStatsActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    if (calMonth == 1) {
      calMonth = 12;
      calYear--;
    } else {
      calMonth--;
    }
    requestUpdate();
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    if (calMonth == 12) {
      calMonth = 1;
      calYear++;
    } else {
      calMonth++;
    }
    requestUpdate();
  }
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down}, [this] {
    if (scrollOffset < maxScrollOffset) {
      scrollOffset = std::min(scrollOffset + 40, maxScrollOffset);
      requestUpdate();
    }
  });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up}, [this] {
    if (scrollOffset > 0) {
      scrollOffset = std::max(scrollOffset - 40, 0);
      requestUpdate();
    }
  });
}

void BookStatsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto& theme = UITheme::getInstance();
  auto metrics = theme.getMetrics();
  Rect screen = theme.getScreenSafeArea(renderer, true, false);

  const int headerLineY = screen.y + metrics.topPadding + metrics.headerHeight - 3;
  const int headerBottom = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentTop = headerBottom - scrollOffset;

  const int cardMargin = 20;
  const int cardX = screen.x + cardMargin;
  const int cardW = screen.width - 2 * cardMargin;

  const StatsWidgets::Today today = StatsWidgets::getToday();
  int y = contentTop + 8;

  // A book that has not been read since this feature shipped has no history at all -- say so
  // rather than showing four zeroes and an empty calendar, which reads like a bug. History
  // starts accumulating the next time the book is opened.
  if (stats.getSessions() == 0 && stats.getDaysRead() == 0) {
    const char* msg = tr(STR_NO_READING_STATS_YET);
    const int mw = renderer.getTextWidth(SMALL_FONT_ID, msg);
    renderer.drawText(SMALL_FONT_ID, cardX + (cardW - mw) / 2, y + 40, msg, true);
    maxScrollOffset = 0;
  } else {
    char sessionsBuf[16], timeBuf[16], avgBuf[16], daysBuf[16];
    snprintf(sessionsBuf, sizeof(sessionsBuf), "%lu", static_cast<unsigned long>(stats.getSessions()));
    formatMinutes(timeBuf, sizeof(timeBuf), stats.getTotalMinutes());
    formatMinutes(avgBuf, sizeof(avgBuf), stats.getAverageSessionMinutes());
    snprintf(daysBuf, sizeof(daysBuf), "%d", stats.getDaysRead());

    const StatsWidgets::Tile tiles[4] = {
        {sessionsBuf, tr(STR_STAT_SESSIONS), BookOpenIcon24, false},
        {timeBuf, tr(STR_STAT_TOTAL_TIME), ClockIcon24, false},
        {avgBuf, tr(STR_STAT_AVG_SESSION), ChartBarIcon, true},
        {daysBuf, tr(STR_STAT_DAYS_READ), CalendarIcon24, false},
    };
    y += StatsWidgets::drawTileGrid(renderer, cardX, y, cardW, tiles) + 8;

    const StatsWidgets::MonthSource source{&stats, bookMonthStatus, bookDaysReadInMonth};
    y += StatsWidgets::drawMonthCalendar(renderer, cardX, y, cardW, calYear, calMonth, today, source);

    // Same scroll accounting as the Insights screen: content bottom minus the visible band,
    // with room left for the button hints.
    const int contentEndY = y + 10;
    const int visibleHeight = renderer.getScreenHeight() - headerBottom - 50;
    maxScrollOffset = std::max(0, contentEndY - headerBottom - visibleHeight + scrollOffset);
  }

  // Header last, over the scrolled content, so text cannot bleed through above the rule.
  renderer.fillRect(0, 0, screen.width, headerLineY, false);
  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 bookTitle.c_str());

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
