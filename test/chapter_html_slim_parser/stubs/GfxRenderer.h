#pragma once

#include <EpdFontFamily.h>
#include <Utf8.h>

#include <deque>
#include <string>

namespace BidiUtils {
enum class BidiBaseDir : signed char { AUTO = -1, LTR = 0, RTL = 1 };
}

class GfxRenderer {
 public:
  enum class TextMeasureMode { Layout, Rendered };
  // The fixture has no framebuffer to lend; image probes run as without a loan.
  class FrameBufferLoan {
   public:
    explicit FrameBufferLoan(GfxRenderer&) {}
    void end() {}
  };
  // Fixture metrics: every glyph is 8 px wide, a space is 4 px, kerning is zero.
  static int trackingBetween(uint32_t left, uint32_t right, int8_t tracking) {
    return left == 0 || left == ' ' || right == ' ' ? 0 : tracking;
  }
  bool isFontCacheScanning() const { return false; }
  void drawLine(int, int, int, int, int, bool) const {}
  // Filled panel backgrounds (PageBox with filled=true); drawing is not what these tests assert.
  void fillRect(int, int, int, int, bool) const {}
  // Signatures mirror the real renderer, trailing defaults included: TextBlock::render() calls
  // these with and without letterSpacing, and the scaled variants carry the fork's per-word
  // scaling (ruby and mixed-size runs).
  void drawText(int, int, int, const char*, bool = true, EpdFontFamily::Style = EpdFontFamily::REGULAR,
                BidiUtils::BidiBaseDir = BidiUtils::BidiBaseDir::AUTO, int8_t = 0) const {}
  void drawTextScaled(int, int, int, const char*, uint16_t, bool = true, EpdFontFamily::Style = EpdFontFamily::REGULAR,
                      BidiUtils::BidiBaseDir = BidiUtils::BidiBaseDir::AUTO, int8_t = 0) const {}
  int getTextWidth(int font, const char* text, EpdFontFamily::Style style,
                   BidiUtils::BidiBaseDir = BidiUtils::BidiBaseDir::AUTO) const {
    return getTextAdvanceX(font, text, style);
  }
  int getTextWidthScaled(int font, const char* text, uint16_t scale,
                         EpdFontFamily::Style style = EpdFontFamily::REGULAR,
                         BidiUtils::BidiBaseDir = BidiUtils::BidiBaseDir::AUTO, int8_t = 0) const {
    return getTextAdvanceX(font, text, style) * scale / 256;
  }
  int getScreenWidth() const { return 480; }
  int getScreenHeight() const { return 800; }
  // Mirrors the real renderer: the line-spacing factor scales the leading.
  int getLineHeight(int, const float compression = 1.0f) const { return static_cast<int>(16 * compression + 0.5f); }
  // 8.8 fixed-point magnification of the leading, for lines carrying a word-scale tag.
  int getLineHeightScaled(const int font, const uint16_t scale) const {
    const int natural = getLineHeight(font);
    if (scale == 256) return natural;
    const int scaled = (natural * scale + 128) / 256;
    return scaled > 1 ? scaled : 1;
  }
  int getFontAscenderSize(int) const { return 12; }
  // Ascender + descender. Deliberately exceeds the 16px advance getLineHeight reports, mirroring
  // the built-in faces whose advanceY is smaller than their own ink.
  int getFontInkHeight(int) const { return 18; }
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
  // The delta the real renderer reports between two glyphs. The stub has no kerning pairs, so
  // tracking (CSS letter-spacing plus the reader's character-spacing setting, which callers
  // combine into this one value) is the whole of it -- returning 0 regardless left every
  // spacing test measuring untracked text.
  int getKerning(int, uint32_t, uint32_t, EpdFontFamily::Style, const int8_t letterSpacing = 0) const {
    return letterSpacing;
  }
  int getSpaceAdvance(int, uint32_t, uint32_t, EpdFontFamily::Style, int8_t = 0) const { return 4; }
  bool isSdCardFont(int) const { return false; }
  void ensureSdCardFontReady(int, const char*, uint8_t = 0x0F) const {}
  // Packed variant: ParsedText hands the layout path whole WordStore chunks rather than
  // per-word strings, so the stub has to accept that shape too.
  void ensureSdCardFontReady(int, const char* const*, const size_t*, size_t, bool, bool, uint8_t = 0x0F) const {}
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
