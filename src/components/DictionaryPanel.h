#pragma once

#include "components/themes/BaseTheme.h"

class GfxRenderer;

// The boxed overlay every dictionary view draws into: a rounded panel floating over the
// reading surface, with the looked-up word above a divider at the top, the definition in
// the middle, and the dictionary's name along the bottom.
//
// Geometry lives here rather than in the four activities that use it (English definition,
// vertical/horizontal Japanese, manga) so the box keeps identical dimensions wherever it is
// opened from -- the activities differ in what they put INSIDE the body, not around it.
namespace DictionaryPanel {

struct Layout {
  Rect box;   // the panel's outer rectangle
  Rect body;  // text area between the divider and the dictionary-name footer
};

// The panel's rectangles for the current orientation. Pure geometry, no drawing, so callers
// can lay text out (wrap, paginate) before any pixels exist.
Layout compute(const GfxRenderer& renderer);

// Paints the frame, `headword` above the divider, `dictName` in the footer, and `counter`
// right-aligned on the headword line. `dictName` and `counter` may be null. The returned
// Layout is the same one compute() gives, so the caller can draw its body straight after.
Layout draw(const GfxRenderer& renderer, const char* headword, const char* dictName, const char* counter);

}  // namespace DictionaryPanel
