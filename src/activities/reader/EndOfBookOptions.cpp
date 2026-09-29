#include "EndOfBookOptions.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <I18n.h>

#include "BookStats.h"
#include "CrossPointSettings.h"
#include "ReaderUtils.h"
#include "ReadingStatsStore.h"
// ReaderUtils.h pulls in ActivityManager.h, which only forward-declares Activity while holding
// std::unique_ptr<Activity> members. Destroying that unique_ptr needs the complete type, so the
// definition must be visible here.
#include "activities/Activity.h"
#include "components/StatsWidgets.h"
#include "components/UITheme.h"
#include "components/icons/dictionaryIcons.h"
#include "components/icons/endOfBookCat.h"
#include "components/icons/stats_icons.h"
#include "fontIds.h"
#include "util/ButtonNavigator.h"
#include "util/NextBookFinder.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_ROW = 1;

// Display name without the file extension, mirroring the file browser rows
std::string displayName(const std::string& filename) {
  const auto pos = filename.rfind('.');
  return filename.substr(0, pos);
}

// "1st", "2nd", "3rd", "11th", "22nd".
void formatOrdinal(char* buf, const size_t size, const unsigned n) {
  const unsigned lastTwo = n % 100;
  const char* suffix = "th";
  if (lastTwo < 11 || lastTwo > 13) {
    if (n % 10 == 1) suffix = "st";
    if (n % 10 == 2) suffix = "nd";
    if (n % 10 == 3) suffix = "rd";
  }
  snprintf(buf, size, "%u%s", n, suffix);
}

// "4h 32m", or "45m" under an hour.
void formatDuration(char* buf, const size_t size, const uint32_t minutes) {
  if (minutes >= 60) {
    snprintf(buf, size, "%luh %lum", static_cast<unsigned long>(minutes / 60),
             static_cast<unsigned long>(minutes % 60));
  } else {
    snprintf(buf, size, "%lum", static_cast<unsigned long>(minutes));
  }
}

struct StatCell {
  const uint8_t* icon;
  int iconSize;
  char value[16];
  const char* label;
};

// One row of stat cells, each an icon over a bold value over a small label, centred in equal
// columns. Returns the row height.
int drawStatRow(const GfxRenderer& renderer, const Rect& area, const int y, const StatCell* cells, const int count) {
  constexpr int ICON = 24;
  const int valueH = renderer.getLineHeight(UI_12_FONT_ID);
  const int labelH = renderer.getLineHeight(SMALL_FONT_ID);
  const int colW = area.width / count;
  for (int i = 0; i < count; i++) {
    const int cx = area.x + colW * i + colW / 2;
    const int iconY = y + (ICON - cells[i].iconSize) / 2;
    renderer.drawIcon(cells[i].icon, cx - cells[i].iconSize / 2, iconY, cells[i].iconSize);
    const int vw = renderer.getTextWidth(UI_12_FONT_ID, cells[i].value, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, cx - vw / 2, y + ICON + 4, cells[i].value, true, EpdFontFamily::BOLD);
    const std::string label = renderer.truncatedText(SMALL_FONT_ID, cells[i].label, colW - 8);
    const int lw = renderer.getTextWidth(SMALL_FONT_ID, label.c_str());
    renderer.drawText(SMALL_FONT_ID, cx - lw / 2, y + ICON + 4 + valueH, label.c_str(), true);
  }
  return ICON + 4 + valueH + labelH;
}
}  // namespace

EndOfBookOptions::EndOfBookOptions(GfxRenderer& renderer) : UiAppHost(renderer), renderer(renderer) {}

