#include "TabRing.h"

#include "MappedInputManager.h"

void TabRing::Host::ringActivateBottomTab(const HomeTab tab) { HomeTabBar::activate(tab, ringBottomTab()); }

bool TabRing::hasBottomBar(const Host& host) { return host.ringBottomTab() != HomeTab::Count && HomeTabBar::enabled(); }

void TabRing::focus(Host& host, const Focus target, const bool atEnd) {
  host.ringSetFocus(target, atEnd);
  host.ringChanged();
}

// The stops in ring order, skipping any the screen does not have. Content without anything to
// stop on is skipped too, so an empty list does not swallow a key press.
void TabRing::step(Host& host, const int direction) {
  const bool tabs = hasTopTabs(host);
  const bool bar = hasBottomBar(host);
  const bool content = host.ringHasContent();
  const Focus from = host.ringFocus();
  if (direction > 0) {
    if (from == Focus::TopTabs) {
      if (content) return focus(host, Focus::Content, false);
      if (bar) return focus(host, Focus::BottomBar, false);
      return;
    }
    if (from == Focus::BottomBar) {
      if (tabs) return focus(host, Focus::TopTabs, false);
      if (content) return focus(host, Focus::Content, false);
      return;
    }
    return leaveContent(host, direction);
  }
  if (from == Focus::BottomBar) {
    if (content) return focus(host, Focus::Content, true);
    if (tabs) return focus(host, Focus::TopTabs, false);
    return;
  }
  if (from == Focus::TopTabs) {
    if (bar) return focus(host, Focus::BottomBar, false);
    if (content) return focus(host, Focus::Content, true);
    return;
  }
  leaveContent(host, direction);
}

void TabRing::leaveContent(Host& host, const int direction) {
  const bool tabs = hasTopTabs(host);
  const bool bar = hasBottomBar(host);
  if (direction > 0) {
    if (bar) return focus(host, Focus::BottomBar, false);
    if (tabs) return focus(host, Focus::TopTabs, false);
    return focus(host, Focus::Content, false);  // nothing else on the ring: wrap
  }
  if (tabs) return focus(host, Focus::TopTabs, false);
  if (bar) return focus(host, Focus::BottomBar, false);
  focus(host, Focus::Content, true);
}

void TabRing::confirmTopTabs(Host& host) {
  const int count = host.ringTopTabCount();
  if (count <= 0) return;
  const int active = host.ringActiveTopTab();
  if (active < count - 1) {
    host.ringSelectTopTab(active + 1);
    host.ringChanged();
    return;
  }
  // Past the last tab the ring carries on into the bar; without one it wraps to the first tab.
  if (hasBottomBar(host)) return focus(host, Focus::BottomBar, false);
  if (active != 0) host.ringSelectTopTab(0);
  host.ringChanged();
}

void TabRing::leaveBottomBar(Host& host) {
  // Back to the top of the ring: the first tab, not the one the cursor last left. Selecting may
  // switch to another screen (the Library's first view), which then opens on its tab band.
  if (hasTopTabs(host)) {
    if (host.ringActiveTopTab() != 0) host.ringSelectTopTab(0);
    return focus(host, Focus::TopTabs, false);
  }
  focus(host, host.ringHasContent() ? Focus::Content : Focus::BottomBar, false);
}

TabRing::Result TabRing::handleInput(Host& host, const MappedInputManager& input, const GfxRenderer& renderer) {
  using Button = MappedInputManager::Button;
  const bool bar = hasBottomBar(host);
  int& slot = host.ringBarSlot();

  if (bar) {
    bool tapped = false;
    const int hit = HomeTabBar::hitTest(input, renderer, tapped);
    if (hit >= 0) {
      if (tapped) host.ringActivateBottomTab(static_cast<HomeTab>(hit));
      return Result::Handled;
    }
  }

  const Focus at = host.ringFocus();
  // Left/Right are taken on the press: they double as Previous/Next, which the screens act on at
  // the press, so a release here would come after the cursor had already left the band.
  const bool left = input.wasPressed(Button::Left);
  const bool right = input.wasPressed(Button::Right);

  if (at == Focus::TopTabs) {
    const int count = host.ringTopTabCount();
    if ((left || right) && count > 1 && host.ringTopTabsLeftRightStep()) {
      host.ringSelectTopTab((host.ringActiveTopTab() + (right ? 1 : count - 1)) % count);
      host.ringChanged();
      return Result::Handled;
    }
    if (input.wasReleased(Button::Confirm)) {
      confirmTopTabs(host);
      return Result::Handled;
    }
    return Result::None;
  }

  if (!bar) return Result::None;

  if (at == Focus::Content && !(host.ringContentLeftRightToBar() && (left || right))) return Result::None;

  // A Left/Right from the content steps from the tab you are in, so the cursor lands next to
  // where you already are rather than at an end.
  const int own = static_cast<int>(host.ringBottomTab());
  if (left || right) {
    if (at != Focus::BottomBar) host.ringSetFocus(Focus::BottomBar, false);
    slot = (slot + (right ? 1 : HomeTabBar::COUNT - 1)) % HomeTabBar::COUNT;
    host.ringChanged();
    return Result::BarStepped;
  }
  if (input.wasReleased(Button::Confirm)) {
    if (slot == own) {
      leaveBottomBar(host);
    } else {
      host.ringActivateBottomTab(static_cast<HomeTab>(slot));
    }
    return Result::Handled;
  }
  return Result::None;
}
