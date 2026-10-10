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
  Rect box;        // the panel's outer rectangle
  Rect body;       // text area between the divider and the dictionary-name footer
  Rect addButton;  // tap target of the save-sentence button in the top-right corner
};

// Keeps the panel clear of a band of the page -- the looked-up word and its selection handles --
// by moving it into the larger free area above or below the band and shrinking it to fit. An
// empty band restores the centred panel. Set by the lookup panel that owns the screen, before it
// lays out or draws, and cleared when it leaves; every caller of compute() then agrees.
void setAvoid(Rect band);

// The panel's rectangles for the current orientation. Pure geometry, no drawing, so callers
// can lay text out (wrap, paginate) before any pixels exist.
Layout compute(const GfxRenderer& renderer);

// Paints the frame and `headword` above the divider. The footer carries "kind | dictName" on the
// left, clipped with an ellipsis, and `counter` (the "1/2" page position) on the right. `kind`
// names which index answered where several can (the Japanese vocab/grammar/name indexes) and is
// null when the source is unambiguous; any of the texts may be null. `addButton` draws the
// save-sentence button in the top-right corner (touch boards) -- its tap target is
// Layout::addButton either way. The returned Layout is the same one compute() gives, so the
// caller can draw its body straight after.
Layout draw(const GfxRenderer& renderer, const char* headword, const char* dictName, const char* counter,
            const char* kind = nullptr, bool addButton = false);

// Blank the button-hint band before the panel's own hints are drawn into it. The panel does not
// clear the screen, so without this the reader's labels stay in the framebuffer and the two sets
// overprint each other.
void clearButtonHints(const GfxRenderer& renderer);

}  // namespace DictionaryPanel
