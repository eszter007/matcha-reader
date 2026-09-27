#include "UiListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace fui = freeink::ui;

UiListActivity::UiListActivity(const char* name, GfxRenderer& renderer, MappedInputManager& mappedInput,
                               const bool wantsTouchLongPress)
    : Activity(name, renderer, mappedInput), UiAppHost(renderer), wantsTouchLongPress(wantsTouchLongPress) {}

void UiListActivity::onEnter() {
  Activity::onEnter();
  activeNav().reset();
  resetUi();
  app.on(ACTION_ROW, &UiListActivity::rowActionTrampoline, this);
  app.setScreen(&UiListActivity::screenTrampoline, this);
  requestUpdate();
}

void UiListActivity::screenTrampoline(UiScreen& screen, void* user) {
  static_cast<UiListActivity*>(user)->buildScreen(screen);
}

void UiListActivity::rowActionTrampoline(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<UiListActivity*>(user);
  if (event.value < 0 || event.value >= self->listCount()) return;
  self->onRowAction(event);
}

void UiListActivity::onRowAction(const fui::ActionEvent& event) {
  activeNav().selected = event.value;
  if (event.longPress) {
    onRowLongPress(event.value);
    return;
  }
  activateIndex(event.value);
}

bool UiListActivity::handleButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onBackButton();
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const int selected = activeNav().selected;
    if (selected >= 0 && selected < listCount()) activateIndex(selected);
    return true;
  }
  return false;
}

bool UiListActivity::routeListTouch() {
  // Touch goes through the FreeInkApp: render() registered the row hit rects;
  // route the snapshot and let the action trampoline dispatch.
  const auto route = UiAppHost::routeTouch(mappedInput, wantsTouchLongPress);
  // No pressed-state repaint: the render it triggers would drop a slow tap's
  // release inside the uiReady window (tap-to-activate needed two taps), and
  // it costs a second e-ink refresh per tap.
  if (route.routed && app.invalidated()) requestUpdate();
  return static_cast<bool>(route);  // dispatched to the action handler
}

int UiListActivity::selectionCursor() {
  return pendingSelection_ >= 0 ? pendingSelection_ : activeNav().selected.load();
}

void UiListActivity::moveSelectionTo(const int index) {
  {
    // TRY the lock, never block on it. The render task holds it for the whole render INCLUDING
    // the panel wait (~502ms for a FAST refresh), and buttons are polled -- InputManager::update()
    // runs on this same loop -- so blocking here stops input being sampled at all, and a press
    // that both starts and ends inside a refresh is lost outright. That is what made tapping
    // through a list feel like it was ignoring presses.
    //
    // The lock itself is still required to mutate nav: the render task reads selection/viewport
    // mid-build (syncToProps, layout feedback), so an unlocked write would tear them. When it is
    // busy, park the target and let loop() apply it the moment the render finishes.
    RenderLock lock{RenderLock::Try{}};
    if (!lock.held()) {
      pendingSelection_ = index;
      return;
    }
    auto& n = activeNav();
    n.selected = index;
    n.follow(listCount());
    pendingSelection_ = -1;
  }
  requestUpdate();
}

void UiListActivity::loop() {
  // Apply a move parked while the render task held the lock. Re-parks itself if the next render
  // is already running, so it simply retries on the following tick.
  if (pendingSelection_ >= 0) {
    const int parked = pendingSelection_;
    pendingSelection_ = -1;
    moveSelectionTo(parked);
  }

  if (handleCustomInput()) return;
  if (handleTabBarInput()) return;
  if (handleButtons()) return;
  if (routeListTouch()) return;

  // Swipes scroll the viewport; the selection stays put (it may scroll
  // off-screen) and button navigation pulls the view back to it.
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    auto& n = activeNav();
    const int delta = swipe == MappedInputManager::SwipeDir::Up ? n.inputPageRows() : -n.inputPageRows();
    LOG_DBG("LIST", "%s swipe delta=%d count=%d", name.c_str(), delta, listCount());
    n.requestScroll(delta);
    requestUpdate();
    return;
  }

  navigateButtons();
}

void UiListActivity::onTabBandExit() {
  topBandFocused = hasTopBand();
  if (!topBandFocused) moveSelectionTo(0);
}

void UiListActivity::enterBottomBand() {
  tabFocus = static_cast<int>(tabBarTab());
  topBandFocused = false;
  requestUpdate();
}

