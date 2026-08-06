#include "VerticalTextBlock.h"

#include <Utf8.h>

#include <cstring>

#include "GfxRenderer.h"
#include "Kinsoku.h"

namespace {
constexpr int kNoStyle = 0;

void drawGlyphs(GfxRenderer& renderer, const VerticalPage& page, int fontId, int offsetX, int offsetY, bool black) {
  const int cellPx = verticalCellPx(renderer, fontId);
  // VerticalGlyph::y is the cell's TOP for every kind (see layoutPages); the draw calls disagree
  // about what they want, so each converts:
  //   drawText              - a TOP, and it adds the ascender itself, so hand it baseline-ascender
  //   drawTextRotated90CCW  - the y it is given, used as-is
  //   drawChar*InCell       - the cell's top-left, positioning within the cell from metrics
  const int ascender = renderer.getFontAscenderSize(fontId);
  const int baselineInCell = verticalCellBaselineOffset(renderer, fontId, cellPx);
  const int uprightTopAdjust = baselineInCell - ascender;

  // Styled-block borders (kakomi boxes, .k-solid-top separator rules). Rects share the glyphs'
  // cell-top coordinate space and the same offsets. `edges` says which lines to draw; EXTEND_*
  // runs the horizontal lines through to the screen edge (half-open box across a page boundary).
  for (const VerticalBoxRect& b : page.boxes) {
    const bool extendLeft = (b.edges & VerticalBoxRect::EXTEND_LEFT) != 0;
    const bool extendRight = (b.edges & VerticalBoxRect::EXTEND_RIGHT) != 0;
    const int yTop = b.y + offsetY;  // box rects are cell-top based, like the rotated kinds
    const int yBottom = yTop + b.h;
    const int xLeft = extendLeft ? 0 : b.x + offsetX;
    const int xRight = extendRight ? renderer.getScreenWidth() - 1 : b.x + b.w + offsetX;
    if (b.edges & VerticalBoxRect::DRAW_TOP) renderer.drawLine(xLeft, yTop, xRight, yTop, black);
    if (b.edges & VerticalBoxRect::DRAW_BOTTOM) renderer.drawLine(xLeft, yBottom, xRight, yBottom, black);
    if (b.edges & VerticalBoxRect::DRAW_LEFT) renderer.drawLine(xLeft, yTop, xLeft, yBottom, black);
    if (b.edges & VerticalBoxRect::DRAW_RIGHT) renderer.drawLine(xRight, yTop, xRight, yBottom, black);
  }

  for (const VerticalGlyph& g : page.glyphs) {
    const int dx = g.x + offsetX;
    const int cellTop = g.y + offsetY;
    int dy = cellTop;
    if (g.renderKind == VerticalGlyph::Upright || g.renderKind == VerticalGlyph::UprightRun) {
      dy = cellTop + uprightTopAdjust;
    }

    if (g.renderKind == VerticalGlyph::RotatedRun) {
      renderer.drawTextRotated90CCW(fontId, dx, dy, page.glyphText(g), black,
                                    static_cast<EpdFontFamily::Style>(g.style));
      continue;
    }

    if (g.renderKind == VerticalGlyph::UprightRun) {
      renderer.drawText(fontId, dx, dy, page.glyphText(g), black, static_cast<EpdFontFamily::Style>(g.style));
      continue;
    }

    if (g.renderKind == VerticalGlyph::RotatedPunct) {
      const int shiftType = Kinsoku::verticalShiftType(g.codepoint);
      // No per-shape dy nudges here any more: drawCharVerticalRotatedInCell places each shape
      // from its own measured ink box within the cell it is given. The one exception is JLREQ's
      // line-head rule: an opening bracket starting a column is set flush to the head, so hand
      // the renderer a cell half an em higher and its second-half placement lands on the line
      // head. The layout flags which brackets those are (it knows where columns start, indents
      // and all) and removes the same half em from the rest of the column.
      if (g.lineHeadFlush) dy -= cellPx / 2;
      renderer.drawCharVerticalRotatedInCell(fontId, dx, dy, cellPx, g.codepoint, shiftType, black,
                                             static_cast<EpdFontFamily::Style>(g.style));
      continue;
    }

    if (g.renderKind == VerticalGlyph::SmallKanaCorner) {
      renderer.drawCharVerticalCornerTopRight(fontId, dx, dy, cellPx, g.codepoint, black,
                                              static_cast<EpdFontFamily::Style>(g.style));
      continue;
    }

    std::string utf8Char;
    utf8AppendCodepoint(g.codepoint, utf8Char);
    // For thin glyphs like 一 (height << ascender), the uniform nudge
    // over-shifts them because their ink sits near the em-box center, not
    // the top. Pull them back by the difference between their top and a
    // full glyph's top so the ink stays visually centered in the column.
    int uprightDy = dy;
    {
      GlyphInk ink;
      if (measureGlyphInk(renderer, fontId, g.codepoint, g.style, &ink) && ink.height > 0 && ink.height < ink.top) {
        // Thin glyph (一): ink sits near the middle of the em-box, not the top, so centre its
        // ink in the cell instead of using the common baseline. Same space conversion as
        // uprightTopFor: derive the cell's top, then hand drawText a TOP (it adds the ascender).
        uprightDy = cellTop + cellPx / 2 + ink.top - ink.height / 2 - ascender;
      }
    }
    renderer.drawText(fontId, dx, uprightDy, utf8Char.c_str(), black, static_cast<EpdFontFamily::Style>(g.style));

    if (g.emphasis) {
      // Sesame dot beside the character, clear of its cell by the same ink gap a normal pair of
      // characters leaves.
      const int emX = dx + cellPx + verticalNominalInkGapPx(renderer, fontId, cellPx);
      const int emY = dy;
      renderer.drawText(fontId, emX, emY, "\xef\xb9\x85", black, static_cast<EpdFontFamily::Style>(EpdFontFamily::SUP));
    }
  }
}

}  // namespace

