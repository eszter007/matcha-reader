#pragma once
#include <EpdFontFamily.h>
#include <Utf8.h>

#include <cstddef>
#include <cstdint>
#include <string>
namespace BidiUtils {
enum class BidiBaseDir : signed char { AUTO = -1, LTR = 0, RTL = 1 };
}
// Fixture: a 20px em; every glyph's ink fills 16px centred in its cell.
class GfxRenderer {
 public:
  static constexpr int EM = 20;
  void ensureSdCardFontReady(int, const char*, uint8_t = 0x0F) const {}
  void ensureSdCardFontReady(int, const char* const*, const size_t*, size_t, uint8_t = 0x0F) const {}
  int getTextAdvanceX(int, const char* text, EpdFontFamily::Style, int8_t = 0) const { return count(text) * EM; }
  int getRenderAdvanceX(int, const char* text, EpdFontFamily::Style) const { return count(text) * EM; }
  bool getGlyphMetrics(int, uint32_t, EpdFontFamily::Style, int* left, int* width, int* top, int* height) const {
    *left = 2;
    *width = 16;
    *top = 16;
    *height = 16;
    return true;
  }
  int getFontAscenderSize(int) const { return 16; }
  int getLineHeight(int) const { return EM; }
  int getLineHeight(int, float c) const { return static_cast<int>(EM * c); }
  bool verticalPunctInkBox(int, uint32_t, EpdFontFamily::Style, int cellTopY, int, int, int* inkTop,
                           int* inkHeight) const {
    *inkTop = cellTopY + 2;
    *inkHeight = 16;
    return true;
  }

 private:
  static int count(const char* s) {
    int n = 0;
    for (; *s; s++)
      if ((*s & 0xC0) != 0x80) n++;
    return n;
  }
};