void EndOfBookOptions::loadSummary(const std::string& bookPath, const char* bookLanguage) {
  READING_STATS_STORE.loadFromFile();
  READING_STATS_STORE.markBookFinished(bookPath);
  READING_STATS_STORE.saveToFile();

  BookStats stats;
  if (stats.load(bookPath.c_str())) {
    summary.minutes = stats.getTotalMinutes();
    summary.daySpan = stats.getDaySpan();
    summary.lookups = stats.getLookups();
    summary.sentences = stats.getSentencesSaved();
  }
  const StatsWidgets::Today today = StatsWidgets::getToday();
  summary.streak = READING_STATS_STORE.getStreak(today.year, today.month, today.day);
  summary.booksFinished = READING_STATS_STORE.getBooksFinished();

  // Primary subtag, lowercased, as the stats store buckets it ("ja-JP" -> "ja").
  char code[4] = {};
  for (size_t i = 0; bookLanguage && i < 3 && bookLanguage[i] && bookLanguage[i] != '-' && bookLanguage[i] != '_';
       i++) {
    const char c = bookLanguage[i];
    code[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
  }
  if (code[0]) {
    summary.languageBooksFinished = READING_STATS_STORE.getBooksFinished(code);
    if (const char* name = I18n::languageNameForCode(code)) summary.languageName = name;
  }
}

void EndOfBookOptions::loadOnce(const std::string& currentBookPath, const std::string& bookTitle,
                                const char* bookLanguage) {
  if (isLoaded.load(std::memory_order_acquire)) {
    return;
  }
  loadSummary(currentBookPath, bookLanguage);
  folder = FsHelpers::extractFolderPath(currentBookPath);
  if (!bookTitle.empty()) {
    title = bookTitle;
  } else {
    const auto slash = currentBookPath.find_last_of('/');
    title = displayName(slash == std::string::npos ? currentBookPath : currentBookPath.substr(slash + 1));
  }
  names = NextBookFinder::findNextBooks(currentBookPath, MAX_SUGGESTIONS);
  selector.store(0, std::memory_order_relaxed);
  // One-time app setup on the render task, before the first render/route.
  resetUi();
  app.on(ACTION_ROW, &EndOfBookOptions::onRowEvent, this);
  app.setScreen(&EndOfBookOptions::listScreen, this);
  buildRowItems();
  // Release-publish so the main task, which gates all access on isLoaded, never
  // observes a partially built list (rowItems/rowLabels included)
  isLoaded.store(true, std::memory_order_release);
}

// Populates rowLabels/rowItems from names + the trailing "Home" row. Called
// once here since names never changes after loadOnce() completes.
void EndOfBookOptions::buildRowItems() {
  rowCount = 0;
  for (const auto& name : names) {
    if (rowCount >= MAX_ROWS) break;
    rowLabels[rowCount] = displayName(name);
    fui::ListItem item;
    item.label = rowLabels[rowCount].c_str();
    item.actionValue = static_cast<int16_t>(rowCount);
    rowItems[rowCount] = item;
    rowCount++;
  }
  if (rowCount < MAX_ROWS) {
    rowLabels[rowCount] = tr(STR_EOB_HOME);
    fui::ListItem item;
    item.label = rowLabels[rowCount].c_str();
    item.actionValue = static_cast<int16_t>(rowCount);
    rowItems[rowCount] = item;
    rowCount++;
  }
}

bool EndOfBookOptions::menuActive() const { return isLoaded.load(std::memory_order_acquire); }

std::string EndOfBookOptions::fullPath(const size_t index) const {
  if (index >= names.size()) {
    return {};
  }
  return folder == "/" ? "/" + names[index] : folder + "/" + names[index];
}

void EndOfBookOptions::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<EndOfBookOptions*>(user);
  if (event.value < 0 || event.value > static_cast<int16_t>(self->names.size())) return;
  self->selector.store(event.value, std::memory_order_relaxed);
  // The tapped row leaves this screen (open book or home); a lingering flash
  // would gray an unrelated element on the next render.
  self->app.clearTapFlash();
  self->tappedRow = event.value;
}

