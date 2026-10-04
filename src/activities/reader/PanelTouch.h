#pragma once

#include "HapticFeedback.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "components/DictionaryPanel.h"

// Touch input shared by the floating panels (dictionary definition, footnotes, translation), so
// each answers the same gestures: a tap outside the card puts it away, a tap on its corner
// button saves, up/down swipes scroll whatever the page-turn setting says, and page turns follow
// the reader's own touch setting (tap zones, inverted zones, swipes, or nothing).
namespace PanelTouch {

// ScrollDown/ScrollUp are the vertical swipes, apart from Next/Previous (the reader's page-turn
// gesture) for a panel that scrolls within an entry and pages between entries.
enum class Action : uint8_t { None, Close, AddButton, Next, Previous, ScrollDown, ScrollUp };

inline Action read(const GfxRenderer& renderer, const MappedInputManager& input, const bool hasAddButton = false) {
  // Checked before paging so a tap outside and a page turn cannot both claim one contact.
  int tx = 0;
  int ty = 0;
  if (input.wasScreenTapped(tx, ty)) {
    const auto layout = DictionaryPanel::compute(renderer);
    const auto& box = layout.box;
    if (tx < box.x || tx >= box.x + box.width || ty < box.y || ty >= box.y + box.height) {
      haptic_feedback::touchAction();
      return Action::Close;
    }
    const auto& add = layout.addButton;
    if (hasAddButton && tx >= add.x && tx < add.x + add.width && ty >= add.y && ty < add.y + add.height) {
      haptic_feedback::touchAction();
      return Action::AddButton;
    }
  }
  if (const int scroll = ReaderUtils::definitionScrollSwipe(input)) {
    return scroll > 0 ? Action::ScrollDown : Action::ScrollUp;
  }
  const auto turn = ReaderUtils::detectTouchPageTurn(renderer, input);
  if (turn.next) return Action::Next;
  if (turn.prev) return Action::Previous;
  return Action::None;
}

}  // namespace PanelTouch
