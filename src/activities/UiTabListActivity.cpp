#include "UiTabListActivity.h"

#include <GfxRenderer.h>

#include <algorithm>
#include <cassert>

#include "MappedInputManager.h"
#include "components/HomeTabBar.h"
#include "components/UITheme.h"
#include "components/UiTabBand.h"

namespace fui = freeink::ui;

UiTabListActivity::UiTabListActivity(const char* name, GfxRenderer& renderer, MappedInputManager& mappedInput,
                                     const bool wantsTouchLongPress)
    : UiListActivity(name, renderer, mappedInput, wantsTouchLongPress) {}

void UiTabListActivity::onEnter() {
  // Size the per-tab state before the base resets activeNav() (which indexes
  // into it).
  tabNavs.assign(static_cast<size_t>(tabCount()), fui::ListNav{});
  UiListActivity::onEnter();
}

fui::ListNav& UiTabListActivity::activeNav() {
  if (tabNavs.empty()) return nav;  // pre-onEnter fallback
  // Invariant: subclasses keep activeTab() inside [0, tabCount()), and
  // tabCount() does not change after onEnter() sized tabNavs.
  assert(activeTab() >= 0 && static_cast<size_t>(activeTab()) < tabNavs.size());
  return tabNavs[static_cast<size_t>(activeTab())];
}

int UiTabListActivity::ringPos() const {
  if (tabNavs.empty()) return 0;
  assert(activeTab() >= 0 && static_cast<size_t>(activeTab()) < tabNavs.size());
  return tabNavs[static_cast<size_t>(activeTab())].selected;
}

void UiTabListActivity::onRowAction(const fui::ActionEvent& event) {
  activeNav().selected = event.value + 1;  // ring position, not row index
  if (event.longPress) {
    onRowLongPress(event.value);
    return;
  }
  activateIndex(event.value);
}

void UiTabListActivity::moveRingTo(const int ringIndex) {
  activeNav().requestSelection(ringIndex);
  requestUpdate();
}

void UiTabListActivity::ringSelectTopTab(const int index) {
  // TabRing took this key before navigateButtons() could snapshot the press, so a hold that
  // follows must not restore the snapshot of some earlier press.
  holdStart_.pending = false;
  onTabAction(index);
}

TabRing::Focus UiTabListActivity::ringFocus() const {
  if (tabFocus >= 0) return TabRing::Focus::BottomBar;
  return ringPos() == 0 ? TabRing::Focus::TopTabs : TabRing::Focus::Content;
}

void UiTabListActivity::ringSetFocus(const TabRing::Focus focus, const bool atEnd) {
  tabFocus = focus == TabRing::Focus::BottomBar ? static_cast<int>(tabBarTab()) : -1;
  if (focus == TabRing::Focus::TopTabs) moveRingTo(0);
  if (focus == TabRing::Focus::Content) moveRingTo(atEnd ? listCount() : 1);
}

void UiTabListActivity::restoreHoldStart() {
  if (!holdStart_.pending) return;
  holdStart_.pending = false;
  activeNav().selected = holdStart_.selected;
  tabFocus = holdStart_.tabFocus;
  // The press's own frame may already be on screen with the cursor where the press moved it.
  requestUpdate();
}

void UiTabListActivity::navigateButtons() {
  if (mappedInput.wasPressed(MappedInputManager::Button::NavNext) ||
      mappedInput.wasPressed(MappedInputManager::Button::NavPrevious)) {
    navigationStartedOnTabs = ringPos() == 0 && tabFocus < 0;
    holdStart_ = {activeNav().selected, tabFocus, true};
  }
  // A press walks the ring at once (TabRing decides every move between the parts); a hold steps
  // the tab instead, see holdStart_.
  buttonNavigator.onNextPress([this] {
    if (ringFocus() != TabRing::Focus::Content) return TabRing::step(*this, 1);
    if (ringPos() >= listCount()) return TabRing::leaveContent(*this, 1);
    moveRingTo(ringPos() + 1);
  });
  buttonNavigator.onPreviousPress([this] {
    if (ringFocus() != TabRing::Focus::Content) return TabRing::step(*this, -1);
    if (ringPos() <= 1) return TabRing::leaveContent(*this, -1);
    moveRingTo(ringPos() - 1);
  });
  buttonNavigator.onNextContinuous([this] {
    restoreHoldStart();
    if (tabFocus >= 0) return;  // a hold in the bottom bar steps nothing
    stepTab(1);
  });
  buttonNavigator.onPreviousContinuous([this] {
    restoreHoldStart();
    if (tabFocus >= 0) return;
    stepTab(-1);
  });
}

void UiTabListActivity::syncTabListViewport(UiScreen& screen, fui::ListProps& props) {
  syncListViewport(screen, props, 1);
}

void UiTabListActivity::buildTabBar(UiScreen& screen) {
  // Tabs. The selected pill dims to a dither when the selection is down in
  // the list (the legacy focused/unfocused tab distinction).
  // Stack array, not a heap vector: this runs on every render and the tab
  // count is small and fixed.
  constexpr int MAX_TABS = 8;
  const int count = tabCount() < MAX_TABS ? tabCount() : MAX_TABS;
  fui::TabItem tabs[MAX_TABS];
  for (int i = 0; i < count; i++) {
    tabs[i].label = tabLabel(i);
    tabs[i].value = static_cast<int16_t>(i);
    tabs[i].selected = activeTab() == i;
    tabs[i].indicator = tabIndicator(i);
  }
  UiTabBand::Options options;
  options.action = ACTION_TAB;
  options.focused = ringFocus() == TabRing::Focus::TopTabs;
  options.hasTouch = mappedInput.hasTouch();
  options.pillMaxPad = tabPillMaxPad;
  UiTabBand::build(screen, renderer, tabs, count, options);
}
