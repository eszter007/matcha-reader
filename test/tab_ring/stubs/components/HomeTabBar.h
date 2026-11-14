#pragma once

#include <cstdint>

class GfxRenderer;
class MappedInputManager;

enum class HomeTab : uint8_t { Home, Library, Transfer, Stats, Settings, Count };

// Test double for the bottom bar: the test decides whether the bar exists and what a touch hits,
// and reads back which tab was activated.
class HomeTabBar {
 public:
  static constexpr int COUNT = static_cast<int>(HomeTab::Count);
  static inline bool enabledValue = true;
  static inline int hitSlot = -1;
  static inline bool hitTapped = false;
  static inline int activated = -1;

  // Every tab shown, in enum order: slot i is tab i.
  static int count() { return COUNT; }
  static HomeTab tabAt(const int slot) { return static_cast<HomeTab>(slot); }
  static int slotOf(const HomeTab tab) { return static_cast<int>(tab); }
  static bool enabled() { return enabledValue; }
  static int hitTest(const MappedInputManager&, const GfxRenderer&, bool& tapped) {
    tapped = hitTapped;
    return hitSlot;
  }
  static void activate(const HomeTab tab, const HomeTab current) {
    if (tab != current) activated = static_cast<int>(tab);
  }
};