EndOfBookOptions::Action EndOfBookOptions::handleMenuInput(const MappedInputManager& input, std::string* openPath) {
  // Touch goes through the FreeInkApp: render() registered the row hit rects;
  // route the snapshot and let onRowEvent record the tapped row.
  tappedRow = -1;
  const auto route = routeTouch(input);
  // cppcheck can't see that route() dispatches into onRowEvent (registered via
  // app.on(ACTION_ROW, ...)), which sets tappedRow, so it flags this as always false.
  // cppcheck-suppress knownConditionTrueFalse
  if (route && tappedRow >= 0) {
    if (tappedRow < static_cast<int>(names.size())) {
      if (openPath) {
        *openPath = fullPath(tappedRow);
      }
      return Action::OpenBook;
    }
    return Action::GoHome;  // "Home" row tapped
  }
  if (route.routed && app.invalidated()) {
    return Action::Redraw;
  }

  const int selectedIndex = selector.load(std::memory_order_relaxed);
  if (input.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectedIndex < static_cast<int>(names.size())) {
      if (openPath) {
        *openPath = fullPath(selectedIndex);
      }
      return Action::OpenBook;
    }
    return Action::GoHome;  // "Home" entry selected
  }

  // Short-press Back returns to the last page; a long press falls through to the
  // reader's own handler (file browser). Home is reached through the list's Home entry.
  if (input.wasReleased(MappedInputManager::Button::Back) && input.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    return Action::LastPage;
  }

  // Selection movement on the standard list navigation buttons (side Up/Down plus front
  // Left/Right, orientation swap included). It follows the reader's page-turn semantics
  // (press-triggered by default, release-triggered when a long-press behavior is
  // configured, same rule as ReaderUtils::detectPageTurn). This matters on entry: with
  // press-triggered turns, the press that turned the final page already fired in the
  // reader, and its release must not double-fire into this menu.
  const bool usePress = SETTINGS.longPressButtonBehavior == CrossPointSettings::OFF;
  const auto triggered = [&](const MappedInputManager::Button button) {
    return usePress ? input.wasPressed(button) : input.wasReleased(button);
  };
  const int itemCount = static_cast<int>(names.size()) + 1;  // + "Home" entry
  if (triggered(MappedInputManager::Button::NavPrevious)) {
    selector.store(ButtonNavigator::previousIndex(selectedIndex, itemCount), std::memory_order_relaxed);
    return Action::Redraw;
  }
  if (triggered(MappedInputManager::Button::NavNext)) {
    selector.store(ButtonNavigator::nextIndex(selectedIndex, itemCount), std::memory_order_relaxed);
    return Action::Redraw;
  }
  return Action::None;
}

void EndOfBookOptions::listScreen(UiScreen& screen, void* user) {
  static_cast<EndOfBookOptions*>(user)->buildListScreen(screen);
}

void EndOfBookOptions::buildListScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // The list band starts under the header render() drew (listTop), and stops above the
  // button hints (the safe-area bottom edge).
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(listTop), static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height) + metrics.verticalSpacing),
      static_cast<int16_t>(safe.x)});

  // rowLabels/rowItems were built once in loadOnce() (see buildRowItems())
  // and reused here on every repaint.
  fui::ListProps props;
  props.items = rowItems;
  props.count = static_cast<uint16_t>(rowCount);
  props.selectedIndex = static_cast<int16_t>(selector.load(std::memory_order_relaxed));
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in handleMenuInput()
  screen.list(props);
}

