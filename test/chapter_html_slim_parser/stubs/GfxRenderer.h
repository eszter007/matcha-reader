#pragma once

#include <EpdFontFamily.h>

#include <deque>
#include <string>

class GfxRenderer {
 public:
  class FrameBufferLoan {
   public:
    explicit FrameBufferLoan(GfxRenderer&) {}
  };

  int getScreenWidth() const { return 480; }
  int getScreenHeight() const { return 800; }
  int getLineHeight(int, float = 1.0f) const { return 16; }
  int getFontAscenderSize(int) const { return 12; }
  int getSpaceWidth(int, EpdFontFamily::Style, int8_t = 0) const { return 4; }
  int getTextAdvanceX(int, const char* text, EpdFontFamily::Style, int8_t = 0) const {
    // Per CHARACTER, not per byte: the real renderer advances once per glyph, so counting bytes
    // would measure any non-ASCII text (an em dash, a guillemet) two or three times too wide.
    int width = 0;
    for (const char* p = text; *p != '\0'; ++p) {
      if ((static_cast<unsigned char>(*p) & 0xC0) != 0x80) width += 8;  // skip continuation bytes
    }
    return width;
  }
  int getKerning(int, uint32_t, uint32_t, EpdFontFamily::Style) const { return 0; }
  int getSpaceAdvance(int, uint32_t, uint32_t, EpdFontFamily::Style, int8_t = 0) const { return 4; }
  bool isSdCardFont(int) const { return false; }
  void ensureSdCardFontReady(int, const std::deque<std::string>&, bool, uint8_t) const {}
  // Square 8x8 ink box with a 1px left bearing, sitting on the baseline. Matches the 8px
  // advance getTextAdvanceX reports per character, so a drop cap's reserved column and the
  // words measured beside it stay consistent in the tests.
  bool getGlyphMetrics(int, uint32_t, EpdFontFamily::Style, int* left, int* width, int* top, int* height) const {
    if (left) *left = 1;
    if (width) *width = 8;
    if (top) *top = 8;
    if (height) *height = 8;
    return true;
  }
  bool drawCharUpscaled(int, uint32_t, int, int, int, bool = true, EpdFontFamily::Style = EpdFontFamily::REGULAR,
                        int* = nullptr, int* = nullptr) const {
    return true;
  }
};
