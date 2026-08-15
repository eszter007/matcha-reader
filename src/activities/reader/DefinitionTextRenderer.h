#pragma once

#include <cstdint>
#include <string>

#include "CrossPointSettings.h"
#include "fontIds.h"

class GfxRenderer;
struct Rect;

// Shared dictionary-definition text wrapping/drawing for the word-lookup activities.
//
// Replaces a per-frame scheme that copied the whole definition, then every paragraph, then
// built a fresh `accum + char` string PER CHARACTER MEASURED -- thousands of alloc/free
// cycles per render, a steady fragmentation source, and an outright abort() under a
// near-exhausted heap (confirmed crash_report: string ctor OOM inside renderContentArea
// while the font decompressor was already failing its 16KB temp buffers).
//
// This version slices the definition by index and reuses ONE line buffer whose single
// guarded reserve happens up front: zero heap growth per line, per paragraph, or per frame.
namespace DefinitionText {

struct EntryMetadata {
  std::string reading;
  std::string grammar;
  std::string source;
};

// Pull the compact dictionary header out of a plain entry so it can be drawn above the senses.
// The dictionaries use `【reading】` followed by bracketed tags such as `[5-dan] [transitive]`.
void extractEntryMetadata(std::string& text, const std::string& fallbackHeadword, EntryMetadata& metadata);

// Turn the dictionary's plain bullet stream into the compact entry layout: numbered gloss rows,
// gray arrow notes, and indented example pairs. It is intentionally plain-text so the existing
// low-allocation wrapper can keep ownership of scrolling and line measurement.
void formatEntryBody(std::string& text, const std::string& omitLeading = {});

// Prewarm each paragraph in the style and fallback font the shared wrapper will use.
// This keeps the compressed font below its four page-buffer-slot limit.
void prewarmStyledText(GfxRenderer& renderer, int fontId, const std::string& text);

// Draw the reading at the selected lookup size and the grammar tags at a compact 75% scale.
// Metadata participates in the same scroll offset as the definition; the return value is the
// unscrolled first y-coordinate available for the definition body.
int drawEntryMetadata(GfxRenderer& renderer, const Rect& body, int fontId, uint16_t scale,
                      const EntryMetadata& metadata, int scrollOffset = 0, int lineHeight = 0);

inline int entryMetadataLineCount(const EntryMetadata& metadata) {
  return (!metadata.reading.empty() ? 1 : 0) + (!metadata.grammar.empty() ? 1 : 0);
}

// Tiny keeps the compact, readable 8pt font. Larger options use native built-in fonts so their
// glyphs stay sharp; CJK characters can be routed to a matching SD-card size by the activity.
// Serif, matching the English definition panel: a dictionary entry reads as the same kind of
// thing whatever the book's language. Japanese glyphs come from the SD-card fallback either way
// (see SdCardFontSystem::ensureWordLookupFallback), so the face chosen here governs the Latin
// text -- the readings, the glosses and the grammar notes.
inline int wordLookupFontId() {
  switch (SETTINGS.wordLookupFontSize) {
    case CrossPointSettings::WORD_LOOKUP_FONT_MEDIUM:
      return NOTOSERIF_14_FONT_ID;
    case CrossPointSettings::WORD_LOOKUP_FONT_LARGE:
      return NOTOSERIF_16_FONT_ID;
    case CrossPointSettings::WORD_LOOKUP_FONT_TINY:
    case CrossPointSettings::WORD_LOOKUP_FONT_SMALL:
    default:
      return NOTOSERIF_12_FONT_ID;
  }
}

inline uint8_t wordLookupFontPointSize() {
  switch (SETTINGS.wordLookupFontSize) {
    case CrossPointSettings::WORD_LOOKUP_FONT_MEDIUM:
      return 14;
    case CrossPointSettings::WORD_LOOKUP_FONT_LARGE:
      return 16;
    case CrossPointSettings::WORD_LOOKUP_FONT_TINY:
    case CrossPointSettings::WORD_LOOKUP_FONT_SMALL:
    default:
      return 12;
  }
}

inline uint16_t wordLookupFontScale() { return 256; }

struct WrapResult {
  int totalLines = 0;  // wrapped line count of the whole text (for scroll bookkeeping)
  int linesDrawn = 0;
};

// Wraps `text` (paragraphs separated by '\n'; empty lines render as half-line gaps) to
// maxWidth and draws the lines that fall inside [scrollOffset, maxY). Latin text breaks at
// the last fitting space; CJK breaks per character, keeping sentence-ending punctuation
// attached to its line rather than orphaned at a line start.
WrapResult drawWrapped(GfxRenderer& renderer, int fontId, const std::string& text, int textX, int startY,
                       int lineHeight, int maxWidth, int maxY, int scrollOffset, uint16_t scale = 256);

}  // namespace DefinitionText
