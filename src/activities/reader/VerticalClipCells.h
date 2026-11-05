#pragma once

#include <Epub/VerticalParsedText.h>

#include <cstddef>
#include <cstdint>
#include <vector>

class GfxRenderer;

// One selectable unit of a vertical page for clippings: a character cell, or a whole rotated or
// upright Latin run. Offsets are chapter-visible codepoint offsets, the same coordinate the
// horizontal TextBlock source ranges use, so a clipping made in either layout highlights in both.
struct VerticalClipCell {
  int16_t x = 0;  // screen rect, margins applied
  int16_t y = 0;
  int16_t width = 0;
  int16_t height = 0;
  uint16_t column = 0;
  uint16_t row = 0;
  uint16_t glyphIndex = 0;
  uint32_t startOffset = 0;  // [start, end)
  uint32_t endOffset = 0;
  bool paragraphStart = false;
};

// Where a glyph's cell is, margins not applied. Small kana and half-em punctuation are drawn
// shifted inside their cell, so their own x/y is not the cell's: a column's x is its leftmost
// upright glyph's, and a shifted glyph's top comes from a full-size neighbour in its column.
void verticalColumnXs(const VerticalPage& page, std::vector<int>& columnX);
int verticalCellTop(const VerticalPage& page, size_t glyphIndex, int cellPx);

// Visits every text cell of `page` in reading order. The visitor returns false to stop early.
// Offsets are derived from the page's first offset by counting the codepoints the cells carry,
// which is how the layout assigned them; inline images and ruby carry none.
using VerticalClipCellVisitor = bool (*)(void* ctx, const VerticalClipCell& cell);
// cellPx > 0 gives the cell size, measured where the reader font is loaded: measured after it is
// released, the reference glyph reports the line height and every cell comes out too narrow.
void forEachVerticalClipCell(const GfxRenderer& renderer, const VerticalPage& page, int fontId, int marginLeft,
                             int marginTop, void* ctx, VerticalClipCellVisitor visit, int cellPx = 0);
