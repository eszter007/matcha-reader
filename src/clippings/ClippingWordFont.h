#pragma once

#include <Epub/blocks/TextBlock.h>
#include <GfxRenderer.h>

#include <cstdint>

// The font a laid-out word was measured and drawn with: the block's font (or the word's inline
// font-size override), its bitmap scale tag, and the block's CSS letter spacing. Clipping
// geometry must measure and redraw with exactly these, or highlights drift from the page.
struct ClippingWordFont {
  int fontId = 0;
  uint16_t scale = TextBlock::WORD_SCALE_ONE;
  int8_t letterSpacing = 0;
};

inline ClippingWordFont clippingWordFont(const TextBlock& block, const uint16_t i, const int baseFontId) {
  const auto& style = block.getBlockStyle();
  const int32_t wordFont = block.wordFont(i);
  return {wordFont != 0 ? static_cast<int>(wordFont) : style.resolveFontId(baseFontId), block.wordFontScale(i),
          style.letterSpacing};
}

inline int clippingWordWidth(const GfxRenderer& renderer, const ClippingWordFont& font, const char* text,
                             const EpdFontFamily::Style style) {
  if (font.scale == TextBlock::WORD_SCALE_ONE) {
    return renderer.getTextAdvanceX(font.fontId, text, style, font.letterSpacing);
  }
  return renderer.getTextWidthScaled(font.fontId, text, font.scale, style, BidiUtils::BidiBaseDir::AUTO,
                                     font.letterSpacing);
}

inline void clippingDrawWord(const GfxRenderer& renderer, const ClippingWordFont& font, const int x, const int y,
                             const char* text, const EpdFontFamily::Style style) {
  if (font.scale == TextBlock::WORD_SCALE_ONE) {
    renderer.drawText(font.fontId, x, y, text, true, style, BidiUtils::BidiBaseDir::AUTO, font.letterSpacing);
  } else {
    renderer.drawTextScaled(font.fontId, x, y, text, font.scale, true, style, BidiUtils::BidiBaseDir::AUTO,
                            font.letterSpacing);
  }
}
