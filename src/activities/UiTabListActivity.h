#pragma once

#include <vector>

#include "activities/UiListActivity.h"

// UiListActivity variant for screens with a tab band above the list (Settings,
// Text Settings). Navigation is a ring: position 0 is the tab bar, 1..N are
// the list rows, so props.selectedIndex = ring - 1 (-1 = tab band focused).
// Each tab owns its own ListNav (selection + viewport memory); activeNav()
// redirects the whole UiListActivity protocol (touch routing, swipe scroll,
// screen sync) to the active tab's state. Button navigation walks the ring on
// release and steps the TAB on continuous hold. The tab-bar chrome (pill
// styles, focused band wash) is shared verbatim via buildTabBar().
//
// Subclasses own the button semantics wholesale (handleButtons is pure here:
// the two existing tab screens differ on press-vs-release and what Back does)
// plus what activating a row or tapping a tab means.
class UiTabListActivity : public UiListActivity {
 public:
  void onEnter() override;

 protected:
  // Subclass actions start here (the base owns ACTION_ROW and ACTION_TAB).
  static constexpr freeink::ui::ActionId ACTION_TAB_USER = ACTION_USER;

  UiTabListActivity(const char* name, GfxRenderer& renderer, MappedInputManager& mappedInput,
                    bool wantsTouchLongPress = false);

  // --- subclass contract (in addition to UiListActivity's) ------------------
  virtual int tabCount() const = 0;
  virtual int activeTab() const = 0;
  virtual const char* tabLabel(int index) const = 0;
  virtual freeink::ui::TabIndicator tabIndicator(int) const { return freeink::ui::TabIndicator::None; }
  // Touch tap on a tab pill (bounds already checked).
  void onTabAction(int index) override = 0;
  // Advance the active tab by direction (continuous-hold navigation; also what
  // Confirm on the tab bar should do). Subclass owns wrap and any per-switch
  // state reset, and requests the update.
  virtual void stepTab(int direction) = 0;
  // The two tab screens disagree on press-vs-release and Back semantics, so
  // there is no shared default.
  bool handleButtons() override = 0;

  // --- ring plumbing ---------------------------------------------------------
  freeink::ui::ListNav& activeNav() override;
  // Ring position of the active tab (0 = tab bar) for const contexts.
  int ringPos() const;
  // ACTION_ROW lands as ring = row + 1, then activateIndex(row).
  void onRowAction(const freeink::ui::ActionEvent& event) override;
  // Release walks the ring; continuous hold steps the tab.
  void navigateButtons() override;
  // Move to a ring position: tab bar rewinds the viewport, a row pulls the
  // viewport to itself.
  void moveRingTo(int ringIndex);

  // --- screen helpers --------------------------------------------------------
  // The shared tab band (UiTabBand) for this screen's tabs.
  // TabRing::Host: the tab band is ring position 0 here, the rows 1..N.
  int ringTopTabCount() const override { return tabCount(); }
  int ringActiveTopTab() const override { return activeTab(); }
  void ringSelectTopTab(int index) override;
  // A hold that follows must not restore the snapshot of some earlier press.
  void onRingInputConsumed() override {
    holdStart_.pending = false;
    navigationStartedOnTabs = false;
  }
  TabRing::Focus ringFocus() const override;
  void ringSetFocus(TabRing::Focus focus, bool atEnd) override;
  void buildTabBar(UiScreen& screen);
  // Ring-aware counterpart of syncListViewport: measures rows, applies the
  // one-shot follow to the remembered row, clamps, and writes
  // props.selectedIndex = ring - 1.
  void syncTabListViewport(UiScreen& screen, freeink::ui::ListProps& props);

  // Per-tab selection/viewport state, sized in onEnter. Protected so subclass
  // tab-switch code can seed the target tab's ring/viewport.
  std::vector<freeink::ui::ListNav> tabNavs;
  // Set on each Next/Previous press: whether the cursor was on the tab band when it began. A
  // subclass that treats the band specially while a key is held (the Library) reads it.
  bool navigationStartedOnTabs = false;

  // When > 0, each tab pill is capped at its label width plus this padding
  // per side, centered in its unchanged equal-width slot. With few tabs the
  // default full-slot pill stretches across a third of the screen; screens
  // with more tabs (Settings) keep the default of 0 (fill the slot).
  int16_t tabPillMaxPad = 0;

 private:
  // Where a press found the cursor. A press steps the ring at once, but a hold steps the TAB, so
  // by the time the hold is recognised the press has already moved one step: the first repeat
  // puts the cursor back here before stepping the tab. `pending` is cleared once restored.
  struct HoldStart {
    int selected = 0;
    int tabFocus = -1;
    bool pending = false;
  } holdStart_;
  void restoreHoldStart();
};
