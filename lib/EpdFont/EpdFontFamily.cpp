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
    const EpdFont* fbFont = fallbackFamily->getFont(style);
    if (fbFont->hasGlyph(cp)) return fbFont->getGlyph(cp);
  }
  // Last resort: try the global fallback (SD card font) which can load
  // any glyph on demand via its glyphMissHandler.
  if (globalFallback_ && globalFallback_ != this && globalFallback_ != fallbackFamily) {
    const EpdFont* gf = globalFallback_->getFont(style);
    if (gf->hasGlyph(cp)) return gf->getGlyph(cp);
    // Try glyphMissHandler directly for codepoints outside the interval table
    if (gf->data->glyphMissHandler) {
      const EpdGlyph* loaded = gf->data->glyphMissHandler(gf->data->glyphMissCtx, cp);
      if (loaded) return loaded;
    }
  }
  return f->getGlyph(cp);
}

const EpdFontData* EpdFontFamily::getDataForGlyph(const uint32_t cp, const Style style) const {
  const EpdFont* f = getFont(style);
  if (f->hasGlyph(cp)) return f->data;
  if (fallbackFamily) {
    const EpdFont* fbFont = fallbackFamily->getFont(style);
    if (fbFont->hasGlyph(cp)) return fbFont->data;
  }
  if (globalFallback_ && globalFallback_ != this && globalFallback_ != fallbackFamily) {
    const EpdFont* gf = globalFallback_->getFont(style);
    if (gf->hasGlyph(cp)) return gf->data;
    // Must mirror getGlyph()'s glyphMissHandler path exactly: getGlyph() and getDataForGlyph()
    // are always called as a pair for the same (cp, style) to resolve one glyph's (pointer, data)
    // -- if getGlyph() finds cp only via this on-demand path (returning a glyph from the SD-card
    // font's overflow buffer, which isn't part of ANY EpdFontData::glyph[] array) but this
    // function doesn't know that and falls through to its own default below, the caller ends up
    // with a glyph pointer from one font paired with fontData from a DIFFERENT font. Confirmed on
    // a real device: that mismatch fed a bogus pointer-difference "glyph index" (e.g. 156280, far
    // beyond any built-in font's actual glyph count) into the built-in-font decompressor, which
    // correctly rejected it as out of range -- silently dropping that one character rather than
    // corrupting memory, but still a real bug in the two functions disagreeing on resolution.
    if (gf->data->glyphMissHandler) {
      const EpdGlyph* loaded = gf->data->glyphMissHandler(gf->data->glyphMissCtx, cp);
      if (loaded) return gf->data;
    }
  }
  if (fallbackFamily) return fallbackFamily->getData(style);
  return f->data;
}

int8_t EpdFontFamily::getKerning(const uint32_t leftCp, const uint32_t rightCp, const Style style) const {
  return getFont(style)->getKerning(leftCp, rightCp);
}

uint32_t EpdFontFamily::applyLigatures(const uint32_t cp, const char*& text, const Style style) const {
  return getFont(style)->applyLigatures(cp, text);
}
