#pragma once

#include <vector>

#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "components/UiAppHost.h"
#include "components/UiTabBand.h"
#include "components/themes/BaseTheme.h"

// The Library's tab band, shared by the activities that draw it. In the Cover Grid theme it
// carries two more tabs, OPDS (the server list) and Files (the SD browser): each is one of the
// library's views there rather than its own destination in the bottom bar. Selecting one switches
// activity -- each keeps its own screen, navigation and long-press actions -- and draws this same
// band with its tab marked, so the switch reads as a tab change.
//
// Every other theme keeps the two tabs it always had; Browse Files and the OPDS browser stay
// separate entries on their home menu.
namespace LibraryTabs {

enum Tab : int { Books = 0, Shelves = 1, Opds = 2, Files = 3 };

// 4 in the Cover Grid theme, 2 elsewhere.
int count();

// The band's tabs with `active` marked, for UiTabBand; returns how many. `tabs` holds MAX_TABS.
constexpr int MAX_TABS = 4;
int bandItems(freeink::ui::TabItem* tabs, int active);
UiTabBand::Options bandOptions(bool focused, bool hasTouch, freeink::ui::ActionId action = 0);

// The band as a UiTabBand, at the top of the screen's remaining content. Taps dispatch `action`
// with the tab's index.
void buildBand(UiAppHost::UiScreen& screen, const GfxRenderer& renderer, int active, bool focused, bool hasTouch,
               freeink::ui::ActionId action);

// Switch to the activity that owns `tab`. A no-op for the tab already showing.
void activate(int tab);

}  // namespace LibraryTabs