void VerticalTextBlock::render(GfxRenderer& renderer, int fontId, int offsetX, int offsetY, bool black) const {
  drawGlyphs(renderer, page_, fontId, offsetX, offsetY, black);
}

void VerticalTextBlock::render(GfxRenderer& renderer, int fontId, int rubyFontId, int offsetX, int offsetY,
                               bool black) const {
  drawGlyphs(renderer, page_, fontId, offsetX, offsetY, black);

  const int rubyLineH = (renderer.getLineHeight(rubyFontId) + 1) / 2;
  const auto rubyStyle = static_cast<EpdFontFamily::Style>(EpdFontFamily::SUP);

  const int cellPxLocal = verticalCellPx(renderer, fontId);
  int prevRubyBottom = -9999;
  uint16_t prevRubyColumn = UINT16_MAX;

  // JLREQ 3.3.8: ruby may hang over ordinary kana/kanji, but not over punctuation or brackets
  // (their half-em placement is what keeps them legible), nor over a character carrying its own.
  const auto hangoverAllowedOver = [this](const VerticalGlyph* n) {
    if (!n) return false;                               // page/column edge
    if (!page_.glyphTextStr(*n).empty()) return false;  // it has ruby of its own
    if (n->renderKind == VerticalGlyph::RotatedRun || n->renderKind == VerticalGlyph::UprightRun) return false;
    if (n->codepoint == 0) return false;
    return Kinsoku::verticalShiftType(n->codepoint) == 0 && !Kinsoku::needsVerticalRotation(n->codepoint);
  };

  for (size_t gi = 0; gi < page_.glyphs.size(); gi++) {
    const VerticalGlyph& g = page_.glyphs[gi];
    const std::string& rubyText = page_.glyphTextStr(g);
    if (rubyText.empty() || g.renderKind == VerticalGlyph::RotatedRun || g.renderKind == VerticalGlyph::UprightRun) {
      continue;
    }

    // Walk the ruby string once: count its characters, and measure how far right its ink can
    // reach. Every ruby character in the stack is drawn at the same x, so the widest one decides.
    size_t rubyCharCount = 0;
    int rubyInkRight = 0;
    {
      const auto* p = reinterpret_cast<const unsigned char*>(rubyText.c_str());
      while (*p) {
        const uint32_t cp = utf8NextCodepoint(&p);
        if (cp == 0) break;
        GlyphInk ink;
        if (measureGlyphInk(renderer, rubyFontId, cp, static_cast<uint8_t>(rubyStyle), &ink) && ink.width > 0) {
          // SUP draws the glyph at 50%, so its metrics halve too.
          rubyInkRight = std::max(rubyInkRight, (ink.left + ink.width + 1) / 2);
        }
        rubyCharCount++;
      }
    }

    // Ruby's virtual body starts where the base character's cell ENDS, beside it, not over it.
    // At the text edge that puts it in the margin, per JLREQ Fig 2.37 -- clamped to the panel by
    // its measured ink reach, since a margin here is only a few px.
    const int rubyReach = rubyInkRight > 0 ? rubyInkRight : std::max(1, cellPxLocal / 2);
    const int rubyX = std::min(g.x + offsetX + cellPxLocal, renderer.getScreenWidth() - rubyReach);

    // JLREQ 3.3.5 nakatsuki (中付き): the ruby text's vertical centre aligns with the base
    // character's. Ruby is set solid, so its virtual length is N ruby bodies, and the alignment is
    // on VIRTUAL BODIES rather than ink. The half-ascender term undoes a renderer detail: SUP
    // scales the glyph to 50% but drawText still offsets by the full ascender.
    const int rubyBlockH = static_cast<int>(rubyCharCount) * rubyLineH;
    const int rubyBodyTop = g.y + offsetY + (cellPxLocal - rubyBlockH) / 2;
    int rubyY = rubyBodyTop - renderer.getFontAscenderSize(rubyFontId) / 2;

    // JLREQ 3.3.8: nakatsuki spills half the overhang each way; where a neighbour cannot be hung
    // over, its share moves to the other side.
    const int overhang = rubyBlockH - cellPxLocal;
    if (overhang > 0) {
      const VerticalGlyph* before = nullptr;
      const VerticalGlyph* after = nullptr;
      if (gi > 0 && page_.glyphs[gi - 1].column == g.column) before = &page_.glyphs[gi - 1];
      if (gi + 1 < page_.glyphs.size() && page_.glyphs[gi + 1].column == g.column) after = &page_.glyphs[gi + 1];
      const bool canBefore = hangoverAllowedOver(before);
      const bool canAfter = hangoverAllowedOver(after);
      if (canAfter && !canBefore) {
        rubyY += overhang / 2;  // push it all onto the following character
      } else if (canBefore && !canAfter) {
        rubyY -= overhang / 2;  // ... or all onto the preceding one
      }
      // both or neither: leave it centred; the forward-only check below then favours the
      // character after, as the spec prefers.
    }
    if (g.column == prevRubyColumn && rubyY < prevRubyBottom + 1) {
      rubyY = prevRubyBottom + 1;
    }

    prevRubyBottom = rubyY + rubyBlockH;
    prevRubyColumn = g.column;

    size_t ri = 0;
    while (ri < rubyText.size()) {
      const auto c0 = static_cast<unsigned char>(rubyText[ri]);
      size_t charLen = 1;
      if (c0 >= 0xF0)
        charLen = 4;
      else if (c0 >= 0xE0)
        charLen = 3;
      else if (c0 >= 0xC0)
        charLen = 2;

      if (ri + charLen > rubyText.size()) break;

      char buf[5];
      std::memcpy(buf, rubyText.data() + ri, charLen);
      buf[charLen] = '\0';

      renderer.drawText(rubyFontId, rubyX, rubyY, buf, black, rubyStyle);
      rubyY += rubyLineH;
      ri += charLen;
    }
  }
}
