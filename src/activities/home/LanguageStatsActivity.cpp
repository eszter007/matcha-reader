#include "LanguageStatsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <vector>

#include "MappedInputManager.h"
#include "components/StatsWidgets.h"
#include "components/UITheme.h"
#include "components/icons/flame.h"
#include "components/icons/stats_icons.h"
#include "fontIds.h"

namespace {
// The code the calendar adapters below read. A file-local pointer rather than a member because
// MonthSource carries one void* and the calendar needs both the store and the language; the
// store is a singleton, so the language is the only thing left to pass.
const char* g_calendarLanguage = "";

void languageMonthStatus(const void*, const uint16_t year, const uint8_t month, bool out[32]) {
  READING_STATS_STORE.getMonthStatus(g_calendarLanguage, year, month, out);
}
int languageDaysReadInMonth(const void*, const uint16_t year, const uint8_t month) {
  return READING_STATS_STORE.getDaysReadInMonth(g_calendarLanguage, year, month);
}
}  // namespace

const char* LanguageStatsActivity::selectedCode() const {
  if (languages.empty() || selectedTab < 0 || selectedTab >= static_cast<int>(languages.size())) return "";
  return languages[selectedTab].code;
}

const char* LanguageStatsActivity::tabLabel(const int index) const {
  if (index < 0 || index >= static_cast<int>(languages.size())) return "";
  const char* code = languages[index].code;
  if (!code[0]) return tr(STR_LANGUAGE_UNKNOWN);

  // The endonym the UI-language list already carries ("ja" -> the Japanese name for Japanese).
  // Free: those strings are in flash whether this screen exists or not, so naming the tabs costs
  // no table of its own. Endonyms rather than names translated into the current UI language --
  // the same convention every language picker uses, and the only one the shipped data supports.
  if (const char* name = I18n::languageNameForCode(code)) return name;

  // A book tagged with a language the firmware has no UI for (say "zh"). Fall back to the tag
  // uppercased rather than mislabelling it: a wrong language name is worse than a bare code.
  static char bufs[4][4];
  static int next = 0;
  char* out = bufs[next];
  next = (next + 1) % 4;
  for (int i = 0; i < 3; i++) {
    const char c = code[i];
    out[i] = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
  }
  out[3] = '\0';
  return out;
}

void LanguageStatsActivity::onEnter() {
  Activity::onEnter();
  READING_STATS_STORE.getLanguages(languages);
  const StatsWidgets::Today today = StatsWidgets::getToday();
  calYear = today.year;
  calMonth = today.month;
  requestUpdate();
}

void LanguageStatsActivity::onExit() { Activity::onExit(); }

