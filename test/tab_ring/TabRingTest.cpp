#include <gtest/gtest.h>

#include "GfxRenderer.h"
#include "HapticFeedback.h"
#include "MappedInputManager.h"
#include "components/TabRing.h"

namespace {
using Button = MappedInputManager::Button;
using Focus = TabRing::Focus;

constexpr int OWN_SLOT = static_cast<int>(HomeTab::Library);

// A screen with `tabs` top tabs, `items` content positions and (optionally) the bottom bar.
struct MockHost : TabRing::Host {
  int tabs = 3;
  int activeTab = 0;
  int items = 4;
  bool hasBar = true;
  bool contentLeftRightToBar = true;
  bool topTabsLeftRightStep = true;

  Focus focusNow = Focus::TopTabs;
  int contentPos = 0;
  int barSlot = -1;
  int changes = 0;

  int ringTopTabCount() const override { return tabs; }
  int ringActiveTopTab() const override { return activeTab; }
  void ringSelectTopTab(const int index) override { activeTab = index; }
  HomeTab ringBottomTab() const override { return hasBar ? HomeTab::Library : HomeTab::Count; }
  bool ringHasContent() const override { return items > 0; }
  bool ringContentLeftRightToBar() const override { return contentLeftRightToBar; }
  bool ringTopTabsLeftRightStep() const override { return topTabsLeftRightStep; }
  Focus ringFocus() const override { return focusNow; }
  void ringSetFocus(const Focus focus, const bool atEnd) override {
    focusNow = focus;
    barSlot = focus == Focus::BottomBar ? OWN_SLOT : -1;
    if (focus == Focus::Content) contentPos = atEnd ? items - 1 : 0;
  }
  int& ringBarSlot() override { return barSlot; }
  void ringChanged() override { changes++; }
};

class TabRingTest : public ::testing::Test {
 protected:
  MockHost host;
  MappedInputManager input;
  GfxRenderer renderer;

  void SetUp() override {
    HomeTabBar::enabledValue = true;
    HomeTabBar::hitSlot = -1;
    HomeTabBar::hitTapped = false;
    HomeTabBar::activated = -1;
    haptic_feedback::taps = 0;
  }

