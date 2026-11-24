#pragma once

#include <GfxRenderer.h>

#include "RubySize.h"

// Measuring and drawing ruby at the ruby scale (RubySize.h).
//
// At half size (the default, JLREQ 3.3.2) a bitmap font draws ruby with its SUP style, which
// reduces each 2x2 block of the glyph to one pixel. Other sizes, and every size of a vector
// font, draw through the scaled path; a vector font then rasterizes the ruby at its own pixel
// size (GfxRenderer::sizedVectorFace) instead of shrinking the body-size bitmap.
namespace ruby {

// Whether ruby is drawn with the SUP style: half size from a bitmap font drawn as is. A face
// drawn at a scale (a built-in size made from another cut) takes the scaled path, which applies
// both scales; SUP there drew the ruby at full size.
inline bool drawsWithSup(const GfxRenderer& renderer, const int fontId, const uint16_t scale) {
  return scale == HALF && !renderer.isVectorFont(fontId) && renderer.faceScaleFor(fontId).identity();
}

// Horizontal advance of ruby text. Half size measures the SUP style for a vector font too, so the
// layout does not depend on whether a sized vector face could be loaded.
inline int advance(const GfxRenderer& renderer, const int fontId, const char* text, const int8_t letterSpacing,
                   const uint16_t scale) {
  if (scale == HALF && renderer.faceScaleFor(fontId).identity()) {
    return renderer.getTextAdvanceX(fontId, text, EpdFontFamily::SUP, letterSpacing);
  }
  return renderer.getTextWidthScaled(fontId, text, scale, EpdFontFamily::REGULAR, BidiUtils::BidiBaseDir::AUTO,
                                     letterSpacing);
}

// Draws horizontal ruby. `top` is the y drawText takes, so the baseline sits the body
// ascender below it at every size.
inline void drawHorizontal(const GfxRenderer& renderer, const int fontId, const int x, const int top, const char* text,
                           const bool black, const BidiUtils::BidiBaseDir baseDir, const int8_t letterSpacing,
                           const uint16_t scale) {
  if (drawsWithSup(renderer, fontId, scale)) {
    renderer.drawText(fontId, x, top, text, black, EpdFontFamily::SUP, baseDir, letterSpacing);
    return;
  }
  const int ascender = renderer.getFontAscenderSize(fontId);
  renderer.drawTextScaled(fontId, x, top + ascender - (ascender * scale + 128) / 256, text, scale, black,
                          EpdFontFamily::REGULAR, baseDir, letterSpacing);
}

// The ruby scale for a line: the renderer's setting, the same for every font.
inline uint16_t horizontalScale(const GfxRenderer& renderer, const int /*fontId*/) { return renderer.rubyScale(); }

}  // namespace ruby
