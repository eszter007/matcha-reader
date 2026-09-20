#include <gtest/gtest.h>

#include <cstdint>

#include "util/SideButtonActions.h"

TEST(SideButtonActions, CustomizedOnlyWhenLeavingDefault) {
  using A = SideButtonAction;
  const auto d = static_cast<uint8_t>(A::Default);
  EXPECT_FALSE(side_button::customized(d, d));
  EXPECT_TRUE(side_button::customized(static_cast<uint8_t>(A::Sleep), d));
  EXPECT_TRUE(side_button::customized(d, static_cast<uint8_t>(A::NextPage)));
  EXPECT_TRUE(side_button::customized(static_cast<uint8_t>(A::PrevPage), static_cast<uint8_t>(A::WordLookup)));
}

// A setting that also governs the front pair or touch must not be hidden just because ONE side
// button moved off Default -- only "which shared side role goes where" truly dies, and only once
// both buttons are gone.
TEST(SideButtonActions, FullyCustomizedNeedsBothButtons) {
  using A = SideButtonAction;
  const auto d = static_cast<uint8_t>(A::Default);
  const auto n = static_cast<uint8_t>(A::NextPage);
  const auto off = static_cast<uint8_t>(A::None);
  EXPECT_FALSE(side_button::fullyCustomized(d, d));
  EXPECT_FALSE(side_button::fullyCustomized(n, d));
  EXPECT_FALSE(side_button::fullyCustomized(d, n));
  EXPECT_TRUE(side_button::fullyCustomized(n, off));
}

TEST(SideButtonActions, ClampKeepsRangeAndDefaultsOverflow) {
  using A = SideButtonAction;
  for (uint8_t v = 0; v < static_cast<uint8_t>(A::Count); ++v) {
    EXPECT_EQ(side_button::clampAction(v), v);
  }
  EXPECT_EQ(side_button::clampAction(static_cast<uint8_t>(A::Count)), static_cast<uint8_t>(A::Default));
  // "Off" is a custom action like any other: it must suppress the shared roles.
  EXPECT_TRUE(side_button::customized(static_cast<uint8_t>(A::None), static_cast<uint8_t>(A::Default)));
  EXPECT_EQ(side_button::clampAction(255), static_cast<uint8_t>(A::Default));
}

TEST(SideButtonActions, OptionOrderMatchesPersistedContract) {
  // SettingsList persists these by index; reordering silently remaps saves.
  EXPECT_EQ(static_cast<uint8_t>(SideButtonAction::Default), 0);
  EXPECT_EQ(static_cast<uint8_t>(SideButtonAction::Sleep), 1);
  EXPECT_EQ(static_cast<uint8_t>(SideButtonAction::PrevPage), 2);
  EXPECT_EQ(static_cast<uint8_t>(SideButtonAction::NextPage), 3);
  EXPECT_EQ(static_cast<uint8_t>(SideButtonAction::Refresh), 4);
  EXPECT_EQ(static_cast<uint8_t>(SideButtonAction::Footnotes), 5);
  EXPECT_EQ(static_cast<uint8_t>(SideButtonAction::WordLookup), 6);
  EXPECT_EQ(static_cast<uint8_t>(SideButtonAction::None), 7);
  EXPECT_EQ(static_cast<uint8_t>(SideButtonAction::Count), 8);
}

TEST(SideButtonActions, LoneReleaseNeedsQuietFrontButtons) {
  // No recent front activity: a lone side release counts as deliberate.
  EXPECT_TRUE(side_button::loneRelease(1000, 0));
  EXPECT_TRUE(side_button::loneRelease(1000, 849));
  // At or inside the window: treated as a ghost of front activity.
  EXPECT_FALSE(side_button::loneRelease(1000, 850));
  EXPECT_FALSE(side_button::loneRelease(1000, 999));
  EXPECT_FALSE(side_button::loneRelease(1000, 1000));
  // Unsigned wrap of the millisecond clock stays correct.
  EXPECT_TRUE(side_button::loneRelease(50, 0xFFFFFF00UL));
  EXPECT_FALSE(side_button::loneRelease(0xFFFFFF80UL, 0xFFFFFF00UL));
}
