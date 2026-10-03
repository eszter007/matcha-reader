#pragma once

#include <cstdint>

#include "components/HomeTabBar.h"

class GfxRenderer;
class MappedInputManager;

// The one button ring every tab screen walks: the screen's own top tabs, its content, then the
// bottom bar, and round again. Each screen keeps its own record of where the cursor is (a ring
// index, a band flag, a grid index) and exposes it through Host; every decision about moving
// between the three parts is made here, so the screens cannot drift apart again.
//
// The ring:
//   Next:     top tabs -> content (first) -> ... -> content (last) -> bottom bar -> top tabs
//   Confirm on the top tabs: the next tab; past the last one, the bottom bar
//   Confirm on the bottom bar's own tab: the FIRST top tab, with the top tabs focused
// A screen without top tabs or without the bar simply skips that stop.
class TabRing {
 public:
  enum class Focus : uint8_t { TopTabs, Content, BottomBar };

  class Host {
   public:
    // Number of top tabs (0: the screen has none), the one showing, and switching to another.
    // ringSelectTopTab() may replace the activity (the Library's views are separate screens).
    virtual int ringTopTabCount() const = 0;
    virtual int ringActiveTopTab() const = 0;
    virtual void ringSelectTopTab(int index) = 0;
    // The bottom bar slot that is this screen; HomeTab::Count when it draws no bar.
    virtual HomeTab ringBottomTab() const = 0;
    // A tap or Confirm on a bottom tab. The default switches screen and ignores the tab this
    // screen already is; overridden where leaving needs bookkeeping first.
    virtual void ringActivateBottomTab(HomeTab tab);
    // False when the content has nothing a cursor can stop on (an empty list, an error).
    virtual bool ringHasContent() const { return true; }
    // Whether Left/Right in the content move into the bottom bar (lists), or mean something on
    // the page itself (Insights steps the month).
    virtual bool ringContentLeftRightToBar() const { return true; }
    // Whether Left/Right on the top tabs step the tab. False where the screen binds them itself
    // (the Library list opens search with Left there).
    virtual bool ringTopTabsLeftRightStep() const { return true; }
    // Where the cursor is, read from and written to the screen's own state. Setting Content puts
    // the cursor on the first item, or the last when atEnd. Setting BottomBar must leave
    // ringBarSlot() on this screen's own slot; every other focus must leave it at -1.
    virtual Focus ringFocus() const = 0;
    virtual void ringSetFocus(Focus focus, bool atEnd) = 0;
    // The bar slot under the cursor, -1 while it is elsewhere.
    virtual int& ringBarSlot() = 0;
    // Something changed that needs drawing.
    virtual void ringChanged() = 0;

   protected:
    ~Host() = default;
  };

  // What handleInput() did. BarStepped: only the bar's cursor moved (screens that can repaint the
  // band alone use it).
  enum class Result : uint8_t { None, Handled, BarStepped };

  // Bottom-bar touch, and Left/Right/Confirm while the cursor is on either band (or Left/Right in
  // the content, where the host lets them reach the bar). Call before the screen's own keys. Up
  // and Down are not taken here: the screen's navigator owns them so its holds keep working, and
  // routes them through step() and leaveContent().
  static Result handleInput(Host& host, const MappedInputManager& input, const GfxRenderer& renderer);

  // Next (+1) or Previous (-1) with the cursor on a band.
  static void step(Host& host, int direction);
  // The content cursor ran off its last item (+1) or its first (-1).
  static void leaveContent(Host& host, int direction);
  // Confirm on the top tabs.
  static void confirmTopTabs(Host& host);
  // Confirm on the bar's own slot.
  static void leaveBottomBar(Host& host);

  static bool hasBottomBar(const Host& host);
  static bool hasTopTabs(const Host& host) { return host.ringTopTabCount() > 0; }

 private:
  static void focus(Host& host, Focus target, bool atEnd);
};
