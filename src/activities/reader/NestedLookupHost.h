#pragma once

#include <I18n.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "DefinitionTextRenderer.h"
#include "activities/ActivityResult.h"

class GfxRenderer;
class MappedInputManager;

// What every dictionary panel needs to look up a word inside its definition: a word cursor for
// buttons, a short notice popup, where it last drew a DefinitionText, and the rule for what a
// closed nested panel means for this one. Only an activity can open another, so the panel itself
// starts the next panel and calls chainResult() from its result callback.
class NestedLookupHost {
 public:
  enum class CursorInput : uint8_t { None, Redraw, Picked };

  // Where render drew the definition (DefinitionText panels). Written by render, read under RenderLock.
  DefinitionText::WrapGeometry drawnText;

  // Hold Confirm starts the cursor; the release that started it is not a pick.
  static bool isConfirmHold(const MappedInputManager& mappedInput);

  // Starts the cursor on `boxes`; a box's offset is the caller's tag for it. False when empty.
  bool startCursor(std::vector<DefinitionText::CharBox>&& boxes);
  // Starts it on the CJK characters of `text` as drawn at drawnText.
  bool startCjkCursor(const GfxRenderer& renderer, const std::string& text);
  bool cursorActive() const { return index >= 0; }
  // One frame of input while cursorActive(): page keys step, Confirm picks, Back leaves.
  CursorInput handleCursorInput(const MappedInputManager& mappedInput);
  // The tag of the box Confirm picked (after CursorInput::Picked).
  uint32_t pickedTag() const { return picked; }
  void drawCursor(const GfxRenderer& renderer) const;

  void showNotice(StrId message);
  // True on the frame the notice ran out: the panel redraws without it.
  bool noticeExpired();
  // The notice while it shows, else `fallback`.
  const char* popupText(const char* fallback) const;

  // For a panel's result callback: nullopt when the nested panel closed by a plain Back (stay
  // open), else the result this panel finishes with. `nestedLevel`: this panel is itself nested,
  // so a long Back passes on down the chain; the first panel turns it into a plain cancel.
  static std::optional<ActivityResult> chainResult(const ActivityResult& result, bool nestedLevel);

 private:
  void stopCursor();
  std::vector<DefinitionText::CharBox> boxes;
  int index = -1;
  uint32_t picked = 0;
  StrId notice = StrId::STR_NO_MATCH;
  unsigned long noticeUntil = 0;  // millis() the notice shows until; 0 = none
};
