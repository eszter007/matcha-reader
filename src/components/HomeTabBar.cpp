#include "HomeTabBar.h"

#include <BoardConfig.h>
#include <GfxRenderer.h>

#include "MappedInputManager.h"
#include "UITheme.h"
#include "activities/Activity.h"
#include "icons/tabBarIcons.h"

namespace {
constexpr int ICON_SIZE = 32;
// Underline under the tab you are in, matching the cover grid's previous fui tab band.
constexpr int UNDERLINE_HEIGHT = 2;
constexpr int UNDERLINE_GAP = 6;

struct TabIcons {
  const uint8_t* outline;
  const uint8_t* filled;
};

// Index order must match HomeTab.
constexpr TabIcons ICONS[HomeTabBar::COUNT] = {
    {HomeIcon, HomeFilledIcon},   {LibraryTabIcon, LibraryTabFilledIcon},   {TransferTabIcon, TransferTabFilledIcon},
    {StatsIcon, StatsFilledIcon}, {SettingsTabIcon, SettingsTabFilledIcon},
};

// Horizontal band the slots divide up: the theme's side padding, so the outer icons line up with
// the header's clock and battery.
int barLeft() { return UITheme::getInstance().getMetrics().headerSidePadding; }

int barWidth(const GfxRenderer& renderer) { return renderer.getScreenWidth() - 2 * barLeft(); }

int slotStep(const GfxRenderer& renderer) { return barWidth(renderer) / HomeTabBar::COUNT; }
}  // namespace

bool HomeTabBar::enabled() { return UITheme::hasCoverGridHome(); }

int HomeTabBar::height() { return UITheme::getInstance().getMetrics().coverGridTabBarHeight; }

int HomeTabBar::top(const GfxRenderer& renderer) { return renderer.getScreenHeight() - height(); }

bool HomeTabBar::showsBackButton(const bool atTabRoot) { return !(enabled() && atTabRoot); }

int HomeTabBar::extraPageReserve() {
  return enabled() ? height() - UITheme::getInstance().getMetrics().buttonHintsHeight : 0;
}

int HomeTabBar::bottomInset() { return enabled() ? height() : UITheme::getInstance().getMetrics().buttonHintsHeight; }

void HomeTabBar::draw(const GfxRenderer& renderer, const HomeTab active, const int focused) {
  const int bandTop = top(renderer);
  const int step = slotStep(renderer);
  if (step <= 0) return;
  const int iconY = bandTop + (height() - ICON_SIZE - UNDERLINE_GAP - UNDERLINE_HEIGHT) / 2;

  renderer.fillRect(0, bandTop, renderer.getScreenWidth(), height(), false);

  // The ring marks where the cursor sits, including on the tab you are already in: a button
  // board's cursor enters the band on the active slot, and with nothing drawn there the first
  // Left/Right press looked like a dead key. Nothing moves a cursor into the band on a board
  // without those keys, so `focused` is simply -1 there.
  const int cursor = focused;
  for (int i = 0; i < COUNT; ++i) {
    const int slotX = barLeft() + i * step;
    const bool isActive = i == static_cast<int>(active);
    if (i == cursor) {
      UITheme::drawFocusRing(renderer, Rect{slotX, bandTop, step, height()});
    }
    renderer.drawIcon(isActive ? ICONS[i].filled : ICONS[i].outline, slotX + (step - ICON_SIZE) / 2, iconY, ICON_SIZE);
    // The underline marks where the cursor is once it is in the band, and which tab you are in
    // only while it is not: both at once read as two selections.
    if (cursor >= 0 ? i == cursor : isActive) {
      const int lineWidth = ICON_SIZE + 8;
      renderer.fillRect(slotX + (step - lineWidth) / 2, iconY + ICON_SIZE + UNDERLINE_GAP, lineWidth, UNDERLINE_HEIGHT);
    }
  }
}

int HomeTabBar::hitTest(const MappedInputManager& input, const GfxRenderer& renderer, bool& tapped) {
  tapped = false;
  const int step = slotStep(renderer);
  if (step <= 0) return -1;
  int col = 0;
  const auto hit = input.colTouch(col, barLeft(), step, COUNT, top(renderer), renderer.getScreenHeight());
  if (hit == MappedInputManager::RowTouch::None) return -1;
  tapped = hit == MappedInputManager::RowTouch::Tap;
  return col;
}

HomeTabBar::Input HomeTabBar::route(const MappedInputManager& input, const GfxRenderer& renderer, const HomeTab current,
                                    int& focus, const bool allowButtons) {
  if (!enabled()) return Input::None;

  bool tapped = false;
  const int hit = hitTest(input, renderer, tapped);
  if (hit >= 0) {
    if (tapped) activate(static_cast<HomeTab>(hit), current);
    return Input::Consumed;
  }

  if (!allowButtons) return Input::None;

  // The first Left/Right parks the cursor on the tab you are in, so the band lights up where the
  // user already is rather than jumping to an end.
  const int from = focus >= 0 ? focus : static_cast<int>(current);
  if (input.wasReleased(MappedInputManager::Button::Left)) {
    focus = (from + COUNT - 1) % COUNT;
    return Input::FocusMoved;
  }
  if (input.wasReleased(MappedInputManager::Button::Right)) {
    focus = (from + 1) % COUNT;
    return Input::FocusMoved;
  }
  if (focus >= 0 && input.wasReleased(MappedInputManager::Button::Confirm)) {
    if (focus == static_cast<int>(current)) return Input::Exited;
    activate(static_cast<HomeTab>(focus), current);
    return Input::Consumed;
  }
  return Input::None;
}

void HomeTabBar::activate(const HomeTab tab, const HomeTab current) {
  if (tab == current) return;
  switch (tab) {
    case HomeTab::Home:
      activityManager.goHome();
      break;
    case HomeTab::Library:
      activityManager.goToLibrary();
      break;
    case HomeTab::Transfer:
      activityManager.goToFileTransfer();
      break;
    case HomeTab::Stats:
      activityManager.goToReadingStats();
      break;
    case HomeTab::Settings:
      activityManager.goToSettings();
      break;
    default:
      break;
  }
}
