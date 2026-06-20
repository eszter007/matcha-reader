#include "EpdFontFamily.h"

const EpdFont* EpdFontFamily::getFont(const Style style) const {
  // Extract font style bits (ignore UNDERLINE bit for font selection)
  const bool hasBold = (style & BOLD) != 0;
  const bool hasItalic = (style & ITALIC) != 0;

  if (hasBold && hasItalic) {
    if (boldItalic) return boldItalic;
    if (bold) return bold;
    if (italic) return italic;
  } else if (hasBold && bold) {
    return bold;
  } else if (hasItalic && italic) {
    return italic;
  }

  return regular;
}

void EpdFontFamily::getTextDimensions(const char* string, int* w, int* h, const Style style) const {
  getFont(style)->getTextDimensions(string, w, h);
}

const EpdFontData* EpdFontFamily::getData(const Style style) const { return getFont(style)->data; }

const EpdGlyph* EpdFontFamily::getGlyph(const uint32_t cp, const Style style) const {
  const EpdFont* f = getFont(style);
  if (f->hasGlyph(cp)) return f->getGlyph(cp);
  if (fallbackFamily) {
    const EpdGlyph* fbg = fallbackFamily->getGlyph(cp, style);
    if (fbg) return fbg;
  }
  return f->getGlyph(cp);
}

const EpdFontData* EpdFontFamily::getDataForGlyph(const uint32_t cp, const Style style) const {
  const EpdFont* f = getFont(style);
  if (f->hasGlyph(cp)) return f->data;
  if (fallbackFamily) return fallbackFamily->getData(style);
  return f->data;
}

int8_t EpdFontFamily::getKerning(const uint32_t leftCp, const uint32_t rightCp, const Style style) const {
  return getFont(style)->getKerning(leftCp, rightCp);
}

uint32_t EpdFontFamily::applyLigatures(const uint32_t cp, const char*& text, const Style style) const {
  return getFont(style)->applyLigatures(cp, text);
}
