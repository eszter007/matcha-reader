#pragma once

#include <vector>

#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "components/themes/BaseTheme.h"

// The Library's tab band, shared by the two activities that draw it. In the Cover Grid theme it
// carries a third tab, Files, which is the SD browser: the browser is one of the library's views
// there rather than its own destination in the bottom bar. Selecting it switches activity -- the
// browser keeps its own screen, navigation and long-press actions -- and the browser draws this
// same band with Files marked, so the switch reads as a tab change.
//
// Every other theme keeps the two tabs it always had; Browse Files stays a separate entry on
// their home menu.
namespace LibraryTabs {

enum Tab : int { Books = 0, Shelves = 1, Files = 2 };

// 3 in the Cover Grid theme, 2 elsewhere.
int count();

// Tab band for `active`; pass Files from the browser.
std::vector<TabInfo> build(int active);

// Where the band sits, under the header.
Rect barRect(const GfxRenderer& renderer, const MappedInputManager& input);

// Height the band occupies, 0 when there is none to draw.
int height(const MappedInputManager& input);

// Tab under a tap, or -1. `y` outside the band always misses.
int hitTest(const GfxRenderer& renderer, const MappedInputManager& input, int x, int y, int active);

// Switch to the activity that owns `tab`. A no-op for the tab already showing.
void activate(int tab);

}  // namespace LibraryTabs
