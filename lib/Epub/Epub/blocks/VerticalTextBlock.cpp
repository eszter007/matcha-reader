#include "VerticalTextBlock.h"

#include <Utf8.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "GfxRenderer.h"
#include "ImageBlock.h"
#include "Kinsoku.h"

namespace {
constexpr int kNoStyle = 0;

// An inline image glyph: its text is "path\tsrc\twidth\theight", already placed by the layout.
void drawInlineImage(GfxRenderer& renderer, const VerticalPage& page, const VerticalGlyph& g, const int x,
                     const int y) {
  const std::string& info = page.glyphTextStr(g);
  const size_t t1 = info.find('\t');
  const size_t t2 = t1 == std::string::npos ? t1 : info.find('\t', t1 + 1);
  const size_t t3 = t2 == std::string::npos ? t2 : info.find('\t', t2 + 1);
  if (t3 == std::string::npos) return;
  const int w = atoi(info.c_str() + t2 + 1);
  const int h = atoi(info.c_str() + t3 + 1);
  if (w <= 0 || h <= 0) return;
  ImageBlock block(info.substr(0, t1), info.substr(t1 + 1, t2 - t1 - 1), static_cast<int16_t>(w),
                   static_cast<int16_t>(h));
  block.render(renderer, x, y);
}

// The sesame mark (U+FE45): a teardrop from a thin tail at the top left to a round head at the
// bottom right, like the 丶 stroke. Built once per page; each pixel is inside when it lies within
// the radius that tapers linearly along the tail-to-head axis.
struct SesameMask {
  int size = 0;
  uint16_t rows[16] = {};
};

SesameMask makeSesameMask(const int cellPx) {
  SesameMask m;
  m.size = std::clamp(cellPx * 4 / 10, 6, 16);
  const float n = static_cast<float>(m.size);
  const float headR = n * 0.30f;
  const float tailR = n * 0.06f;
  const float x0 = n * 0.18f, y0 = n * 0.12f;                // tail centre
  const float x1 = n - headR - 0.5f, y1 = n - headR - 0.5f;  // head centre
  const float ax = x1 - x0, ay = y1 - y0;
  const float len2 = ax * ax + ay * ay;
  for (int row = 0; row < m.size; ++row) {
    for (int col = 0; col < m.size; ++col) {
      const float px = col + 0.5f - x0, py = row + 0.5f - y0;
      const float t = std::clamp((px * ax + py * ay) / len2, 0.0f, 1.0f);
      const float ex = px - t * ax, ey = py - t * ay;
      const float r = tailR + (headR - tailR) * t;
      if (ex * ex + ey * ey <= r * r) m.rows[row] |= static_cast<uint16_t>(1u << col);
    }
  }
  return m;
}

// Returns the cell size it drew with, so a caller needing the same geometry (ruby) reuses it
// instead of re-deriving it -- every derivation probes the reference glyph out of the SD font.
int drawGlyphs(GfxRenderer& renderer, const VerticalPage& page, int fontId, int offsetX, int offsetY, bool black) {
  const int cellPx = verticalCellPx(renderer, fontId);
  // VerticalGlyph::y is the cell's TOP for every kind (see layoutPages); the draw calls disagree
  // about what they want, so each converts:
  //   drawText              - a TOP, and it adds the ascender itself, so hand it baseline-ascender
  //   drawTextRotated90CCW  - the y it is given, used as-is
  //   drawChar*InCell       - the cell's top-left, positioning within the cell from metrics
  const int ascender = renderer.getFontAscenderSize(fontId);
  const int baselineInCell = verticalCellBaselineOffset(renderer, fontId, cellPx);
  const int uprightTopAdjust = baselineInCell - ascender;
  const SesameMask sesame = makeSesameMask(cellPx);

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
    // Inline image: its text is "path\tsrc\twidth\theight", already placed by the layout (x is its
    // left edge), top-aligned. Skipped in the font scan pass, which draws nothing and would decode it.
    if (VerticalParsedText::isImageMarker(g.codepoint)) {
      if (!renderer.isFontCacheScanning()) drawInlineImage(renderer, page, g, dx, cellTop);
      continue;
    }
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
      // No per-shape dy nudges: drawCharVerticalRotatedInCell places each shape from its own measured ink
      // box within the cell it is given. The one exception is JLREQ's line-head rule -- an opening bracket
      // starting a column is set flush to the head, so it is handed a cell half an em higher and its
      // second-half placement lands on the line head. The layout flags which brackets those are (it knows
      // where columns start, indents included) and removes the same half em from the rest of the column.
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
      // Sesame dot beside the character, in the ruby band (JLREQ: an emphasis mark takes the ruby
      // position). It starts at the cell edge: centred in the band it read as floating between the
      // columns. Clamped to the panel like ruby, for the first column. Stamped from a mask, not drawn
      // as U+FE45: the built-in fonts lack that block, so a glyph showed as the missing-character box.
      const int left = std::min(dx + cellPx, renderer.getScreenWidth() - sesame.size);
      const int top = cellTop + (cellPx - sesame.size) / 2;
      for (int row = 0; row < sesame.size; ++row) {
        for (int col = 0; col < sesame.size; ++col) {
          if (sesame.rows[row] & (1u << col)) renderer.drawPixel(left + col, top + row, black);
        }
      }
    }
  }
  return cellPx;
}

}  // namespace