void LanguageStatsActivity::loop() {
  // Back: a tap steps back to the overall screen, a hold goes straight home. The latch stops
  // the release that ends the hold from also firing the tap.
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
  // Confirm cycles languages. Left/Right stay on the calendar month, matching the overall
  // screen -- a reader who has learned those there should not have to relearn them here.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && languages.size() > 1) {
    selectedTab = (selectedTab + 1) % static_cast<int>(languages.size());
    scrollOffset = 0;
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    StatsWidgets::stepMonth(calYear, calMonth, -1);
    requestUpdate();
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    StatsWidgets::stepMonth(calYear, calMonth, +1);
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

void LanguageStatsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto& theme = UITheme::getInstance();
  auto metrics = theme.getMetrics();
  Rect screen = theme.getScreenSafeArea(renderer, true, false);

  const int headerLineY = screen.y + metrics.topPadding + metrics.headerHeight - 3;
  // Content starts below the tab bar, which sits in the fixed band under the header exactly as
  // it does in Library and Settings.
  const int tabBarY = screen.y + metrics.topPadding + metrics.headerHeight;
  const int headerBottom = tabBarY + metrics.tabBarHeight + metrics.verticalSpacing;
  const int contentTop = headerBottom - scrollOffset;

  const int cardMargin = 20;
  const int cardX = screen.x + cardMargin;
  const int cardW = screen.width - 2 * cardMargin;

  const StatsWidgets::Today today = StatsWidgets::getToday();
  int y = contentTop + 8;

  if (languages.empty()) {
    // Nothing has been read with a language recorded yet. Distinct from "no reading at all":
    // TXT and XTC declare none, so a reader can have plenty of overall history and still land
    // here with nothing to split.
    const char* msg = tr(STR_NO_LANGUAGE_STATS_YET);
    const int mw = renderer.getTextWidth(SMALL_FONT_ID, msg);
    renderer.drawText(SMALL_FONT_ID, cardX + (cardW - mw) / 2, y + 40, msg, true);
    maxScrollOffset = 0;
  } else {
    const char* code = selectedCode();
    g_calendarLanguage = code;

    // ---- same three cards as the overall screen, filtered to this language ----
    const int streak = READING_STATS_STORE.getStreak(code, today.year, today.month, today.day);
    const uint16_t weekMinutes = READING_STATS_STORE.getMinutesThisWeek(code, today.year, today.month, today.day);
    bool weekDays[7] = {};
    READING_STATS_STORE.getWeekStatus(code, today.year, today.month, today.day, today.dow, weekDays);
    y += StatsWidgets::drawStreakCard(renderer, cardX, y, cardW, streak, weekMinutes, weekDays, today.dow) + 16;

    const uint32_t totalMin = READING_STATS_STORE.getTotalMinutes(code);
    char booksBuf[16], daysBuf[16], timeBuf[16], streakLBuf[16];
    snprintf(booksBuf, sizeof(booksBuf), "%u", READING_STATS_STORE.getBooksFinished(code));
    snprintf(daysBuf, sizeof(daysBuf), "%d", READING_STATS_STORE.getDaysRead(code));
    if (totalMin >= 60) {
      snprintf(timeBuf, sizeof(timeBuf), "%dh", static_cast<int>(totalMin / 60));
    } else {
      snprintf(timeBuf, sizeof(timeBuf), "%dm", static_cast<int>(totalMin));
    }
    snprintf(streakLBuf, sizeof(streakLBuf), "%d", READING_STATS_STORE.getLongestStreak(code));

    const StatsWidgets::Tile tiles[4] = {
        {booksBuf, tr(STR_STAT_BOOKS_FINISHED), BookOpenIcon24, false},
        {daysBuf, tr(STR_STAT_DAYS_READ), CalendarIcon24, false},
        {timeBuf, tr(STR_STAT_TOTAL_TIME), ClockIcon24, false},
        {streakLBuf, tr(STR_STAT_LONGEST_STREAK), FlameIcon, true},
    };
    y += StatsWidgets::drawTileGrid(renderer, cardX, y, cardW, tiles) + 8;

    const StatsWidgets::MonthSource source{nullptr, languageMonthStatus, languageDaysReadInMonth};
    y += StatsWidgets::drawMonthCalendar(renderer, cardX, y, cardW, calYear, calMonth, today, source);

    const int contentEndY = y + 10;
    const int visibleHeight = renderer.getScreenHeight() - headerBottom - 50;
    maxScrollOffset = std::max(0, contentEndY - headerBottom - visibleHeight + scrollOffset);
  }

  // Header and tab bar last, over the scrolled content, so nothing bleeds through above them.
  renderer.fillRect(0, 0, screen.width, headerBottom - metrics.verticalSpacing, false);
  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_LANGUAGE));
  if (!languages.empty()) {
    std::vector<TabInfo> tabs;
    tabs.reserve(languages.size());
    for (int i = 0; i < static_cast<int>(languages.size()); i++) {
      tabs.push_back({tabLabel(i), i == selectedTab});
    }
    // The same component Library and Settings draw, so a language tab looks like every other tab
    // in the firmware. Always drawn selected: the tab row is the only thing Confirm acts on here.
    GUI.drawTabBar(renderer, Rect{0, tabBarY, screen.width, metrics.tabBarHeight}, tabs, true);
  }

  // Left/Right are month steps, so the hints name the months they land on rather than saying
  // "left"/"right" -- three characters is all the hint strip has room for.
  char prevBuf[16], nextBuf[16];
  uint16_t py = calYear, ny = calYear;
  uint8_t pm = calMonth, nm = calMonth;
  StatsWidgets::stepMonth(py, pm, -1);
  StatsWidgets::stepMonth(ny, nm, +1);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), languages.size() > 1 ? tr(STR_SWITCH) : "",
                                            StatsWidgets::monthAbbrev(pm, prevBuf, sizeof(prevBuf)),
                                            StatsWidgets::monthAbbrev(nm, nextBuf, sizeof(nextBuf)));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
