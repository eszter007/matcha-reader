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
    int width = 0;
    while (*text++) width += 8;
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
