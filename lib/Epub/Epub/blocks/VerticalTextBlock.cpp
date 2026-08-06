#include "VerticalTextBlock.h"

#include <cstring>

#include "GfxRenderer.h"
#include "Kinsoku.h"

namespace {
constexpr int kNoStyle = 0;

void encodeCodepoint(uint32_t cp, std::string& out) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

int computeCellPx(GfxRenderer& renderer, int fontId) {
  // A cold SD-font advance table measures 漢 as 0 and silently falls back to getLineHeight,
  // so the draw-time cell no longer matches the layout-time cell and every rotated-punct
  // nudge lands wrong for that frame (seen live: cell flapping 42 -> 33 on NotoSansJP).
  renderer.ensureSdCardFontReady(fontId, "\xe6\xbc\xa2", 0x01);
  const int cjkAdvance = renderer.getTextAdvanceX(fontId, "\xe6\xbc\xa2", static_cast<EpdFontFamily::Style>(0));
  if (cjkAdvance > 0) return cjkAdvance + cjkAdvance / 6;
  return renderer.getLineHeight(fontId);
}

void drawGlyphs(GfxRenderer& renderer, const VerticalPage& page, int fontId, int offsetX, int offsetY, bool black) {
  const int cellPx = computeCellPx(renderer, fontId);
  // Was a flat 3/8-cell "down nudge" added to every glyph; now the correction that sits the
  // measured CJK ink box in the middle of its cell (see verticalCellBaselineOffset).
  //
  // It applies ONLY to the baseline-anchored kinds. Upright/UprightRun carry the font ascender in
  // g.y, and this is the adjustment to that baseline. RotatedRun, RotatedPunct and SmallKanaCorner
  // anchor on the cell's top instead (see RenderKind), and their draw calls place ink from that
  // point -- adding a baseline correction to them shifted their ink relative to the CJK around
  // them. Measured: a rotated Latin run's ink started 7px above the CJK ink beside it, which read
  // as the CJK column "starting too low" next to it.
  const int ascender = renderer.getFontAscenderSize(fontId);
  const int baselineInCell = verticalCellBaselineOffset(renderer, fontId, cellPx);
  // VerticalGlyph::y is the cell's TOP for every kind (see layoutPages). What differs is what each
  // draw call wants:
  //   drawText              - a TOP; it adds the font ascender itself to reach the baseline, so
  //                           hand it (baseline - ascender).
  //   drawTextRotated90CCW  - the y it is given, used as-is.
  //   drawChar*InCell       - the cell's top-left, positioning within the cell from metrics.
  const int uprightTopAdjust = baselineInCell - ascender;
  // Two coordinate spaces meet here, and conflating them is what the old "global down nudge" was
  // really compensating for:
  //
  //   Upright / UprightRun  - layout stores g.y as a BASELINE (row*cellPx + ascender), but
  //                           GfxRenderer::drawText takes the glyph's TOP and adds the ascender
  //                           itself (yPos = y + getFontAscenderSize). Passing the baseline
  //                           straight in added the ascender twice, drawing every upright glyph a
  //                           full ascender low -- measured 34px, which is most of a 33px cell.
  //   RotatedRun            - drawTextRotated90CCW uses y as given (lastBaseY = y).
  //   RotatedPunct / SmallKanaCorner - drawChar*InCell position within the cell from its top-left.
  //
  // So only the first group needs converting, and the conversion is exact rather than a fraction:
  // put the baseline at baselineInCell below the cell's top, then hand drawText the top.

  // Styled-block borders (kakomi boxes, .k-solid-top separator rules). Rects share the glyphs'
  // logical coordinate space; glyph y values are baseline-based while rect y is cell-top-based,
  // but both get the same offsets. `edges` says which lines to draw; EXTEND_* runs the
  // horizontal lines through to the screen edge (half-open box across a page boundary).
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
    encodeCodepoint(g.codepoint, utf8Char);
    // For thin glyphs like 一 (height << ascender), the uniform nudge
    // over-shifts them because their ink sits near the em-box center, not
    // the top. Pull them back by the difference between their top and a
    // full glyph's top so the ink stays visually centered in the column.
    int uprightDy = dy;
    {
      int gl = 0, gw = 0, gt = 0, gh = 0;
      if (renderer.getGlyphMetrics(fontId, g.codepoint, static_cast<EpdFontFamily::Style>(g.style), &gl, &gw, &gt,
                                   &gh) &&
          gh > 0 && gh < gt) {
        // Thin glyph (一): ink sits near the middle of the em-box, not the top, so centre its
        // ink in the cell instead of using the common baseline. Same space conversion as
        // uprightTopFor: derive the cell's top, then hand drawText a TOP (it adds the ascender).
        uprightDy = cellTop + cellPx / 2 + gt - gh / 2 - ascender;
      }
    }
    renderer.drawText(fontId, dx, uprightDy, utf8Char.c_str(), black, static_cast<EpdFontFamily::Style>(g.style));

    if (g.emphasis) {
      const int emX = dx + cellPx + std::max(1, cellPx / 12);
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

  const int cellPxLocal = computeCellPx(renderer, fontId);
  int prevRubyBottom = -9999;
  uint16_t prevRubyColumn = UINT16_MAX;

  // JLREQ 3.3.8: when a ruby text is longer than its base, whether the overhang may fall on the
  // character before, the character after, or both, depends on what those neighbours are. Ruby
  // may hang over ordinary kana/kanji, but not over punctuation or bracket shapes (their own
  // half-em placement is what keeps them legible), nor over a character that itself carries ruby.
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
      size_t ri = 0;
      while (ri < rubyText.size()) {
        const auto c0 = static_cast<unsigned char>(rubyText[ri]);
        size_t len = 1;
        uint32_t cp = c0;
        if (c0 >= 0xF0) {
          len = 4;
          cp = c0 & 0x07u;
        } else if (c0 >= 0xE0) {
          len = 3;
          cp = c0 & 0x0Fu;
        } else if (c0 >= 0xC0) {
          len = 2;
          cp = c0 & 0x1Fu;
        }
        if (ri + len > rubyText.size()) break;
        for (size_t k = 1; k < len; k++) cp = (cp << 6) | (static_cast<unsigned char>(rubyText[ri + k]) & 0x3Fu);
        int gl = 0, gw = 0, gt = 0, gh = 0;
        if (renderer.getGlyphMetrics(rubyFontId, cp, rubyStyle, &gl, &gw, &gt, &gh) && gw > 0) {
          // SUP draws the glyph at 50%, so its metrics halve too.
          rubyInkRight = std::max(rubyInkRight, (gl + gw + 1) / 2);
        }
        ri += len;
        rubyCharCount++;
      }
    }

    // Ruby starts where its base character's cell ENDS -- its own virtual body beside the base,
    // not overlapping it. (It used to be pulled back by an eighth of a cell, which read as ruby
    // crowding the character it annotates.) For the first column that puts it outside the text
    // area and into the margin, which is what JLREQ asks for (Fig 2.37). The margin is only a few
    // px on this screen, so clamp to the panel -- using the measured ink reach rather than a
    // half-cell guess, which held the edge column's ruby a couple of px further left than needed.
    const int rubyReach = rubyInkRight > 0 ? rubyInkRight : std::max(1, cellPxLocal / 2);
    const int rubyX = std::min(g.x + offsetX + cellPxLocal, renderer.getScreenWidth() - rubyReach);

    // JLREQ 3.3.5 nakatsuki (中付き): "attach a ruby character so that its vertical center
    // matches that of the base character", and with three or more ruby characters on one base,
    // "position a ruby text so that its vertical center is aligned with that of its base
    // character". Ruby is set solid, so the text's virtual length is simply N ruby bodies.
    //
    // The alignment is on VIRTUAL BODIES, not ink -- so no glyph measuring and no fudge factor.
    // One renderer detail has to be undone: ruby draws with EpdFontFamily::SUP, which scales the
    // glyph to 50%, but GfxRenderer::drawText still offsets by the FULL ascender (yPos = y +
    // getFontAscenderSize). The rendered baseline therefore belongs half an ascender higher than
    // drawText will put it, which is exactly what the old "ruby sat a touch high, drop it by
    // ~6/5 of a ruby line" tuning was compensating for from the wrong end.
    const int rubyBlockH = static_cast<int>(rubyCharCount) * rubyLineH;
    const int rubyBodyTop = g.y + offsetY + (cellPxLocal - rubyBlockH) / 2;
    int rubyY = rubyBodyTop - renderer.getFontAscenderSize(rubyFontId) / 2;

    // JLREQ 3.3.8: distribute any overhang according to what the neighbours are. Centring
    // (nakatsuki) spills half before and half after the base; where a neighbour cannot be hung
    // over, that share moves to the other side, and where neither can, the ruby stays centred
    // and simply overlaps as little as possible.
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
      // both or neither: leave it centred -- "at break-even situation, the hangover is usually
      // on the character after", which centring already favours once the collision check below
      // only ever pushes forward.
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
