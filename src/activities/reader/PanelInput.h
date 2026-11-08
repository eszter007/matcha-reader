#pragma once

#include <cstdint>

class GfxRenderer;
class MappedInputManager;
class NestedLookupHost;

// The one input model every floating panel follows (dictionary entries, footnotes, page
// translation): a press reads on, a hold skips to the neighbouring item, and touch swipes do the
// same; Confirm is the panel's own action and holding it starts the word cursor; Back, the power
// click, the side toggle and a tap outside close it. read() decodes one frame into one event, in
// one fixed order, so every panel answers the same gesture the same way. What an event means
// (which item is next, what Confirm saves) stays with the panel.
namespace PanelInput {

enum class Event : uint8_t {
  None,
  ReadOn,       // direction: a page, and past the last page the next item
  Skip,         // direction: straight to the neighbouring item
  Confirm,      // Confirm pressed, or the + button tapped
  StartCursor,  // Confirm held
  CursorPick,   // the word cursor picked the box tagged `tag`
  LongPress,    // a long press at x, y
  Back,
  LongBack,    // Back held: leave a lookup chain altogether
  CloseAll,    // the power click or a tap outside: back to the book
  SideToggle,  // the side button bound to Word Lookup
};

struct Result {
  Event event = Event::None;
  int direction = 0;  // +1 forward, -1 back (ReadOn, Skip)
  int x = 0;
  int y = 0;
  uint32_t tag = 0;
  bool redraw = false;  // the cursor moved or a notice ran out: redraw even with no event
};

// `host` (may be null) owns the word cursor and the notice popup; while its cursor is up it takes
// every key. `addButton`: the panel shows the + button, so a tap on it is Confirm. `longPress`: the
// panel looks words up on a long press. Only then is it read, since reading one ends the contact.
Result read(const GfxRenderer& renderer, const MappedInputManager& input, NestedLookupHost* host, bool addButton,
            bool longPress, bool pageTurnReversed);

}  // namespace PanelInput
