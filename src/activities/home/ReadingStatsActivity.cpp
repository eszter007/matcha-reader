#include "ReadingStatsActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "HapticFeedback.h"
#include "MappedInputManager.h"
#include "ReadingStatsStore.h"
#include "components/HomeTabBar.h"
#include "components/StatsWidgets.h"
#include "components/UITheme.h"
#include "components/icons/flame.h"
#include "components/icons/stats_icons.h"
#include "fontIds.h"

namespace {
using StatsWidgets::dayLabel;
using StatsWidgets::getToday;
using Today = StatsWidgets::Today;

// Adapters letting StatsWidgets read the global store without knowing its type. ctx is the
// language code, or nullptr on the All tab.
void statsMonthStatus(const void* ctx, const uint16_t year, const uint8_t month, bool out[32]) {
  if (ctx) {
    READING_STATS_STORE.getMonthStatus(static_cast<const char*>(ctx), year, month, out);
  } else {
    READING_STATS_STORE.getMonthStatus(year, month, out);
  }
}
int statsDaysReadInMonth(const void* ctx, const uint16_t year, const uint8_t month) {
  return ctx ? READING_STATS_STORE.getDaysReadInMonth(static_cast<const char*>(ctx), year, month)
             : READING_STATS_STORE.getDaysReadInMonth(year, month);
}
}  // namespace

const char* ReadingStatsActivity::selectedCode() const {
  const int index = selectedTab - 1;
  if (index < 0 || index >= static_cast<int>(languages.size())) return nullptr;
  return languages[index].code;
}

std::string ReadingStatsActivity::makeTabLabel(const char* code) {
  if (!code[0]) return tr(STR_LANGUAGE_UNKNOWN);
  // Endonym from the UI-language list; those strings are in flash already, so this costs no
  // table of its own.
  if (const char* name = I18n::languageNameForCode(code)) return name;
  // No UI for this language (say "zh"): show the bare tag rather than mislabel it.
  std::string out(code);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](const char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; });
  return out;
}

void ReadingStatsActivity::selectTab(const int index) {
  if (index == selectedTab || index < 0 || index >= static_cast<int>(tabLabels.size())) return;
  selectedTab = index;
  scrollOffset = 0;
  requestUpdate();
}

void ReadingStatsActivity::stepTab(const int direction) {
  const int count = static_cast<int>(tabLabels.size());
  if (count <= 1) return;
  selectTab((selectedTab + direction + count) % count);
}

void ReadingStatsActivity::ringSetFocus(const TabRing::Focus focus, const bool atEnd) {
  tabFocus = focus == TabRing::Focus::BottomBar ? static_cast<int>(HomeTab::Stats) : -1;
  // The top of the ring is the top of the page, where the tabs are; coming up out of the bar
  // lands at the page's end.
  if (focus == TabRing::Focus::TopTabs) scrollOffset = 0;
  if (focus == TabRing::Focus::Content) scrollOffset = atEnd ? maxScrollOffset : 0;
}

void ReadingStatsActivity::onEnter() {
  Activity::onEnter();
  tabBand_.begin();
  READING_STATS_STORE.loadFromFile();
  READING_STATS_STORE.getLanguages(languages);
  tabLabels.clear();
  tabLabels.reserve(languages.size() + 1);
  tabLabels.emplace_back(tr(STR_STATS_TAB_ALL));
  std::transform(languages.begin(), languages.end(), std::back_inserter(tabLabels),
                 [](const ReadingStatsStore::LanguageSummary& l) { return makeTabLabel(l.code); });
  const Today today = getToday();
  calYear = today.year;
  calMonth = today.month;
  requestUpdate();
}

void ReadingStatsActivity::onExit() { Activity::onExit(); }

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
  haptic_feedback::touchAction();
  requestUpdate();
  return true;
}

