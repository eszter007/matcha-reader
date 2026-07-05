#pragma once

#include <string>

class GfxRenderer;

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

struct WrapResult {
  int totalLines = 0;  // wrapped line count of the whole text (for scroll bookkeeping)
  int linesDrawn = 0;
};

// Wraps `text` (paragraphs separated by '\n'; empty lines render as half-line gaps) to
// maxWidth and draws the lines that fall inside [scrollOffset, maxY). Latin text breaks at
// the last fitting space; CJK breaks per character, keeping sentence-ending punctuation
// attached to its line rather than orphaned at a line start.
WrapResult drawWrapped(GfxRenderer& renderer, int fontId, const std::string& text, int textX, int startY,
                       int lineHeight, int maxWidth, int maxY, int scrollOffset);

}  // namespace DefinitionText
