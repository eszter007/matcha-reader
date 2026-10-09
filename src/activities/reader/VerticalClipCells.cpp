#include "VerticalClipCells.h"

#include <GfxRenderer.h>

#include <algorithm>
#include <string>

namespace {

uint32_t countCodepoints(const std::string& text) {
  return static_cast<uint32_t>(
      std::count_if(text.begin(), text.end(), [](const char c) { return (static_cast<uint8_t>(c) & 0xC0) != 0x80; }));
}

}  // namespace

void forEachVerticalClipCell(const GfxRenderer& renderer, const VerticalPage& page, const int fontId,
                             const int marginLeft, const int marginTop, void* ctx,
                             const VerticalClipCellVisitor visit) {
  const int cellPx = verticalCellPx(renderer, fontId);
  uint32_t offset = page.visibleTextOffset;
  uint32_t previousParagraph = UINT32_MAX;
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
        height = std::max(cellPx, renderer.getTextAdvanceX(fontId, text.c_str(),
                                                           static_cast<EpdFontFamily::Style>(g.style)));
      }
    } else if (g.codepoint == 0) {
      continue;
    }
    VerticalClipCell cell;
    cell.x = static_cast<int16_t>(g.x + marginLeft);
    cell.y = static_cast<int16_t>(g.y + marginTop);
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
    if (!visit(ctx, cell)) return;
  }
}
