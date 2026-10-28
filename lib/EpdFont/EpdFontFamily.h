#pragma once
#include "EpdFont.h"

class EpdFontFamily {
 public:
  // Bitmask of text style flags carried per-word through layout and serialized in page cache.
  // Bits 0-1 select the font variant (BOLD/ITALIC); bits 2-5 are decoration/positioning overlays
  // applied at render time without changing the underlying font. getFont() ignores all bits
  // above bit 1 so decorations compose freely with bold/italic (e.g. BOLD | UNDERLINE | SUP).
  enum Style : uint8_t {
    REGULAR = 0,
    BOLD = 1,
    ITALIC = 2,
    BOLD_ITALIC = 3,
    UNDERLINE = 4,       // drawn as a line below baseline by TextBlock::render()
    STRIKETHROUGH = 8,   // drawn as a line through midline by TextBlock::render()
    SUP = 16,            // superscript: glyph scaled 50%, raised ~40% of ascender
    SUB = 32,            // subscript: glyph scaled 50%, lowered ~25% of ascender
    RUBY_CONTINUE = 64,  // Group ruby follower marker (used internally by Epub layout)
  };
  static constexpr uint8_t TEXT_DECORATION_MASK = static_cast<uint8_t>(UNDERLINE | STRIKETHROUGH);

  // pointSize becomes pixels at FONT_RASTER_DPI.
  explicit EpdFontFamily(uint16_t pointSize, const EpdFont* regular, const EpdFont* bold = nullptr,
                         const EpdFont* italic = nullptr, const EpdFont* boldItalic = nullptr)
      : regular(regular),
        bold(bold),
        italic(italic),
        boldItalic(boldItalic),
        sizeInPixels(pointSize * FONT_RASTER_DPI / 72.0f) {}
  ~EpdFontFamily() = default;
  float getSizeInPixels() const { return sizeInPixels; }
  void getTextDimensions(const char* string, int* w, int* h, Style style = REGULAR) const;
  const EpdFontData* getData(Style style = REGULAR) const;
  // A jōyō kanji nothing can draw is drawn as its other standard form instead (𠮟 as 叱, 填 as 塡):
  // books print either one, and each font carries only one of the pair. tryVariant is that step's
  // recursion guard; getGlyph, getGlyphResident and getDataForGlyph all take it, so they agree.
  const EpdGlyph* getGlyph(uint32_t cp, Style style = REGULAR, bool tryVariant = true) const;
  // Like getGlyph() but never triggers an SD font's on-demand glyphMissHandler (the dictionary
  // variant excepted): resolves only
  // flash-resident and already-loaded glyphs. For MEASUREMENT (layout/indexing) -- getGlyph's
  // miss handler does a seek+read per glyph, which turned chapter indexing into thousands of
  // SD transactions once fallback fonts entered the measurement path.
  const EpdGlyph* getGlyphResident(uint32_t cp, Style style = REGULAR, bool tryVariant = true) const;
  const EpdFontData* getDataForGlyph(uint32_t cp, Style style = REGULAR, bool tryVariant = true) const;
  // getGlyph(), except that a solid block the font lacks becomes a synthetic glyph. Everything
  // else resolves through the fallback chain like getGlyph(), so it pairs with getDataForGlyph():
  // asking only the primary font handed callers one font's glyph with another font's data, and
  // the renderer dropped every glyph a UI font gets from its fallback (all UI Japanese).
  const EpdGlyph* getGlyphMetrics(uint32_t cp, EpdGlyph& solidFallback, Style style = REGULAR) const {
    const EpdFont* f = getFont(style);
    if (syntheticGlyph::isSolid(cp) && !f->hasCodepoint(cp)) return f->getGlyphMetrics(cp, solidFallback);
    return getGlyph(cp, style);
  }
  /// Returns true if the resolved style's font can render `cp` directly
  /// (interval coverage only — see EpdFont::hasCodepoint).
  bool hasCodepoint(uint32_t cp, Style style = REGULAR) const;
  int8_t getKerning(uint32_t leftCp, uint32_t rightCp, Style style = REGULAR) const;
  uint32_t applyLigatures(uint32_t cp, const char*& text, Style style = REGULAR) const;
  static constexpr bool hasTextDecoration(const Style style) {
    return (static_cast<uint8_t>(style) & TEXT_DECORATION_MASK) != 0;
  }

  // nonCjkFirst: take everything that is not CJK from the fallback before this family's own
  // glyphs. For a CJK companion standing in for a built-in face: a line redirected to it for
  // its hanzi keeps its Latin in the face, and the italics, of the lines around it.
  void setFallback(const EpdFontFamily* fb, const bool nonCjkFirst = false) {
    fallbackFamily = fb;
    fallbackFirstForNonCjk = fb != nullptr && nonCjkFirst;
  }
  const EpdFontFamily* getFallback() const { return fallbackFamily; }
  bool fallbackIsFirstForNonCjk() const { return fallbackFirstForNonCjk; }
  // True when cp is drawn from the fallback family rather than this one (see setFallback).
  bool drawsFromFallback(uint32_t cp, Style style = REGULAR) const { return nonCjkFallbackFont(cp, style) != nullptr; }

  static void setGlobalFallback(const EpdFontFamily* fb) { globalFallback_ = fb; }
  static const EpdFontFamily* getGlobalFallback() { return globalFallback_; }
  // The last stops, tried after the global fallback: first the other Chinese script's cut,
  // loaded for a dictionary session so a Simplified book's entry can show the traditional form
  // beside the headword (說 / 说), then a built-in face, so a glyph an SD font lacks still draws when
  // that SD font is itself the global fallback (a subset NotoSansSC without 淨). The variant also
  // loads glyphs for getGlyphResident(); see EpdFontFamily.cpp.
  static void setVariantFallback(const EpdFontFamily* fb) { variantFallback_ = fb; }
  static void setBuiltinFallback(const EpdFontFamily* fb) { builtinFallback_ = fb; }

 private:
  static inline const EpdFontFamily* globalFallback_ = nullptr;
  static inline const EpdFontFamily* builtinFallback_ = nullptr;
  static inline const EpdFontFamily* variantFallback_ = nullptr;
  const EpdFont* regular;
  const EpdFont* bold;
  const EpdFont* italic;
  const EpdFont* boldItalic;
  float sizeInPixels;
  const EpdFontFamily* fallbackFamily = nullptr;
  bool fallbackFirstForNonCjk = false;

  static const EpdGlyph* missingCjkGlyph(const EpdFont* f, uint32_t cp);
  // The fallback's font when it, not this family, supplies cp (see setFallback); else null.
  const EpdFont* nonCjkFallbackFont(uint32_t cp, Style style) const;
  const EpdFont* lastResortFont(uint32_t cp, Style style, const EpdGlyph** glyph) const;

  const EpdFont* getFont(Style style) const;
};