void ReadingStatsActivity::loop() {
  if (TabRing::handleInput(*this, mappedInput, renderer) != TabRing::Result::None) return;
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
  if (tabFocus >= 0) {
    // Cursor in the bar: Up goes back to the end of the page, Down carries on to its top.
    if (mappedInput.wasPressed(MappedInputManager::Button::ScreenUp)) TabRing::step(*this, -1);
    if (mappedInput.wasPressed(MappedInputManager::Button::ScreenDown)) TabRing::step(*this, 1);
    return;
  }
  // Confirm cycles the tabs -- All, then one per language -- and past the last one steps into
  // the bottom bar.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    TabRing::confirmTopTabs(*this);
    return;
  }
  // Tapping a tab picks it: the tabs are FreeInkUI touch targets, exactly where the band drew them.
  {
    const int tab = tabBand_.tappedTab(mappedInput);
    if (tab >= 0) {
      selectTab(tab);
      return;
    }
    // Swallowed either way: a tap in the gap between labels must not fall through to the cards.
    int tabX = 0;
    int tabY = 0;
    if (tabBar.width > 0 && mappedInput.wasScreenTapped(tabX, tabY) && tabY >= tabBar.y &&
        tabY < tabBar.y + tabBar.height) {
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
  // A horizontal flick steps the tabs, in the reader's direction: right-to-left advances. A
  // left-to-right flick that starts in the left quarter is the back gesture and never arrives.
  if (swipe == MappedInputManager::SwipeDir::Left || swipe == MappedInputManager::SwipeDir::Right) {
    stepTab(swipe == MappedInputManager::SwipeDir::Left ? 1 : -1);
    return;
  }
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
      return;
    }
    // Bottom of the page: the next Down carries on into the bottom bar.
    if (TabRing::hasBottomBar(*this)) TabRing::leaveContent(*this, 1);
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
  // The tab bar replaces the hints at the bottom, so the scrollable area ends where it begins.
  if (HomeTabBar::enabled()) screen.height = HomeTabBar::top(renderer) - screen.y;

  // The tab band sits in the fixed strip under the header, exactly as it does in Library and
  // Settings, and the scrollable content starts below it.
  const int tabBarY = screen.y + metrics.topPadding + metrics.headerHeight;
  const int tabBarH = tabBandHeight(metrics, mappedInput.hasTouch());
  const int headerBottom = tabBarY + tabBarH + metrics.verticalSpacing;
  const int contentTop = headerBottom - scrollOffset;

  // Draw header AFTER content so it covers scrolled text underneath.
  // Content is drawn first, then the header area is cleared and redrawn on top.
  const int cardMargin = 20;
  const int cardX = screen.x + cardMargin;
  const int cardW = screen.width - 2 * cardMargin;

  const Today today = getToday();
  // nullptr on the All tab: the store keeps unfiltered totals separate from the per-language
  // ones, and "" is a real bucket (books that declare no language).
  const char* code = selectedCode();
  const int streak = code ? READING_STATS_STORE.getStreak(code, today.year, today.month, today.day)
                          : READING_STATS_STORE.getStreak(today.year, today.month, today.day);
  const uint16_t weekMinutes = code ? READING_STATS_STORE.getMinutesThisWeek(code, today.year, today.month, today.day)
                                    : READING_STATS_STORE.getMinutesThisWeek(today.year, today.month, today.day);
  bool weekDays[7] = {};
  if (code) {
    READING_STATS_STORE.getWeekStatus(code, today.year, today.month, today.day, today.dow, weekDays);
  } else {
    READING_STATS_STORE.getWeekStatus(today.year, today.month, today.day, today.dow, weekDays);
  }

  int y = contentTop + 8;

  // ==================== STREAK WIDGET ====================
  y += StatsWidgets::drawStreakCard(renderer, cardX, y, cardW, streak, weekMinutes, weekDays, today.dow) + 16;

  // ==================== 4 STAT CARDS (2x2) ====================
  const int booksFinished =
      code ? static_cast<int>(READING_STATS_STORE.getBooksFinished(code)) : READING_STATS_STORE.getBooksFinished();
  const int daysRead = code ? READING_STATS_STORE.getDaysRead(code) : READING_STATS_STORE.getDaysRead();
  const uint32_t totalMin = code ? READING_STATS_STORE.getTotalMinutes(code) : READING_STATS_STORE.getTotalMinutes();
  const int longestStreak = code ? READING_STATS_STORE.getLongestStreak(code) : READING_STATS_STORE.getLongestStreak();

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
  const StatsWidgets::MonthSource source{code, statsMonthStatus, statsDaysReadInMonth};
  y += StatsWidgets::drawMonthCalendar(renderer, cardX, y, cardW, calYear, calMonth, today, source, &monthNav);

  // Compute max scroll: content bottom minus the visible area.
  const int contentEndY = y + 10;  // 10px bottom margin
  // Where the scrollable area really ends: the tab bar's top edge when it is drawn, otherwise the
  // old hint-band allowance. Reserving the smaller of the two left the last card (View details)
  // unable to scroll clear of the bar.
  const int visibleBottom = HomeTabBar::enabled() ? HomeTabBar::top(renderer) : renderer.getScreenHeight() - 50;
  const int visibleHeight = visibleBottom - headerBottom;
  maxScrollOffset = contentEndY - headerBottom - visibleHeight + scrollOffset;
  if (maxScrollOffset < 0) maxScrollOffset = 0;
  scrollPageHeight = visibleHeight;

  // Redraw header and band on top of scrolled content so text doesn't bleed through.
  renderer.fillRect(0, 0, screen.width, headerBottom - metrics.verticalSpacing, false);
  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_STATS), nullptr, HomeTabBar::showsBackButton(true), UiTabBand::drawsTopRule() ? 0 : -1);
  // Kept for loop(), so a tap in the band's gaps is swallowed. Drawn focused while the cursor is
  // on the page: Confirm acts on the tabs there and nothing else.
  tabBar = Rect{0, tabBarY, screen.width, tabBarH};
  {
    constexpr int MAX_TABS = 12;
    freeink::ui::TabItem tabs[MAX_TABS];
    const int total = static_cast<int>(tabLabels.size());
    const int count = std::min(total, MAX_TABS);
    // Past MAX_TABS languages the array holds the run that ends on the selected one.
    const int first = std::clamp(selectedTab - count + 1, 0, total - count);
    for (int i = 0; i < count; i++) {
      tabs[i].label = tabLabels[first + i].c_str();
      tabs[i].value = static_cast<int16_t>(first + i);
      tabs[i].selected = first + i == selectedTab;
    }
    UiTabBand::Options options;
    options.focused = tabFocus < 0;
    options.hasTouch = mappedInput.hasTouch();
    tabBand_.render(tabs, count, options, tabBarY);
  }

  if (HomeTabBar::enabled()) {
    HomeTabBar::draw(renderer, HomeTab::Stats, tabFocus);
  } else {
    // Hints name the months Left/Right land on.
    char prevBuf[16], nextBuf[16];
    uint16_t py = calYear, ny = calYear;
    uint8_t pm = calMonth, nm = calMonth;
    StatsWidgets::stepMonth(py, pm, -1);
    StatsWidgets::stepMonth(ny, nm, +1);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tabLabels.size() > 1 ? tr(STR_SWITCH) : "",
                                              StatsWidgets::monthAbbrev(pm, prevBuf, sizeof(prevBuf)),
                                              StatsWidgets::monthAbbrev(nm, nextBuf, sizeof(nextBuf)));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
