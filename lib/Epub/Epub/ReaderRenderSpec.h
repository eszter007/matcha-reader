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
  // The Line Spacing setting itself (0 tight .. 2 wide). Vertical text sets its column gap in
  // quarter ems from this step; lineCompression above is the same setting as horizontal reads it.
  uint8_t lineSpacingLevel = 1;
  bool extraParagraphSpacing = false;
  uint8_t paragraphIndentSpaces = 2;
  int8_t characterSpacing = 0;
  uint8_t wordSpacingPercent = 100;
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
  // Matcha, vertical text: the margin right of the text area, bezel inset included. The first
  // column's ruby is drawn there, and where it is narrower than the ruby the layout holds back
  // the difference. Set by the reader like fontId; 0 (the default) reserves the full ruby width.
  uint16_t rightMarginPx = 0;

  bool operator==(const ReaderRenderSpec&) const = default;
};

// Stable signature for clipping anchors. Page-local word ordinals are valid
// only for the exact layout that produced them.
inline uint32_t readerRenderSpecSignature(const ReaderRenderSpec& spec) {
  uint32_t signature = 2166136261U;
  const auto mix = [&signature](const uint32_t value) {
    signature ^= value;
    signature *= 16777619U;
  };
  mix(static_cast<uint32_t>(spec.fontId));
  mix(static_cast<uint32_t>(spec.lineCompression * 1000.0f));
  mix(spec.extraParagraphSpacing);
  mix(static_cast<uint8_t>(spec.characterSpacing));
  mix(spec.wordSpacingPercent);
  mix(spec.paragraphAlignment);
  mix(spec.viewportWidth);
  mix(spec.viewportHeight);
  mix(spec.hyphenationEnabled);
  mix(spec.embeddedStyle);
  mix(spec.imageRendering);
  mix(spec.focusReadingEnabled);
  return signature == 0 ? 1 : signature;
}
