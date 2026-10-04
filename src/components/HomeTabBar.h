#pragma once

#include <cstdint>

class GfxRenderer;
class MappedInputManager;

// The six destinations the bottom bar switches between, in bar order.
// Files is not here: in this theme the SD browser is one of the Library's tabs, not a
// destination of its own (see LibraryTabs).
enum class HomeTab : uint8_t { Home, Library, Transfer, Stats, Settings, Count };

// Bottom tab bar for the Cover Grid theme: one band, drawn as chrome over a strip each screen
// reserves, shared by the home grid and the five activities the tabs lead to. Deliberately
// renderer-drawn rather than a FreeInkUI component -- three of those screens (cover library,
// reading stats, file transfer) have no UiScreen to hang a component off, and a bar that is
// chrome everywhere cannot drift between the two kinds of screen.
//
// Switching tabs calls ActivityManager::goTo* directly, so it REPLACES the activity rather than
// pushing one: the bar stays, Back keeps whatever meaning it already had, and the stack never
// grows by tabbing around.
class HomeTabBar {
 public:
  static constexpr int COUNT = static_cast<int>(HomeTab::Count);

  // The bar exists only in the Cover Grid theme; every other theme keeps its button hints.
  // This is the single gate: a screen that draws the bar must suppress its hints, and vice versa.
  static bool enabled();

  // Reserved band height. Screens must keep this much free at the bottom of their content, or
  // their last row ends up behind the bar.
  static int height();

  // Top edge of the band in screen coordinates.
  static int top(const GfxRenderer& renderer);

  // Whether a tab screen should still draw the header's back chevron. At a tab's own root the
  // bar already says where you are and where else you can go, so the chevron is noise; one level
  // down (a folder inside Files, a shelf inside Library) it still means "up".
  static bool showsBackButton(bool atTabRoot);

  // Extra height a page-size calculation must give up on top of the button hints it already
  // reserves, so a screen that counts rows lands on the same floor as the bar it draws.
  static int extraPageReserve();

  // What a tab screen must leave free at the bottom: the bar when it is drawn, otherwise the
  // button hints it replaces (already 0 on touch boards). One place, so a screen cannot reserve
  // for one and draw the other.
  static int bottomInset();

  // active: the tab whose screen this is (filled icon). focused: the slot a button board's
  // cursor sits on, or -1 when the cursor is elsewhere on the screen.
  static void draw(const GfxRenderer& renderer, HomeTab active, int focused = -1);

  // Slot under a touch. Returns -1 for none; `tapped` distinguishes a released tap (act on it)
  // from a finger still down (highlight only).
  static int hitTest(const MappedInputManager& input, const GfxRenderer& renderer, bool& tapped);

  // What a routed input pass did, so the caller can request a redraw for a focus move without
  // the bar needing to know how that screen renders.
  // Exited: Confirm landed on the tab you are already in, so the band hands the cursor back to
  // the host instead of rebuilding the same screen.
  enum class Input : uint8_t { None, Consumed, FocusMoved, Exited };

  // The whole input contract in one place: a tap switches tabs, Left/Right walk the focus on
  // button boards, Confirm on a focused slot switches. `focus` is the caller's cursor slot
  // (-1 = elsewhere on the screen) and is updated in place. Pass allowButtons = false on a
  // screen where Left/Right already mean something (reading stats steps the month).
  static Input route(const MappedInputManager& input, const GfxRenderer& renderer, HomeTab current, int& focus,
                     bool allowButtons = true);

  // Replace the current activity with the tab's screen. Re-entering the tab you are already in
  // is a no-op, so tapping it does not tear down and rebuild the screen under your finger.
  static void activate(HomeTab tab, HomeTab current);
};
