#pragma once

#include <GfxRenderer.h>

#include "MappedInputManager.h"
#include "activities/ActivityResult.h"
#include "clippings/SelectionGeometry.h"
#include "components/DictionaryPanel.h"
#include "components/UITheme.h"

// The two selection handles on the word a lookup panel is showing. Dragging either one turns the
// lookup into a clip selection of that word (ClipStartResult), so clipping starts from the same
// long press that looks a word up. Touch boards only; geometry is shared with clip selection.
struct LookupClipHandles {
  Rect first{};  // the word's first box on screen (a column-wrapped match has a second)
  Rect last{};   // its last box; equal to first for a one-box word
  bool vertical = false;
  bool rtl = false;

  bool valid() const { return first.width > 0 && first.height > 0; }

  void set(const Rect firstBox, const Rect lastBox, const bool isVertical, const bool isRtl = false) {
    first = firstBox;
    last = lastBox;
    vertical = isVertical;
    rtl = isRtl;
  }

  Rect handle(const GfxRenderer& renderer, const bool start) const {
    return selectionGeometry::handle(start ? first : last, start, vertical, rtl,
                                     UITheme::getInstance().getScreenSafeArea(renderer, true, false),
                                     BaseTheme::selectionHandleSize());
  }

  // The word and both handles, for DictionaryPanel::setAvoid.
  Rect band(const GfxRenderer& renderer) const {
    if (!valid()) return Rect{};
    const Rect a = handle(renderer, true);
    const Rect b = handle(renderer, false);
    const int top = std::min({first.y, last.y, a.y, b.y});
    const int bottom = std::max({first.y + first.height, last.y + last.height, a.y + a.height, b.y + b.height});
    return Rect{0, top, renderer.getScreenWidth(), bottom - top};
  }

  // Drawn before the panel card, so a handle the card overlaps stays under it.
  void draw(const GfxRenderer& renderer) const {
    if (!valid()) return;
    for (const bool start : {true, false}) {
      BaseTheme::drawSelectionHandle(renderer, handle(renderer, start),
                                     selectionGeometry::handleCorner(start, vertical, rtl));
    }
  }

  // A touch-down on a visible handle (one outside the panel card).
  bool pressed(const GfxRenderer& renderer, const MappedInputManager& input, ClipStartResult& out) const {
    int x = 0;
    int y = 0;
    if (!valid() || !input.hasTouch() || !input.wasScreenTouchPressed(x, y)) return false;
    if (selectionGeometry::contains(DictionaryPanel::compute(renderer).box, x, y)) return false;
    for (const bool start : {true, false}) {
      if (!selectionGeometry::contains(handle(renderer, start), x, y)) continue;
      out.wordX = static_cast<int16_t>(first.x + first.width / 2);
      out.wordY = static_cast<int16_t>(first.y + first.height / 2);
      out.touchX = static_cast<int16_t>(x);
      out.touchY = static_cast<int16_t>(y);
      return true;
    }
    return false;
  }
};
