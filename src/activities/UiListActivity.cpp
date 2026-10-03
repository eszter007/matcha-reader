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
  frameValid_ = false;
  bandStepRequest_ = 0;
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

TabRing::Focus UiListActivity::ringFocus() const {
  if (tabFocus >= 0) return TabRing::Focus::BottomBar;
  return topBandFocused ? TabRing::Focus::TopTabs : TabRing::Focus::Content;
}

void UiListActivity::ringSetFocus(const TabRing::Focus focus, const bool atEnd) {
  tabFocus = focus == TabRing::Focus::BottomBar ? static_cast<int>(tabBarTab()) : -1;
  topBandFocused = focus == TabRing::Focus::TopTabs;
  if (focus == TabRing::Focus::Content) {
    const int count = listCount();
    moveSelectionTo(atEnd && count > 0 ? count - 1 : 0);
  }
}

void UiListActivity::navigateButtons() {
  const int count = listCount();
  auto& n = activeNav();
  buttonNavigator.onNextPress([this, count] {
    if (ringFocus() != TabRing::Focus::Content) return TabRing::step(*this, 1);
    if (count <= 0 || selectionCursor() >= count - 1) return TabRing::leaveContent(*this, 1);
    moveSelectionTo(selectionCursor() + 1);
  });
  buttonNavigator.onPreviousPress([this, count] {
    if (ringFocus() != TabRing::Focus::Content) return TabRing::step(*this, -1);
    if (selectionCursor() <= 0) return TabRing::leaveContent(*this, -1);
    moveSelectionTo(selectionCursor() - 1);
  });
  // Page by the rows the last build actually drew (pageRows), not the
  // fixed-height visibleRows estimate: with wrapped labels the estimate
  // overshoots and rows between pages would never be shown. The measurement
  // can be one build old while a refresh is in flight; the next layout's
  // feedback corrects the viewport.
  // A hold pages through the rows. If the press that started it carried the cursor onto a band,
  // there is nothing to page, and moving the hidden row selection would only surprise later.
  buttonNavigator.onNextContinuous([this, count, &n] {
    if (ringFocus() != TabRing::Focus::Content) return;
    moveSelectionTo(ButtonNavigator::nextPageIndex(selectionCursor(), count, n.inputPageRows()));
  });
  buttonNavigator.onPreviousContinuous([this, count, &n] {
    if (ringFocus() != TabRing::Focus::Content) return;
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
  const uint32_t before = activityManager.updateRequestCount();
  const auto result = TabRing::handleInput(*this, mappedInput, renderer);
  if (result == TabRing::Result::None) return false;
  if (result == TabRing::Result::BarStepped) {
    // A step between bar slots: when it is the only thing pending, render() repaints just the band.
    const uint32_t lastStep = bandStepRequest_;
    const bool onlyBandPending = before == lastRenderRequest_ || (lastStep != 0 && before == lastStep);
    bandStepRequest_ = onlyBandPending ? activityManager.updateRequestCount() : 0;
  } else {
    app.clearTapFlash();
  }
  return true;
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
  const uint32_t seen = activityManager.updateRequestCount();
  const uint32_t step = bandStepRequest_.exchange(0);
  lastRenderRequest_ = seen;
  if (step != 0 && step == seen && frameValid_ && renderedTabFocus_ >= 0 && tabFocus >= 0 && hasTabBar()) {
    HomeTabBar::draw(renderer, tabBarTab(), tabFocus);
    renderedTabFocus_ = tabFocus;
    renderer.displayBuffer();
    return;
  }

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
  frameValid_ = true;
  renderedTabFocus_ = tabFocus;
  renderer.displayBuffer();
}