void UiListActivity::navigateButtons() {
  const int count = listCount();
  auto& n = activeNav();
  // One ring: top band (when the screen has one) -> rows -> bottom bar -> back to the top.
  const auto stepDown = [this, count] {
    if (tabFocus >= 0) {
      tabFocus = -1;
      if (hasTopBand()) {
        topBandFocused = true;
      } else {
        moveSelectionTo(0);
      }
      requestUpdate();
      return;
    }
    if (topBandFocused) {
      topBandFocused = false;
      moveSelectionTo(0);
      return;
    }
    if (hasTabBar() && (count <= 0 || selectionCursor() >= count - 1)) {
      enterBottomBand();
      return;
    }
    moveSelectionTo(ButtonNavigator::nextIndex(selectionCursor(), count));
  };
  const auto stepUp = [this, count] {
    if (tabFocus >= 0) {
      tabFocus = -1;
      moveSelectionTo(count > 0 ? count - 1 : 0);
      requestUpdate();
      return;
    }
    if (topBandFocused) {
      topBandFocused = false;
      if (hasTabBar()) {
        enterBottomBand();
      } else {
        moveSelectionTo(count > 0 ? count - 1 : 0);
      }
      return;
    }
    if (selectionCursor() <= 0 && (hasTopBand() || hasTabBar())) {
      if (hasTopBand()) {
        topBandFocused = true;
        requestUpdate();
      } else {
        enterBottomBand();
      }
      return;
    }
    moveSelectionTo(ButtonNavigator::previousIndex(selectionCursor(), count));
  };
  buttonNavigator.onNextRelease(stepDown);
  buttonNavigator.onPreviousRelease(stepUp);
  // Page by the rows the last build actually drew (pageRows), not the
  // fixed-height visibleRows estimate: with wrapped labels the estimate
  // overshoots and rows between pages would never be shown. The measurement
  // can be one build old while a refresh is in flight; the next layout's
  // feedback corrects the viewport.
  buttonNavigator.onNextContinuous([this, count, &n] {
    moveSelectionTo(ButtonNavigator::nextPageIndex(selectionCursor(), count, n.inputPageRows()));
  });
  buttonNavigator.onPreviousContinuous([this, count, &n] {
    moveSelectionTo(ButtonNavigator::previousPageIndex(selectionCursor(), count, n.inputPageRows()));
  });
}

void UiListActivity::syncListViewport(UiScreen& screen, fui::ListProps& props, const int selectionOffset) {
  props.partialTrailingRow = true;
  auto& n = activeNav();
  const int prevTop = n.top;
  const bool trusted = n.trusts(listCount());
  const int drawn = n.drawnRows;

  screen.syncListViewport(n, props, listCount(), selectionOffset);

  // When the selection is already visible in the current viewport (based on
  // the measured drawnRows rather than the unweighted visibleRows estimate),
  // keep selection-follow anchored instead of jumping to top. Explicit swipe
  // scrolling clears followPending and must retain its new viewport.
  if (n.followPending && trusted && drawn > 0) {
    const int sel = props.selectedIndex;
    if (sel >= prevTop && sel < prevTop + drawn) {
      n.top = prevTop;
      props.topIndex = static_cast<uint16_t>(prevTop);
    }
  }
}

void UiListActivity::drawChrome() {
  const char* title = headerTitle();
  if (!title) return;
  const auto& metrics = UITheme::getInstance().getMetrics();
  // The rule under the title only earns its place once there is content behind it: at the top of
  // a list it is a second horizontal line stacked on the band below it.
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight}, title, nullptr,
                 HomeTabBar::showsBackButton(hasTabBar()), activeNav().top > 0 ? 1 : 0);
}

bool UiListActivity::hasTabBar() const { return tabBarTab() != HomeTab::Count && HomeTabBar::enabled(); }

bool UiListActivity::handleTabBarInput() {
  if (topBandFocused) {
    // Left/Right belong to the screen's own band while the cursor is on it.
    if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      stepTopBand(-1);
      return true;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      stepTopBand(1);
      return true;
    }
    // Confirm advances the ring rather than acting on a row the cursor is not on.
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (hasTabBar()) {
        enterBottomBand();
      } else {
        topBandFocused = false;
        moveSelectionTo(0);
      }
      return true;
    }
  }
  if (!hasTabBar()) return false;
  const auto routed = HomeTabBar::route(mappedInput, renderer, tabBarTab(), tabFocus, !topBandFocused);
  if (routed == HomeTabBar::Input::Exited) {
    // Confirm on the tab you are already in hands the cursor back to the top of the ring.
    tabFocus = -1;
    onTabBandExit();
    requestUpdate();
    return true;
  }
  if (routed == HomeTabBar::Input::FocusMoved) requestUpdate();
  if (routed == HomeTabBar::Input::Consumed) app.clearTapFlash();
  return routed != HomeTabBar::Input::None;
}

void UiListActivity::drawFooter() {
  if (hasTabBar()) {
    HomeTabBar::draw(renderer, tabBarTab(), tabFocus);
    return;
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void UiListActivity::render(RenderLock&&) {
  renderer.clearScreen();
  drawChrome();
  renderUi();
  // Wrapped labels grow rows, so fewer rows can fit than the fixed-height
  // estimate ListNav plans with. list() reports the real layout back
  // (ListNav::onListRendered); when the selection landed past the drawn rows
  // the nav advanced the viewport and asked for another build. Bounded: top
  // strictly advances toward the selection each pass.
  for (int pass = 0; activeNav().consumeRebuildNeeded() && pass < 8; ++pass) {
    renderer.clearScreen();
    drawChrome();
    renderUi();
  }
  drawFooter();
  renderer.displayBuffer();
}
