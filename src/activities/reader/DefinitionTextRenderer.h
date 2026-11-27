#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

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
  std::string reading;  // may end in kPitchSeparator + pitch; use readingText() for the kana
  // Anki furigana syntax from the dictionary's `【reading】｛木[こ]漏[も]れ 日[び]｝`; empty when absent.
  std::string furigana;
  std::string grammar;
  std::string source;
};

// A reading may carry pitch-accent positions after this separator ("こうこう\x1F0"), from the
// dictionary's `【reading】｟0｠`. They are drawn as a line over the high morae, never saved.
constexpr char kPitchSeparator = '\x1F';
// The reading without its pitch positions, for cards. Same kana with a different pitch is a different
// entry, so section de-duplication compares the full value on purpose.
std::string readingText(const std::string& reading);

// Pull the compact dictionary header out of a plain entry so it can be drawn above the senses.
// The dictionaries use `【reading】` followed by bracketed tags such as `[5-dan] [transitive]`.
void extractEntryMetadata(std::string& text, const std::string& fallbackHeadword, EntryMetadata& metadata);

// Turn the dictionary's plain bullet stream into the compact entry layout: numbered gloss rows,
// gray arrow notes, and indented example pairs. It is intentionally plain-text so the existing
// low-allocation wrapper can keep ownership of scrolling and line measurement.
// `headerGrammar` is the tag list drawn above the senses; a sense repeats only tags it lacks.
// `nameEntry` numbers each line of a bullet-less names entry; other bullet-less entries keep their lines.
void formatEntryBody(std::string& text, const std::string& omitLeading = {}, const std::string& headerGrammar = {},
                     bool nameEntry = false);

// Prewarm each paragraph in the style and fallback font the shared wrapper will use.
// This keeps the compressed font below its four page-buffer-slot limit.
void prewarmStyledText(GfxRenderer& renderer, int fontId, const std::string& text);

// Draw the reading at the selected lookup size and the grammar tags at a compact 75% scale.
// Metadata participates in the same scroll offset as the definition; the return value is the
// unscrolled first y-coordinate available for the definition body.
int drawEntryMetadata(const GfxRenderer& renderer, const Rect& body, int fontId, uint16_t scale,
                      const EntryMetadata& metadata, int scrollOffset = 0, int lineHeight = 0);

// Reading and grammar tags wrap to the panel width, so their line count -- which the callers
// use for scroll bookkeeping -- depends on the renderer's measurements, not just on presence.
int entryMetadataLineCount(const GfxRenderer& renderer, const Rect& body, int fontId, uint16_t scale,
                           const EntryMetadata& metadata);

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
// Where one CJK character of a definition was drawn: a stop for a word cursor.
struct CharBox {
  int16_t x;
  int16_t y;
  int16_t width;
  int16_t height;
  uint32_t offset;  // byte offset in the definition text
};

// A line "\uE000<w>x<h> <file>" is a picture from the dictionary folder's images/ (see
// tools/dict_convert --images), drawn centred and counted as the lines its height covers. With
// pageLines set (the caller pages by pageLines lines, the text starting pageLineOffset lines into
// the first page), a picture that would cross a page break starts on the next page instead.
WrapResult drawWrapped(const GfxRenderer& renderer, int fontId, const std::string& text, int textX, int startY,
                       int lineHeight, int maxWidth, int maxY, int scrollOffset, uint16_t scale = 256,
                       std::vector<CharBox>* boxes = nullptr, int pageLines = 0, int pageLineOffset = 0);

// Where a panel last drew its definition with drawWrapped, so a touch can be mapped back to the
// text. Written by the render task, read by the input loop.
struct WrapGeometry {
  bool valid = false;
  int fontId = 0;
  int textX = 0;
  int startY = 0;
  int lineHeight = 0;
  int maxWidth = 0;
  int maxY = 0;
  int scrollOffset = 0;
  uint16_t scale = 256;
  int pageLines = 0;
  int pageLineOffset = 0;
};

// The CJK characters of `text` on screen, in reading order; full-width punctuation is left out.
// Draws nothing. Empty when the heap cannot hold the list. Hold RenderLock: it measures through
// the shared font cache.
void collectCjkBoxes(const GfxRenderer& renderer, const WrapGeometry& geometry, const std::string& text,
                     std::vector<CharBox>& out);

// Byte offset in `text` of the CJK character drawn at (x, y), or npos. Hold RenderLock.
inline size_t hitTest(const GfxRenderer& renderer, const WrapGeometry& geometry, const std::string& text, const int x,
                      const int y) {
  std::vector<CharBox> boxes;
  collectCjkBoxes(renderer, geometry, text, boxes);
  const auto hit = std::find_if(boxes.begin(), boxes.end(), [x, y](const CharBox& box) {
    return x >= box.x && x < box.x + box.width && y >= box.y && y < box.y + box.height;
  });
  return hit != boxes.end() ? hit->offset : std::string::npos;
}

}  // namespace DefinitionText
