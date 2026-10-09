#pragma once

#include <Epub/VerticalParsedText.h>

#include <cstdint>

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

// Visits every text cell of `page` in reading order. The visitor returns false to stop early.
// Offsets are derived from the page's first offset by counting the codepoints the cells carry,
// which is how the layout assigned them; inline images and ruby carry none.
using VerticalClipCellVisitor = bool (*)(void* ctx, const VerticalClipCell& cell);
void forEachVerticalClipCell(const GfxRenderer& renderer, const VerticalPage& page, int fontId, int marginLeft,
                             int marginTop, void* ctx, VerticalClipCellVisitor visit);
