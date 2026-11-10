#include "HomeTabBar.h"

#include <BoardConfig.h>
#include <GfxRenderer.h>

#include "MappedInputManager.h"
#include "UITheme.h"
#include "activities/Activity.h"
#include "icons/tabBarIcons.h"

namespace {
constexpr int ICON_SIZE = 32;

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
  const int iconY = bandTop + (height() - ICON_SIZE) / 2;

  renderer.fillRect(0, bandTop, renderer.getScreenWidth(), height(), false);

  // The ring marks where the cursor sits, including on the tab you are already in: a button
  // board's cursor enters the band on the active slot, and with nothing drawn there the first
  // Left/Right press looked like a dead key. Nothing moves a cursor into the band on a board
  // without those keys, so `focused` is simply -1 there. On a touch board it shows only while
  // buttons are in use; the filled icon marks the tab you are in.
  const int cursor = MappedInputManager::buttonCursorShown() ? focused : -1;
  for (int i = 0; i < COUNT; ++i) {
    const int slotX = barLeft() + i * step;
    const bool isActive = i == static_cast<int>(active);
    if (i == cursor) {
      UITheme::drawFocusRing(renderer, Rect{slotX, bandTop, step, height()});
    }
    renderer.drawIcon(isActive ? ICONS[i].filled : ICONS[i].outline, slotX + (step - ICON_SIZE) / 2, iconY, ICON_SIZE);
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
  // user already is rather than jumping to an end. Taken on the press: Left/Right are also
  // NavPrevious/NavNext, which the lists act on at the press, so a release here would come after
  // the list had already moved the cursor out of the band.
  const int from = focus >= 0 ? focus : static_cast<int>(current);
  if (input.wasPressed(MappedInputManager::Button::Left)) {
    focus = (from + COUNT - 1) % COUNT;
    return Input::FocusMoved;
  }
  if (input.wasPressed(MappedInputManager::Button::Right)) {
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
