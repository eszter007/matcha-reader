#include "VerticalClipCells.h"

#include <GfxRenderer.h>
#include <Kinsoku.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

uint32_t countCodepoints(const std::string& text) {
  return static_cast<uint32_t>(
      std::count_if(text.begin(), text.end(), [](const char c) { return (static_cast<uint8_t>(c) & 0xC0) != 0x80; }));
}

}  // namespace

void forEachVerticalClipCell(const GfxRenderer& renderer, const VerticalPage& page, const int fontId,
                             const int marginLeft, const int marginTop, void* ctx, const VerticalClipCellVisitor visit,
                             const int knownCellPx) {
  const int cellPx = knownCellPx > 0 ? knownCellPx : verticalCellPx(renderer, fontId);
  // Punctuation and small kana are drawn shifted inside their cell, so their glyph x is not the
  // column's. Anchor every cell to the leftmost upright glyph of its column (or the leftmost glyph
  // when a column has none), so a selection reads as one straight column.
  std::vector<int> columnX(page.columnCount, -1);
  std::vector<bool> columnUpright(page.columnCount, false);
  for (const VerticalGlyph& g : page.glyphs) {
    if (g.column >= columnX.size() || VerticalParsedText::isImageMarker(g.codepoint)) continue;
    const bool upright = g.renderKind == VerticalGlyph::Upright;
    if (upright && !columnUpright[g.column]) {
      columnUpright[g.column] = true;
      columnX[g.column] = g.x;
    } else if (columnX[g.column] < 0 || (upright == columnUpright[g.column] && g.x < columnX[g.column])) {
      columnX[g.column] = g.x;
    }
  }
  // A glyph's y is where its ink is drawn. For a full-size upright glyph that is the cell top; a
  // small kana is centred in its frame (JLREQ A.11), so its y lies half a row down, and a cell
  // taken from it starts there and drags the cell above into stretching to meet it. Columns do
  // not share one row grid (a half-width bracket shifts everything after it), so the cell top
  // comes from the nearest full-size neighbour in the same column: one cell above the next, or
  // one cell below the previous.
  const auto isFullUpright = [](const VerticalGlyph& g) {
    return g.renderKind == VerticalGlyph::Upright && !VerticalParsedText::isImageMarker(g.codepoint) &&
           !Kinsoku::isSmallKana(g.codepoint);
  };
  const auto cellTop = [&](const size_t i) -> int {
    const VerticalGlyph& g = page.glyphs[i];
    if (!Kinsoku::isSmallKana(g.codepoint) || g.renderKind != VerticalGlyph::Upright) return g.y;
    for (size_t j = i + 1; j < page.glyphs.size() && page.glyphs[j].column == g.column; j++) {
      if (isFullUpright(page.glyphs[j])) return static_cast<int>(page.glyphs[j].y) - cellPx;
    }
    for (size_t j = i; j-- > 0 && page.glyphs[j].column == g.column;) {
      if (isFullUpright(page.glyphs[j])) return static_cast<int>(page.glyphs[j].y) + cellPx;
    }
    return g.y;
  };
  uint32_t offset = page.visibleTextOffset;
  uint32_t previousParagraph = UINT32_MAX;
  // Cells are handed over one behind, so a cell can be stretched down to the next one in its
  // column: a selection then reads as one unbroken bar per column, across the air that
  // punctuation shifts and kinsoku spacing leave between glyphs.
  VerticalClipCell pending;
  bool hasPending = false;
  const auto emit = [&](const VerticalClipCell* next) {
    if (!hasPending) return true;
    if (next && next->column == pending.column && next->y > pending.y) {
      pending.height = static_cast<int16_t>(std::max<int>(pending.height, next->y - pending.y));
    }
    hasPending = false;
    return visit(ctx, pending);
  };
  for (size_t i = 0; i < page.glyphs.size(); i++) {
    const VerticalGlyph& g = page.glyphs[i];
    if (VerticalParsedText::isImageMarker(g.codepoint)) continue;
    const bool run = g.renderKind == VerticalGlyph::RotatedRun || g.renderKind == VerticalGlyph::UprightRun;
    uint32_t codepoints = 1;
    int height = cellPx;
    if (run) {
      const std::string& text = page.glyphTextStr(g);
      if (text.empty()) continue;
      codepoints = countCodepoints(text);
      if (g.renderKind == VerticalGlyph::RotatedRun) {
        height = std::max(cellPx,
                          renderer.getTextAdvanceX(fontId, text.c_str(), static_cast<EpdFontFamily::Style>(g.style)));
      }
    } else if (g.codepoint == 0) {
      continue;
    }
    VerticalClipCell cell;
    const int x = g.column < columnX.size() && columnX[g.column] >= 0 ? columnX[g.column] : g.x;
    cell.x = static_cast<int16_t>(x + marginLeft);
    cell.y = static_cast<int16_t>(cellTop(i) + marginTop);
    cell.width = static_cast<int16_t>(cellPx);
    cell.height = static_cast<int16_t>(height);
    cell.column = g.column;
    cell.row = g.row;
    cell.glyphIndex = static_cast<uint16_t>(std::min<size_t>(i, UINT16_MAX));
    cell.startOffset = offset;
    cell.endOffset = offset + codepoints;
    cell.paragraphStart = previousParagraph != UINT32_MAX && g.paragraphIndex != previousParagraph;
    previousParagraph = g.paragraphIndex;
    offset += codepoints;
    if (!emit(&cell)) return;
    pending = cell;
    hasPending = true;
  }
  emit(nullptr);
}
