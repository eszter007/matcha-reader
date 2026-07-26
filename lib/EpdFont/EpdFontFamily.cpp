#include "EpdFontFamily.h"

const EpdFont* EpdFontFamily::getFont(const Style style) const {
  // Extract font style bits; render-time overlay bits do not affect font selection.
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
  // The requested font may cover cp without it being RAM-resident: SD fonts keep only the
  // current page's glyphs in their interval table, so hasGlyph() (residency) says no while
  // hasCodepoint() (full coverage index, RAM-only, no card I/O) says yes. Give f its own
  // on-demand load HERE, before the fallback chain. Otherwise a cold glyph walks straight
  // past the selected font to the global fallback -- which is the reader-size companion
  // (loaded at SETTINGS.fontPointSize) -- and a 12pt UI string draws in 14pt glyphs from a
  // different typeface. Device log: a Home title measured 244px through UDDigiKyokasho_12
  // and then drew from NotoSansJP_14, overflowing the card it had just been truncated to fit.
  if (f->hasCodepoint(cp)) return f->getGlyph(cp);
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

const EpdGlyph* EpdFontFamily::getGlyphResident(const uint32_t cp, const Style style) const {
  const EpdFont* f = getFont(style);
  if (f->hasGlyph(cp)) return f->getGlyph(cp);
  if (fallbackFamily) {
    const EpdFont* fbFont = fallbackFamily->getFont(style);
    if (fbFont->hasGlyph(cp)) return fbFont->getGlyph(cp);
  }
  if (globalFallback_ && globalFallback_ != this && globalFallback_ != fallbackFamily) {
    const EpdFont* gf = globalFallback_->getFont(style);
    if (gf->hasGlyph(cp)) return gf->getGlyph(cp);
    // Deliberately NO glyphMissHandler here.
  }
  return nullptr;
}

const EpdFontData* EpdFontFamily::getDataForGlyph(const uint32_t cp, const Style style) const {
  const EpdFont* f = getFont(style);
  if (f->hasGlyph(cp)) return f->data;
  // Mirrors getGlyph()'s own-font on-demand step above -- the pair must resolve to the same
  // font or getGlyphBitmap() indexes one font's glyph array with another's pointer.
  if (f->hasCodepoint(cp)) return f->data;
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
  // Mirror getGlyph()'s last resort exactly: it returns f->getGlyph(cp) (f's replacement
  // glyph), so the data MUST be f's too. Returning fallbackFamily's data here was the one
  // step of the pair that was never mirrored -- it hands the caller a glyph from f paired
  // with fontData from the fallback, and getGlyphBitmap() then computes the glyph index as a
  // pointer difference across two unrelated arrays. With a 1-bit primary and a 2-bit
  // compressed fallback that index sizes a scratch buffer from the wrong glyph's dataLength
  // (ceil(w*h/8)) which compactSingleGlyph then fills at 2 bits per pixel (ceil(w*h/4)) --
  // a heap overflow of roughly the buffer's own size.
  return f->data;
}

bool EpdFontFamily::hasCodepoint(const uint32_t cp, const Style style) const {
  return getFont(style)->hasCodepoint(cp);
}

int8_t EpdFontFamily::getKerning(const uint32_t leftCp, const uint32_t rightCp, const Style style) const {
  return getFont(style)->getKerning(leftCp, rightCp);
}

uint32_t EpdFontFamily::applyLigatures(const uint32_t cp, const char*& text, const Style style) const {
  return getFont(style)->applyLigatures(cp, text);
}
