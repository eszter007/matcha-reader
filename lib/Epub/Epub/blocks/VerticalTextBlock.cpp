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
  const int cjkAdvance = renderer.getTextAdvanceX(
      fontId, "\xe6\xbc\xa2", static_cast<EpdFontFamily::Style>(0));
  if (cjkAdvance > 0) return cjkAdvance + cjkAdvance / 6;
  return renderer.getLineHeight(fontId);
}

void drawGlyphs(GfxRenderer& renderer, const VerticalPage& page, int fontId, int offsetX, int offsetY, bool black) {
  const int cellPx = computeCellPx(renderer, fontId);
  const int globalDownNudge = std::max(1, (cellPx * 3) / 8);

  // Styled-block borders (kakomi boxes, .k-solid-top separator rules). Rects share the glyphs'
  // logical coordinate space; glyph y values are baseline-based while rect y is cell-top-based,
  // but both get the same offsets. `edges` says which lines to draw; EXTEND_* runs the
  // horizontal lines through to the screen edge (half-open box across a page boundary).
  for (const VerticalBoxRect& b : page.boxes) {
    const bool extendLeft = (b.edges & VerticalBoxRect::EXTEND_LEFT) != 0;
    const bool extendRight = (b.edges & VerticalBoxRect::EXTEND_RIGHT) != 0;
    const int yTop = b.y + offsetY + globalDownNudge;
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
    int dy = g.y + offsetY + globalDownNudge;

    if (g.renderKind == VerticalGlyph::RotatedRun) {
      renderer.drawTextRotated90CCW(fontId, dx, dy, g.rotatedRunText.c_str(), black,
                                     static_cast<EpdFontFamily::Style>(g.style));
      continue;
    }

    if (g.renderKind == VerticalGlyph::UprightRun) {
      renderer.drawText(fontId, dx, dy, g.rotatedRunText.c_str(), black, static_cast<EpdFontFamily::Style>(g.style));
      continue;
    }

    if (g.renderKind == VerticalGlyph::RotatedPunct) {
      const int shiftType = Kinsoku::verticalShiftType(g.codepoint);
      if (g.codepoint == 0x2025 || g.codepoint == 0x2026) {
        dy += std::max(1, (cellPx * 5) / 8);
      } else if (shiftType == 4) {
        // Dashes/chōonpu sat visibly low in their cell (device photo, kyokasho) -- a
        // quarter cell less down-shift than the ellipsis dot stack.
        dy += std::max(1, (cellPx * 3) / 8);
      }
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
                                   &gh) && gh > 0 && gh < gt) {
        // Thin glyph (一): ink sits near the middle of the em-box, not the
        // top. Remove the uniform nudge and instead position so the ink's
        // vertical center aligns with the cell's vertical center.
        // ink center = cursorY - top + height/2; cell center ≈ g.y + offsetY
        uprightDy = g.y + offsetY + gt - gh / 2;
      }
    }
    renderer.drawText(fontId, dx, uprightDy, utf8Char.c_str(), black, static_cast<EpdFontFamily::Style>(g.style));

    if (g.emphasis) {
      const int emX = dx + cellPx + std::max(1, cellPx / 12);
      const int emY = dy;
      renderer.drawText(fontId, emX, emY, "\xef\xb9\x85", black,
                        static_cast<EpdFontFamily::Style>(EpdFontFamily::SUP));
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
  const int rubyAscender = renderer.getFontAscenderSize(rubyFontId) / 2;
  const int baseLineH = renderer.getLineHeight(fontId);
  const auto rubyStyle = static_cast<EpdFontFamily::Style>(EpdFontFamily::SUP);

  const int cellPxLocal = computeCellPx(renderer, fontId);
  int prevRubyBottom = -9999;
  uint16_t prevRubyColumn = UINT16_MAX;

  for (const VerticalGlyph& g : page_.glyphs) {
    if (g.rubyText.empty() || g.renderKind == VerticalGlyph::RotatedRun || g.renderKind == VerticalGlyph::UprightRun) {
      continue;
    }

    const int rubyX = g.x + offsetX + cellPxLocal - cellPxLocal / 8;

    size_t rubyCharCount = 0;
    {
      size_t ri = 0;
      while (ri < g.rubyText.size()) {
        const auto c0 = static_cast<unsigned char>(g.rubyText[ri]);
        if (c0 < 0x80) ri += 1;
        else if ((c0 & 0xE0) == 0xC0) ri += 2;
        else if ((c0 & 0xF0) == 0xE0) ri += 3;
        else ri += 4;
        rubyCharCount++;
      }
    }

    const int rubyBlockH = static_cast<int>(rubyCharCount) * rubyLineH;
    const int rubyDownNudge = std::max(3, (rubyLineH * 4) / 5);
    int rubyY = g.y + offsetY + (baseLineH - rubyBlockH) / 2 - rubyLineH + rubyDownNudge;

    if (g.column == prevRubyColumn && rubyY < prevRubyBottom + 1) {
      rubyY = prevRubyBottom + 1;
    }

    prevRubyBottom = rubyY + rubyBlockH;
    prevRubyColumn = g.column;

    size_t ri = 0;
    while (ri < g.rubyText.size()) {
      const auto c0 = static_cast<unsigned char>(g.rubyText[ri]);
      size_t charLen = 1;
      if (c0 >= 0xF0) charLen = 4;
      else if (c0 >= 0xE0) charLen = 3;
      else if (c0 >= 0xC0) charLen = 2;

      if (ri + charLen > g.rubyText.size()) break;

      char buf[5];
      std::memcpy(buf, g.rubyText.data() + ri, charLen);
      buf[charLen] = '\0';

      renderer.drawText(rubyFontId, rubyX, rubyY, buf, black, rubyStyle);
      rubyY += rubyLineH;
      ri += charLen;
    }
  }
}
