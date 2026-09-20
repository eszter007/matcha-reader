#pragma once

#include <cstdint>

struct EpdFontData {
  const void* groups = nullptr;
};

// Only ever null-tested by FontCacheManager, so an opaque tag type is enough.
struct EpdGlyph {};

class EpdFontFamily {
 public:
  enum Style : uint8_t { REGULAR = 0, BOLD = 1, ITALIC = 2, BOLD_ITALIC = 3 };

  EpdFontFamily() = default;
  EpdFontFamily(const EpdFontData* regular, const EpdFontData* bold, const EpdFontData* italic,
                const EpdFontData* boldItalic)
      : styleData{regular, bold, italic, boldItalic} {}

  // This fork chains a JP companion family behind the reader font; the tests exercise
  // the built-in path, so there is no fallback and every codepoint counts as resident --
  // both defaults leave FontCacheManager's fallback branches inert.
  const EpdFontFamily* getFallback() const { return fallbackFamily; }
  const EpdGlyph* getGlyphResident(uint32_t, Style = REGULAR) const { return residentGlyph; }

  const EpdFontFamily* fallbackFamily = nullptr;
  const EpdGlyph* residentGlyph = &kResidentGlyph;

  const EpdFontData* getData(Style style) const {
    const uint8_t requested = static_cast<uint8_t>(style) & 0x03;
    if (styleData[requested]) return styleData[requested];
    if ((requested & BOLD) && styleData[BOLD]) return styleData[BOLD];
    if ((requested & ITALIC) && styleData[ITALIC]) return styleData[ITALIC];
    return styleData[REGULAR];
  }

 private:
  static inline const EpdGlyph kResidentGlyph{};
  const EpdFontData* styleData[4] = {};
};