  TabRing::Result press(const Button button) {
    input = {};
    input.pressed = static_cast<uint8_t>(1u << static_cast<unsigned>(button));
    return TabRing::handleInput(host, input, renderer);
  }
  TabRing::Result release(const Button button) {
    input = {};
    input.released = static_cast<uint8_t>(1u << static_cast<unsigned>(button));
    return TabRing::handleInput(host, input, renderer);
  }
};

TEST_F(TabRingTest, NextWalksTabsContentBarAndBackToTabs) {
  TabRing::step(host, 1);
  EXPECT_EQ(host.focusNow, Focus::Content);
  EXPECT_EQ(host.contentPos, 0);

  TabRing::leaveContent(host, 1);
  EXPECT_EQ(host.focusNow, Focus::BottomBar);
  EXPECT_EQ(host.barSlot, OWN_SLOT);

  TabRing::step(host, 1);
  EXPECT_EQ(host.focusNow, Focus::TopTabs);
  EXPECT_EQ(host.barSlot, -1);
}

TEST_F(TabRingTest, PreviousWalksTabsBarLastContentAndBackToTabs) {
  TabRing::step(host, -1);
  EXPECT_EQ(host.focusNow, Focus::BottomBar);
  EXPECT_EQ(host.barSlot, OWN_SLOT);

  TabRing::step(host, -1);
  EXPECT_EQ(host.focusNow, Focus::Content);
  EXPECT_EQ(host.contentPos, host.items - 1);
  EXPECT_EQ(host.barSlot, -1);

  TabRing::leaveContent(host, -1);
  EXPECT_EQ(host.focusNow, Focus::TopTabs);
}

TEST_F(TabRingTest, StepFromContentLeavesIt) {
  host.focusNow = Focus::Content;
  TabRing::step(host, 1);
  EXPECT_EQ(host.focusNow, Focus::BottomBar);
  host.focusNow = Focus::Content;
  host.barSlot = -1;
  TabRing::step(host, -1);
  EXPECT_EQ(host.focusNow, Focus::TopTabs);
}

TEST_F(TabRingTest, EmptyContentIsSkippedInBothDirections) {
  host.items = 0;
  TabRing::step(host, 1);
  EXPECT_EQ(host.focusNow, Focus::BottomBar);
  TabRing::step(host, 1);
  EXPECT_EQ(host.focusNow, Focus::TopTabs);
  TabRing::step(host, -1);
  EXPECT_EQ(host.focusNow, Focus::BottomBar);
  TabRing::step(host, -1);
  EXPECT_EQ(host.focusNow, Focus::TopTabs);
}

TEST_F(TabRingTest, WithoutTopTabsTheRingIsContentAndBar) {
  host.tabs = 0;
  host.focusNow = Focus::Content;
  TabRing::leaveContent(host, 1);
  EXPECT_EQ(host.focusNow, Focus::BottomBar);
  TabRing::step(host, 1);
  EXPECT_EQ(host.focusNow, Focus::Content);
  EXPECT_EQ(host.contentPos, 0);

  TabRing::leaveContent(host, -1);
  EXPECT_EQ(host.focusNow, Focus::BottomBar);
  TabRing::step(host, -1);
  EXPECT_EQ(host.focusNow, Focus::Content);
  EXPECT_EQ(host.contentPos, host.items - 1);
}

TEST_F(TabRingTest, WithoutBottomBarTheRingIsTabsAndContent) {
  host.hasBar = false;
  host.focusNow = Focus::Content;
  TabRing::leaveContent(host, 1);
  EXPECT_EQ(host.focusNow, Focus::TopTabs);
  TabRing::step(host, -1);
  EXPECT_EQ(host.focusNow, Focus::Content);
  EXPECT_EQ(host.contentPos, host.items - 1);
  EXPECT_EQ(host.barSlot, -1);
}

TEST_F(TabRingTest, ABarTheThemeDoesNotDrawIsNoStop) {
  HomeTabBar::enabledValue = false;
  EXPECT_FALSE(TabRing::hasBottomBar(host));
  host.focusNow = Focus::Content;
  TabRing::leaveContent(host, 1);
  EXPECT_EQ(host.focusNow, Focus::TopTabs);
}

TEST_F(TabRingTest, WithNeitherBandContentWraps) {
  host.tabs = 0;
  host.hasBar = false;
  host.focusNow = Focus::Content;
  host.contentPos = host.items - 1;
  TabRing::leaveContent(host, 1);
  EXPECT_EQ(host.focusNow, Focus::Content);
  EXPECT_EQ(host.contentPos, 0);
  TabRing::leaveContent(host, -1);
  EXPECT_EQ(host.contentPos, host.items - 1);
}

TEST_F(TabRingTest, ConfirmOnTabsStepsThenEntersTheBar) {
  TabRing::confirmTopTabs(host);
  EXPECT_EQ(host.activeTab, 1);
  EXPECT_EQ(host.focusNow, Focus::TopTabs);
  TabRing::confirmTopTabs(host);
  EXPECT_EQ(host.activeTab, 2);
  TabRing::confirmTopTabs(host);
  EXPECT_EQ(host.activeTab, 2);
  EXPECT_EQ(host.focusNow, Focus::BottomBar);
  EXPECT_EQ(host.barSlot, OWN_SLOT);
}

TEST_F(TabRingTest, ConfirmPastTheLastTabWrapsWithoutABar) {
  host.hasBar = false;
  host.activeTab = 2;
  TabRing::confirmTopTabs(host);
  EXPECT_EQ(host.activeTab, 0);
  EXPECT_EQ(host.focusNow, Focus::TopTabs);
}

TEST_F(TabRingTest, LeavingTheBarReturnsToTheFirstTab) {
  host.activeTab = 2;
  host.ringSetFocus(Focus::BottomBar, false);
  TabRing::leaveBottomBar(host);
  EXPECT_EQ(host.activeTab, 0);
  EXPECT_EQ(host.focusNow, Focus::TopTabs);
  EXPECT_EQ(host.barSlot, -1);
}

TEST_F(TabRingTest, LeavingTheBarWithoutTabsGoesToTheFirstItem) {
  host.tabs = 0;
  host.contentPos = 3;
  host.ringSetFocus(Focus::BottomBar, false);
  TabRing::leaveBottomBar(host);
  EXPECT_EQ(host.focusNow, Focus::Content);
  EXPECT_EQ(host.contentPos, 0);
}

TEST_F(TabRingTest, ConfirmOnTheBarsOwnSlotReturnsToTheFirstTab) {
  host.activeTab = 2;
  host.ringSetFocus(Focus::BottomBar, false);
  EXPECT_EQ(release(Button::Confirm), TabRing::Result::Handled);
  EXPECT_EQ(host.activeTab, 0);
  EXPECT_EQ(host.focusNow, Focus::TopTabs);
  EXPECT_EQ(HomeTabBar::activated, -1);
}

TEST_F(TabRingTest, ConfirmOnAnotherBarSlotActivatesIt) {
  host.ringSetFocus(Focus::BottomBar, false);
  EXPECT_EQ(press(Button::ScreenRight), TabRing::Result::BarStepped);
  EXPECT_EQ(host.barSlot, OWN_SLOT + 1);
  EXPECT_EQ(release(Button::Confirm), TabRing::Result::Handled);
  EXPECT_EQ(HomeTabBar::activated, OWN_SLOT + 1);
  EXPECT_EQ(host.focusNow, Focus::BottomBar);
}

TEST_F(TabRingTest, BarStepsWrapAtBothEnds) {
  host.ringSetFocus(Focus::BottomBar, false);
  host.barSlot = 0;
  press(Button::ScreenLeft);
  EXPECT_EQ(host.barSlot, HomeTabBar::COUNT - 1);
  press(Button::ScreenRight);
  EXPECT_EQ(host.barSlot, 0);
}

TEST_F(TabRingTest, ConfirmOnTheTopTabsIsTaken) {
  EXPECT_EQ(release(Button::Confirm), TabRing::Result::Handled);
  EXPECT_EQ(host.activeTab, 1);
}

TEST_F(TabRingTest, LeftRightOnTheTopTabsStepAndWrap) {
  EXPECT_EQ(press(Button::ScreenLeft), TabRing::Result::Handled);
  EXPECT_EQ(host.activeTab, 2);
  EXPECT_EQ(press(Button::ScreenRight), TabRing::Result::Handled);
  EXPECT_EQ(host.activeTab, 0);
  EXPECT_EQ(host.focusNow, Focus::TopTabs);
}

TEST_F(TabRingTest, RawLeftRightAreNotTheTabKeys) {
  EXPECT_EQ(press(Button::Right), TabRing::Result::None);
  EXPECT_EQ(host.activeTab, 0);
}

TEST_F(TabRingTest, AScreenThatBindsLeftRightOnItsTabsKeepsThem) {
  host.topTabsLeftRightStep = false;
  EXPECT_EQ(press(Button::ScreenRight), TabRing::Result::None);
  EXPECT_EQ(host.activeTab, 0);
}

TEST_F(TabRingTest, LeftRightInContentEnterTheBarNextToTheOwnSlot) {
  host.focusNow = Focus::Content;
  EXPECT_EQ(press(Button::ScreenRight), TabRing::Result::BarStepped);
  EXPECT_EQ(host.focusNow, Focus::BottomBar);
  EXPECT_EQ(host.barSlot, OWN_SLOT + 1);
}

TEST_F(TabRingTest, ContentKeepsLeftRightAndConfirmWhenItBindsThem) {
  host.focusNow = Focus::Content;
  host.contentLeftRightToBar = false;
  EXPECT_EQ(press(Button::ScreenRight), TabRing::Result::None);
  EXPECT_EQ(release(Button::Confirm), TabRing::Result::None);
  EXPECT_EQ(host.focusNow, Focus::Content);
  EXPECT_EQ(host.barSlot, -1);
}

TEST_F(TabRingTest, ConfirmInContentIsTheScreens) {
  host.focusNow = Focus::Content;
  EXPECT_EQ(release(Button::Confirm), TabRing::Result::None);
}

TEST_F(TabRingTest, ATapOnTheBarActivatesTheSlot) {
  host.focusNow = Focus::Content;
  HomeTabBar::hitSlot = static_cast<int>(HomeTab::Settings);
  HomeTabBar::hitTapped = true;
  input = {};
  EXPECT_EQ(TabRing::handleInput(host, input, renderer), TabRing::Result::Handled);
  EXPECT_EQ(HomeTabBar::activated, static_cast<int>(HomeTab::Settings));
  EXPECT_EQ(haptic_feedback::taps, 1);
}

// The tab already showing swallows the tap and goes nowhere, so there is nothing to confirm.
TEST_F(TabRingTest, ATapOnTheBarsOwnSlotDoesNotTap) {
  HomeTabBar::hitSlot = static_cast<int>(host.ringBottomTab());
  HomeTabBar::hitTapped = true;
  input = {};
  EXPECT_EQ(TabRing::handleInput(host, input, renderer), TabRing::Result::Handled);
  EXPECT_EQ(HomeTabBar::activated, -1);
  EXPECT_EQ(haptic_feedback::taps, 0);
}

TEST_F(TabRingTest, AFingerStillDownOnTheBarIsSwallowedWithoutActivating) {
  HomeTabBar::hitSlot = static_cast<int>(HomeTab::Settings);
  HomeTabBar::hitTapped = false;
  input = {};
  EXPECT_EQ(TabRing::handleInput(host, input, renderer), TabRing::Result::Handled);
  EXPECT_EQ(HomeTabBar::activated, -1);
  EXPECT_EQ(haptic_feedback::taps, 0);
}
}  // namespace
