#pragma once

#include <utility>

#include "../VerticalParsedText.h"

class GfxRenderer;

// Renders a single VerticalPage (produced by VerticalParsedText) to the
// screen. Analogous to TextBlock::render() in the horizontal pipeline, but
// deliberately kept as a free-standing class rather than a TextBlock
// subclass -- TextBlock's exact virtual interface wasn't verified against
// this checkout, and the two layouts differ enough (per-character cells vs.
// per-line word runs) that forcing a shared base class would likely cost
// more in awkward indirection than it saves. If/when this lands upstream,
// reconciling the two under a common `Block` interface (so Page can hold
// either) is the natural follow-up cleanup once the column code is proven
// out.
class VerticalTextBlock {
 public:
  explicit VerticalTextBlock(VerticalPage page) : page_(std::move(page)) {}

  void render(GfxRenderer& renderer, int fontId, int offsetX = 0, int offsetY = 0, bool black = true) const;
  void render(GfxRenderer& renderer, int fontId, int rubyFontId, int offsetX, int offsetY, bool black = true) const;

  const VerticalPage& page() const { return page_; }

 private:
  VerticalPage page_;
};
