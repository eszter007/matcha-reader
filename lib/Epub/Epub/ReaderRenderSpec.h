#pragma once
#include <cstdint>

// The resolved text-rendering configuration a reader hands to the layout
// engine. Section-cache validation keys on every field: a section file built
// with a different spec is discarded and rebuilt.
//
// Build one via CrossPointSettings::readerRenderSpec(width, height), which
// fills every field: the settings-derived ones from the store, the viewport
// from the caller. Taking the viewport as arguments is what keeps a spec from
// existing in a half-filled state — the 0 defaults below are a last-resort
// backstop (a 0x0 viewport lays out nothing), not an invitation to omit it.
struct ReaderRenderSpec {
  int fontId = 0;
  float lineCompression = 1.0f;
  bool extraParagraphSpacing = false;
  uint8_t paragraphAlignment = 0;
  uint16_t viewportWidth = 0;
  uint16_t viewportHeight = 0;
  bool hyphenationEnabled = false;
  bool embeddedStyle = true;
  uint8_t imageRendering = 0;
  bool focusReadingEnabled = false;
  // Matcha: honour the book's own horizontal CSS margins. Part of the spec because the
  // section cache keys on it -- a book laid out with and without them differs.
  bool honorBookInsets = false;
  // Matcha: furigana is drawn above the ascender, so a ruby-carrying line reserves extra
  // leading for it. With furigana off that room is empty, and the page reads looser than it
  // needs to -- vertical already tightens for the same reason (see the two column-gap tables
  // in VerticalSection::streamParseAndLayout). Part of the spec because the layout changes:
  // toggling it has to rebuild the section, not just stop drawing the annotations.
  //
  // Per-book (the reader's furigana override), so callers set it like fontId rather than
  // taking it from the store. Layout and drawing must agree on it: TextBlock::render shifts
  // words down by exactly the reserve this adds.
  bool furiganaEnabled = true;
};
