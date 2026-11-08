#include "PanelInput.h"

#include "HapticFeedback.h"
#include "MappedInputManager.h"
#include "NestedLookupHost.h"
#include "PanelTouch.h"
#include "ReaderUtils.h"

namespace PanelInput {

Result read(const GfxRenderer& renderer, const MappedInputManager& input, NestedLookupHost* host, const bool addButton,
            const bool longPress, const bool pageTurnReversed) {
  Result r;
  if (host) {
    r.redraw = host->noticeExpired();
    if (host->cursorActive()) {
      const auto cursor = host->handleCursorInput(input);
      if (cursor != NestedLookupHost::CursorInput::None) r.redraw = true;
      if (cursor == NestedLookupHost::CursorInput::Picked) {
        r.event = Event::CursorPick;
        r.tag = host->pickedTag();
      }
      return r;
    }
  }
  const auto event = [&r](const Event e, const int direction = 0) {
    r.event = e;
    r.direction = direction;
    return r;
  };
  if (ReaderUtils::wordLookupPowerClick(input)) return event(Event::CloseAll);
  if (input.wasReleased(MappedInputManager::Button::Back)) {
    return event(input.getHeldTime() >= ReaderUtils::GO_BACK_OR_HOME_MS ? Event::LongBack : Event::Back);
  }
  if (ReaderUtils::wordLookupSideToggle(input)) return event(Event::SideToggle);
  if (input.wasReleased(MappedInputManager::Button::Confirm)) {
    return event(NestedLookupHost::isConfirmHold(input) ? Event::StartCursor : Event::Confirm);
  }
  if (longPress && input.wasScreenLongPress(r.x, r.y)) return event(Event::LongPress);
  const auto touchAction = PanelTouch::read(renderer, input, addButton, pageTurnReversed);
  // Every panel answers a swipe that reads on or skips with the same tick.
  if (touchAction == PanelTouch::Action::ScrollDown || touchAction == PanelTouch::Action::ScrollUp ||
      touchAction == PanelTouch::Action::Next || touchAction == PanelTouch::Action::Previous) {
    haptic_feedback::touchAction();
  }
  switch (touchAction) {
    case PanelTouch::Action::Close:
      return event(Event::CloseAll);
    case PanelTouch::Action::AddButton:
      return event(Event::Confirm);
    case PanelTouch::Action::ScrollDown:
      return event(Event::ReadOn, 1);
    case PanelTouch::Action::ScrollUp:
      return event(Event::ReadOn, -1);
    case PanelTouch::Action::Next:
      return event(Event::Skip, 1);
    case PanelTouch::Action::Previous:
      return event(Event::Skip, -1);
    case PanelTouch::Action::None:
      break;
  }
  // Remapped page bindings (side buttons, the power button) skip, like a hold.
  const auto step = ReaderUtils::lookupPanelStep(input);
  if (step != ReaderUtils::PanelStep::None) return event(Event::Skip, step == ReaderUtils::PanelStep::Next ? 1 : -1);
  int delta = 0;
  switch (ReaderUtils::lookupPressAny(input, delta)) {
    case ReaderUtils::LookupPress::Press:
      return event(Event::ReadOn, delta);
    case ReaderUtils::LookupPress::Hold:
      return event(Event::Skip, delta);
    case ReaderUtils::LookupPress::None:
      break;
  }
  return r;
}

}  // namespace PanelInput
