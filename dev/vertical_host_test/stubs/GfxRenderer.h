#pragma once

#include <cstdio>
#include <cstring>
#include <string>

#include "EpdFontFamily.h"

// Fake renderer standing in for the real lib/GfxRenderer/GfxRenderer.h just
// well enough to compile and exercise VerticalParsedText/VerticalTextBlock
// on a desktop machine. Numbers below are rough stand-ins for a ~16px CJK
// font (a real 16px CJK glyph is roughly a 16x16-22x22px cell depending on
// font); they are NOT meant to be visually accurate, only large enough to
// drive the layout algorithm through realistic page/column counts.
class GfxRenderer {
 public:
  int getLineHeight(int /*fontId*/) const { return 22; }
  int getFontAscenderSize(int /*fontId*/) const { return 18; }

  int getTextAdvanceX(int /*fontId*/, const char* text, EpdFontFamily::Style /*style*/) const {
    return static_cast<int>(std::strlen(text)) * 11;
  }

  void drawText(int fontId, int x, int y, const char* text, bool black, EpdFontFamily::Style style) const {
    std::printf("  drawText      font=%d x=%4d y=%4d black=%d style=%d text=\"%s\"\n", fontId, x, y, black, style,
                text);
  }

  void drawTextRotated90CW(int fontId, int x, int y, const char* text, bool black, EpdFontFamily::Style style) const {
    std::printf("  drawRotated   font=%d x=%4d y=%4d black=%d style=%d text=\"%s\"\n", fontId, x, y, black, style,
                text);
  }
};