void VerticalTextBlock::render(GfxRenderer& renderer, int fontId, int offsetX, int offsetY, bool black) const {
  drawGlyphs(renderer, page_, fontId, offsetX, offsetY, black);
}

void VerticalTextBlock::render(GfxRenderer& renderer, int fontId, int rubyFontId, int offsetX, int offsetY,
                               bool black) const {
  const int cellPxLocal = drawGlyphs(renderer, page_, fontId, offsetX, offsetY, black);

  // JLREQ 3.3.2: ruby is half the size of its base and set solid, so one ruby character's body is
  // half an EM. The font's line height is the wrong yardstick: it carries Latin leading (48px
  // against a 33px em in Noto Serif JP), which stepped the ruby 45% too far apart and ran every
  // annotation past the foot of its base.
  const int rubyEmPx = verticalCellPx(renderer, rubyFontId);
  const int rubyLineH = (rubyEmPx + 1) / 2;
  // Baseline of a ruby character below the top of its body: the base text's measured figure,
  // halved with the glyph.
  const int rubyBaselineInBody = verticalCellBaselineOffset(renderer, rubyFontId, rubyEmPx) / 2;
  const auto rubyStyle = static_cast<EpdFontFamily::Style>(EpdFontFamily::SUP);

  int prevRubyBottom = -9999;
  uint16_t prevRubyColumn = UINT16_MAX;

  const auto isGroupFollower = [this](const size_t i) {
    const VerticalGlyph& n = page_.glyphs[i];
    return (n.style & EpdFontFamily::RUBY_CONTINUE) != 0 && n.renderKind != VerticalGlyph::RotatedRun &&
           n.renderKind != VerticalGlyph::UprightRun && !VerticalParsedText::isImageMarker(n.codepoint);
  };
  // JLREQ 3.3.8: ruby longer than its base may hang over a neighbouring kana by up to one ruby
  // character, and over the blank half of a closing bracket or 、。 before it or an opening
  // bracket after it. Never over kanji, nor over a character carrying ruby of its own.
  const auto hangoverAllowed = [this](const VerticalGlyph* n, const bool before) {
    if (!n) return false;                               // column edge
    if (!page_.glyphTextStr(*n).empty()) return false;  // it has ruby of its own
    // ... or is part of a group that has (its own share may be empty)
    if ((n->style & EpdFontFamily::RUBY_CONTINUE) != 0) return false;
    const size_t at = static_cast<size_t>(n - page_.glyphs.data());
    if (at + 1 < page_.glyphs.size() && (page_.glyphs[at + 1].style & EpdFontFamily::RUBY_CONTINUE) != 0) return false;
    if (n->renderKind == VerticalGlyph::RotatedRun || n->renderKind == VerticalGlyph::UprightRun) return false;
    const uint32_t cp = n->codepoint;
    if ((cp >= 0x3041 && cp <= 0x309F) || (cp >= 0x30A1 && cp <= 0x30FA)) return true;  // kana, not ー
    const int shift = Kinsoku::verticalShiftType(cp);
    return before ? (shift == 1 || shift == 2) : shift == 3;
  };

  size_t segmentEnd = 0;
  for (size_t gi = 0; gi < page_.glyphs.size(); gi++) {
    if (gi < segmentEnd) continue;  // set with the group that starts before it
    const VerticalGlyph& g = page_.glyphs[gi];
    if (VerticalParsedText::isImageMarker(g.codepoint) || g.renderKind == VerticalGlyph::RotatedRun ||
        g.renderKind == VerticalGlyph::UprightRun) {
      continue;
    }

    // The annotation goes over its whole group (JLREQ 3.3.6): this glyph and the group's glyphs
    // that follow it down the same column. Each carries a share of the reading; together they are
    // the reading. A group split across columns or pages is set per part.
    segmentEnd = gi + 1;
    while (segmentEnd < page_.glyphs.size() && page_.glyphs[segmentEnd].column == g.column &&
           isGroupFollower(segmentEnd)) {
      segmentEnd++;
    }
    std::string groupText;
    const std::string* text = &page_.glyphTextStr(g);
    if (segmentEnd > gi + 1) {
      for (size_t k = gi; k < segmentEnd; k++) groupText += page_.glyphTextStr(page_.glyphs[k]);
      text = &groupText;
    }
    const std::string& rubyText = *text;
    if (rubyText.empty()) continue;

    // Walk the ruby string once: count its characters, and measure how far right its ink can
    // reach. Every ruby character in the stack is drawn at the same x, so the widest one decides.
    size_t rubyCharCount = 0;
    int rubyInkRight = 0;
    // Ink band of an UPRIGHT character in this annotation, so a rotated one can be put on the
    // same line as its neighbours rather than on a band guessed from font metrics. -1 = none
    // measured, e.g. an annotation that is entirely dashes.
    int uprightInkTop = -1;
    int uprightInkHeight = 0;
    {
      const auto* p = reinterpret_cast<const unsigned char*>(rubyText.c_str());
      while (*p) {
        const uint32_t cp = utf8NextCodepoint(&p);
        if (cp == 0) break;
        GlyphInk ink;
        if (measureGlyphInk(renderer, rubyFontId, cp, static_cast<uint8_t>(rubyStyle), &ink) && ink.width > 0) {
          // SUP draws the glyph at 50%, so its metrics halve too.
          rubyInkRight = std::max(rubyInkRight, (ink.left + ink.width + 1) / 2);
          if (uprightInkTop < 0 && ink.height > 0 && !Kinsoku::needsVerticalRotation(cp)) {
            uprightInkTop = ink.top;
            uprightInkHeight = ink.height;
          }
        }
        rubyCharCount++;
      }
    }

    // Ruby's virtual body starts where the base character's cell ENDS, beside it, not over it.
    // At the text edge that puts it in the margin, per JLREQ Fig 2.37 -- clamped to the panel by
    // its measured ink reach, since a margin here is only a few px.
    const int rubyReach = rubyInkRight > 0 ? rubyInkRight : std::max(1, cellPxLocal / 2);
    const int rubyX = std::min(g.x + offsetX + cellPxLocal, renderer.getScreenWidth() - rubyReach);

    // The base the annotation is set against: from the group's first cell to its last.
    const int baseTop = g.y + offsetY;
    const int baseLen = page_.glyphs[segmentEnd - 1].y + offsetY + cellPxLocal - baseTop;
    // Ruby is set solid, so its virtual length is N ruby bodies; alignment is on VIRTUAL BODIES
    // rather than ink.
    const int rubyBlockH = static_cast<int>(rubyCharCount) * rubyLineH;
    int bodyTop;
    int step = rubyLineH;
    if (rubyBlockH <= baseLen) {
      // JLREQ 3.3.5 nakatsuki: centred on the base. A shorter annotation over a group is spread
      // to its length 1:2:1 -- half as much space at each end as between two characters.
      const int unit = (baseLen - rubyBlockH) / static_cast<int>(2 * rubyCharCount);
      bodyTop = segmentEnd > gi + 1 ? baseTop + unit : baseTop + (baseLen - rubyBlockH) / 2;
      if (segmentEnd > gi + 1) step = rubyLineH + 2 * unit;
    } else {
      // Longer: centred, taking half the excess each way where there is room (JLREQ 3.3.8). What a
      // side cannot take moves to the other. Layout spaced the base so that the room suffices, and
      // at a column's head left room above it, so the reading starts with the column (3.3.9).
      const VerticalGlyph* before = gi > 0 && page_.glyphs[gi - 1].column == g.column ? &page_.glyphs[gi - 1] : nullptr;
      const VerticalGlyph* after = segmentEnd < page_.glyphs.size() && page_.glyphs[segmentEnd].column == g.column
                                       ? &page_.glyphs[segmentEnd]
                                       : nullptr;
      const int overhang = rubyBlockH - baseLen;
      // Room beside the base first: the space layout left when it spaced the base out to the
      // reading, then one ruby character over a neighbour that may be hung over.
      const int lastBottom = page_.glyphs[segmentEnd - 1].y + cellPxLocal;
      // At a column's head the room is what layout left above the base, up to the text area's top;
      // at its foot the reading may run on into the margin.
      const int maxBefore =
          before ? std::max(0, g.y - (before->y + cellPxLocal)) + (hangoverAllowed(before, true) ? rubyLineH : 0)
                 : static_cast<int>(g.y);
      const int maxAfter =
          after ? std::max(0, after->y - lastBottom) + (hangoverAllowed(after, false) ? rubyLineH : 0) : overhang;
      int hangBefore = std::min(overhang / 2, maxBefore);
      const int hangAfter = std::min(overhang - hangBefore, maxAfter);
      hangBefore = std::min(overhang - hangAfter, maxBefore);
      // More than the neighbours allow: the excess goes on past them, away from a column edge.
      const int excess = overhang - hangBefore - hangAfter;
      if (excess > 0) {
        if (before && !after) {
          hangBefore += excess;
        } else if (before && after) {
          hangBefore += excess / 2;
        }  // at the column head it all goes below
      }
      bodyTop = baseTop - hangBefore;
    }
    int rubyY = bodyTop + rubyBaselineInBody - renderer.textBaselineOffset(rubyFontId, rubyText.c_str(), rubyStyle);
    if (g.column == prevRubyColumn && rubyY < prevRubyBottom + 1) {
      rubyY = prevRubyBottom + 1;
    }
    prevRubyBottom = rubyY + step * static_cast<int>(rubyCharCount - 1) + rubyLineH;
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

      // Ruby runs down the column like the base text, so a chōonpu or bracket inside an
      // annotation has to be turned on its side exactly as the main path turns it (see the
      // RotatedPunct branch above). Drawing the annotation as plain text left ハート's ー
      // lying horizontally across the column.
      const auto* cursor = reinterpret_cast<const unsigned char*>(buf);
      const uint32_t cp = utf8NextCodepoint(&cursor);
      if (Kinsoku::needsVerticalRotation(cp)) {
        // Put the mark on the line its upright neighbours sit on, so the stack stays evenly
        // spaced. drawText() takes rubyY as a TOP and puts the baseline textBaselineOffset()
        // below it; a SUP glyph's ink then spans [baseline - top/2, +height/2], its metrics
        // halved by the 50% scale. Measuring a real upright neighbour above (rather than
        // assuming a band from the ascender, which reaches well past where kana ink stops) gives
        // that band, and its centre in THIS character's slot is the target.
        //
        // The cell must hold the ROTATED glyph in both axes: drawCharVerticalRotatedInCell
        // centres ink at cellTop + cellSize/2 but derives its clamps from the same cellSize, so
        // too small a cell clamps the y centring to the cell top -- which silently parked the
        // mark low -- and can leave maxX below minX, which is UB in std::clamp. After CCW
        // rotation the glyph's width becomes its height, and vice versa; both halve with SUP.
        int cell = std::max(1, rubyLineH);
        GlyphInk rot;
        if (measureGlyphInk(renderer, rubyFontId, cp, static_cast<uint8_t>(rubyStyle), &rot) && rot.width > 0) {
          cell = std::max({cell, (rot.width + 1) / 2, (rot.height + 1) / 2});
        }
        const int baseline = rubyY + renderer.textBaselineOffset(rubyFontId, rubyText.c_str(), rubyStyle);
        // No upright neighbour to measure (an all-dash annotation): fall back to the slot centre.
        const int centre =
            uprightInkTop >= 0 ? baseline - uprightInkTop / 2 + uprightInkHeight / 4 : rubyY + rubyLineH / 2;
        renderer.drawCharVerticalRotatedInCell(rubyFontId, rubyX, centre - cell / 2, cell, cp,
                                               Kinsoku::verticalShiftType(cp), black, rubyStyle);
      } else {
        renderer.drawText(rubyFontId, rubyX, rubyY, buf, black, rubyStyle);
      }
      rubyY += step;
      ri += charLen;
    }
  }
}

bool VerticalTextBlock::hasImages() const {
  return std::any_of(page_.glyphs.begin(), page_.glyphs.end(),
                     [](const VerticalGlyph& g) { return VerticalParsedText::isImageMarker(g.codepoint); });
}

void VerticalTextBlock::renderImages(GfxRenderer& renderer, const int offsetX, const int offsetY) const {
  for (const VerticalGlyph& g : page_.glyphs) {
    if (VerticalParsedText::isImageMarker(g.codepoint))
      drawInlineImage(renderer, page_, g, g.x + offsetX, g.y + offsetY);
  }
}