int EndOfBookOptions::drawSummary(int y, const Rect& safe) const {
  const auto& metrics = UITheme::getInstance().getMetrics();

  // Milestone: "Your 12th book · 3rd in 日本語". Counts past this book's own finish only.
  if (summary.booksFinished > 0) {
    char nth[16];
    formatOrdinal(nth, sizeof(nth), summary.booksFinished);
    char line[96];
    snprintf(line, sizeof(line), tr(STR_EOB_NTH_BOOK), nth);
    if (summary.languageBooksFinished > 0 && !summary.languageName.empty()) {
      char nthLang[16];
      formatOrdinal(nthLang, sizeof(nthLang), summary.languageBooksFinished);
      const size_t used = strlen(line);
      snprintf(line + used, sizeof(line) - used, tr(STR_EOB_NTH_IN_LANGUAGE), nthLang, summary.languageName.c_str());
    }
    UITheme::drawCenteredText(renderer, safe, UI_10_FONT_ID, y, line);
    y += renderer.getLineHeight(UI_10_FONT_ID) + metrics.verticalSpacing * 3;
  }

  const Rect area{safe.x + metrics.contentSidePadding, safe.y, safe.width - metrics.contentSidePadding * 2,
                  safe.height};
  StatCell cells[3];
  int count = 0;
  cells[count] = {ClockIcon24, 24, {}, tr(STR_EOB_READING_TIME)};
  formatDuration(cells[count++].value, sizeof(cells[0].value), summary.minutes);
  if (summary.daySpan > 0) {
    cells[count] = {CalendarIcon24, 24, {}, tr(STR_EOB_DAYS_TO_FINISH)};
    snprintf(cells[count++].value, sizeof(cells[0].value), "%d", summary.daySpan);
  }
  if (summary.streak > 0) {
    cells[count] = {StreakIcon24, 24, {}, tr(STR_EOB_DAY_STREAK)};
    snprintf(cells[count++].value, sizeof(cells[0].value), "%d", summary.streak);
  }
  y += drawStatRow(renderer, area, y, cells, count);

  // The dictionary row only for books read with one.
  if (summary.lookups > 0 || summary.sentences > 0) {
    y += metrics.verticalSpacing * 2;
    cells[0] = {DictLookupIcon, 24, {}, tr(STR_STAT_WORDS_LOOKED_UP)};
    snprintf(cells[0].value, sizeof(cells[0].value), "%lu", static_cast<unsigned long>(summary.lookups));
    cells[1] = {DictAddCardIcon, 24, {}, tr(STR_STAT_SENTENCES_SAVED)};
    snprintf(cells[1].value, sizeof(cells[1].value), "%lu", static_cast<unsigned long>(summary.sentences));
    y += drawStatRow(renderer, area, y, cells, 2);
  }
  return y;
}

void EndOfBookOptions::render(GfxRenderer& renderer, const MappedInputManager& input) {
  const auto& metrics = UITheme::getInstance().getMetrics();

  if (!menuActive()) {
    renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() * 3 / 8, tr(STR_END_OF_BOOK), true,
                              EpdFontFamily::BOLD);
    return;
  }

  // The hints are drawn at the physical front buttons, which is a logical side/top edge in
  // the rotated orientations: lay out inside the safe area so nothing hides behind them.
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  int y = safe.y + metrics.verticalSpacing * 2;
  renderer.drawIcon(EndOfBookCat, safe.x + (safe.width - END_OF_BOOK_CAT_SIZE) / 2, y, END_OF_BOOK_CAT_SIZE);
  y += END_OF_BOOK_CAT_SIZE + metrics.verticalSpacing;

  const int titleLineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  UITheme::drawCenteredText(renderer, safe, UI_12_FONT_ID, y, tr(STR_EOB_YOU_FINISHED), true, EpdFontFamily::BOLD);
  y += titleLineHeight;
  const int textWidth = safe.width - metrics.contentSidePadding * 2;
  for (const auto& line : renderer.wrappedText(UI_12_FONT_ID, title.c_str(), textWidth, 2, EpdFontFamily::BOLD)) {
    UITheme::drawCenteredText(renderer, safe, UI_12_FONT_ID, y, line.c_str(), true, EpdFontFamily::BOLD);
    y += titleLineHeight;
  }
  y = drawSummary(y + metrics.verticalSpacing, safe);
  y += metrics.verticalSpacing * 2;

  if (!names.empty()) {
    renderer.drawText(UI_10_FONT_ID, safe.x + metrics.contentSidePadding, y, tr(STR_EOB_CONTINUE_WITH));
    y += renderer.getLineHeight(UI_10_FONT_ID) + metrics.verticalSpacing;
  }
  listTop = y;

  // The list renders through the FreeInkApp so its rows register touch hit
  // rects; renderUi re-derives the device context, picking up any rotation
  // since construction (reader menu rotate).
  renderUi();

  const auto labels = input.mapLabels(tr(STR_BACK), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
