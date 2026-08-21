#include "UiListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

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

int UiListActivity::selectionCursor() { return pendingSelection_ >= 0 ? pendingSelection_ : activeNav().selected; }

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
  if (handleButtons()) return;
  if (routeListTouch()) return;

  // Swipes scroll the viewport; the selection stays put (it may scroll
  // off-screen) and button navigation pulls the view back to it.
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    bool moved = false;
    {
      // Same nav-vs-render race as moveSelectionTo: the render task writes
      // pageRows/top mid-build, so read and mutate under one lock.
      RenderLock lock(*this);
      auto& n = activeNav();
      const int delta = swipe == MappedInputManager::SwipeDir::Up ? n.pageRows() : -n.pageRows();
      moved = n.scrollBy(delta, listCount());
    }
    if (moved) requestUpdate();
    return;
  }

  navigateButtons();
}

void UiListActivity::navigateButtons() {
  const int count = listCount();
  auto& n = activeNav();
  buttonNavigator.onNextRelease(
      [this, count] { moveSelectionTo(ButtonNavigator::nextIndex(selectionCursor(), count)); });
  buttonNavigator.onPreviousRelease(
      [this, count] { moveSelectionTo(ButtonNavigator::previousIndex(selectionCursor(), count)); });
  // Page by the rows the last build actually drew (pageRows), not the
  // fixed-height visibleRows estimate: with wrapped labels the estimate
  // overshoots and rows between pages would never be shown.
  buttonNavigator.onNextContinuous(
      [this, count, &n] { moveSelectionTo(ButtonNavigator::nextPageIndex(selectionCursor(), count, n.pageRows())); });
  buttonNavigator.onPreviousContinuous([this, count, &n] {
    moveSelectionTo(ButtonNavigator::previousPageIndex(selectionCursor(), count, n.pageRows()));
  });
}

void UiListActivity::syncListViewport(UiScreen& screen, fui::ListProps& props, const bool hasSubtitle) {
  int16_t rowHeight = screen.theme().rowHeight;
  if (!mappedInput.hasTouch()) {
    // Non-touch hardware (X3/X4) keeps the original, denser per-theme row
    // height instead of FreeInkUI's touch-target-sized default, so lists fit
    // as many rows per screen as they did before the FreeInkUI migration.
    // props.rowHeight must be set explicitly: screen.list() otherwise falls
    // back to the (touch-friendly) theme token, not this local value.
    // A label that must wrap (labelText.maxLines > 1) grows only its own row:
    // list() sizes wrapped items per-row, so the dense height stays.
    const auto& metrics = UITheme::getInstance().getMetrics();
    rowHeight = static_cast<int16_t>(hasSubtitle ? metrics.listWithSubtitleRowHeight : metrics.listRowHeight);
    props.rowHeight = rowHeight;
  }
  activeNav().syncToProps(screen.body(), rowHeight, screen.theme().listRowGap, listCount(), props);
}

void UiListActivity::drawChrome() {
  const char* title = headerTitle();
  if (!title) return;
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight}, title);
}

void UiListActivity::drawFooter() {
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
