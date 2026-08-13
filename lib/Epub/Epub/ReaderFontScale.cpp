#include "ReaderFontScale.h"

#include <Logging.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>

#include "../../../src/fontIds.h"

namespace {

// The CSS initial font size, used only to turn absolute units into a multiple of the
// reader's size: 1em = 16px = 12pt at the CSS reference resolution.
constexpr float CSS_INITIAL_PX = 16.0f;
constexpr float CSS_INITIAL_PT = 12.0f;

// pt -> px at the CSS reference resolution (96/72), matching CssLength::toPixels().
constexpr float PT_TO_PX = 1.33f;

// A book that asks for 12em would otherwise walk straight off the ladder; clamping first
// keeps the arithmetic in range and makes the "snapped" log meaningful.
constexpr float MIN_SCALE = 0.5f;
constexpr float MAX_SCALE = 4.0f;

// What CSS `line-height: normal` computes to for a typical text face, and therefore what a
// font's own advanceY (which is what getLineHeight returns) already represents. Dividing the
// declared multiple by it converts "1.3 times the font size" into "1.083 times the leading the
// reader was going to use", which is the only form the layout can act on.
constexpr float CSS_NORMAL_LINE_HEIGHT = 1.2f;

// Reduce a CssLength to a multiple of the element's own font size. Absolute units go through
// the CSS initial size, exactly as cssFontSizeScale does, so `18px` means 1.125 of the READER's
// size rather than 18 device pixels. Returns 0 for a non-positive or NaN value.
float lengthAsFontSizeMultiple(const CssLength& len) {
  float multiple;
  switch (len.unit) {
    case CssUnit::Number:
    case CssUnit::Em:
    case CssUnit::Rem:
      multiple = len.value;
      break;
    case CssUnit::Percent:
      multiple = len.value / 100.0f;
      break;
    case CssUnit::Points:
      multiple = len.value / CSS_INITIAL_PT;
      break;
    case CssUnit::Pixels:
    default:
      multiple = len.value / CSS_INITIAL_PX;
      break;
  }
  return multiple > 0.0f ? multiple : 0.0f;  // the comparison also rejects NaN
}

// The point sizes each built-in reader family is compiled at, ascending. These are the
// ONLY sizes a block can be laid out in: they cost no extra RAM beyond their glyph cache
// because main.cpp registers all of them at boot.
struct SizeStep {
  uint8_t pt;
  int fontId;
};
constexpr SizeStep SERIF_LADDER[] = {
    {12, NOTOSERIF_12_FONT_ID}, {14, NOTOSERIF_14_FONT_ID}, {16, NOTOSERIF_16_FONT_ID}, {18, NOTOSERIF_18_FONT_ID}};
constexpr SizeStep SANS_LADDER[] = {
    {12, NOTOSANS_12_FONT_ID}, {14, NOTOSANS_14_FONT_ID}, {16, NOTOSANS_16_FONT_ID}, {18, NOTOSANS_18_FONT_ID}};

// Ladder containing baseFontId, plus the base's point size. Null when the reader font is
// not a built-in family (SD-card font, or the JP companion): those are resident at exactly
// one size, so there is nothing to snap to.
const SizeStep* ladderFor(const int baseFontId, size_t& count, uint8_t& basePt) {
  for (const auto& step : SERIF_LADDER) {
    if (step.fontId == baseFontId) {
      count = std::size(SERIF_LADDER);
      basePt = step.pt;
      return SERIF_LADDER;
    }
  }
  for (const auto& step : SANS_LADDER) {
    if (step.fontId == baseFontId) {
      count = std::size(SANS_LADDER);
      basePt = step.pt;
      return SANS_LADDER;
    }
  }
  return nullptr;
}

// Nearest entry to `pt`; ties resolve to the smaller size, matching snapToNearestPointSize()
// in src/ReaderFontSizes.cpp (the ladders are the same set as BUILTIN_READER_POINT_SIZES).
const SizeStep& snapToLadder(const SizeStep* ladder, const size_t count, const int pt) {
  size_t best = 0;
  int bestDelta = std::abs(static_cast<int>(ladder[0].pt) - pt);
  for (size_t i = 1; i < count; i++) {
    const int delta = std::abs(static_cast<int>(ladder[i].pt) - pt);
    if (delta < bestDelta) {
      best = i;
      bestDelta = delta;
    }
  }
  return ladder[best];
}

}  // namespace

float cssFontSizeScale(const CssStyle& style) {
  if (!style.hasFontSize()) return 0.0f;

  // rem is treated as em on purpose: the root font size IS the reader's font size here.
  const float scale = lengthAsFontSizeMultiple(style.fontSize);
  if (scale <= 0.0f) return 0.0f;
  return std::clamp(scale, MIN_SCALE, MAX_SCALE);
}

uint8_t cssLineHeightPercent(const CssStyle& style) {
  if (!style.hasLineHeight()) return 0;

  const float multiple = lengthAsFontSizeMultiple(style.lineHeight);
  if (multiple <= 0.0f) return 0;

  // The book's ask, expressed against the leading the reader had already computed for this
  // block -- which is where the user's Line Spacing setting lives, so the clamp below is a band
  // around THEIR value, not around a fixed default.
  const float percent = 100.0f * multiple / CSS_NORMAL_LINE_HEIGHT;
  const auto rounded = static_cast<int>(std::lround(percent));
  return static_cast<uint8_t>(
      std::clamp(rounded, static_cast<int>(CSS_LINE_HEIGHT_MIN_PCT), static_cast<int>(CSS_LINE_HEIGHT_MAX_PCT)));
}

int8_t cssLetterSpacingPx(const CssStyle& style, const float emPx) {
  if (!style.hasLetterSpacing()) return 0;

  const CssLength& len = style.letterSpacing;
  float px;
  switch (len.unit) {
    case CssUnit::Number:
    case CssUnit::Em:
    case CssUnit::Rem:
      // The em base is the BLOCK's own font: `h1 { letter-spacing: .05em }` tracks against the
      // h1's size, which is where the caller's emPx comes from (ChapterHtmlSlimParser resolves
      // the block font id BEFORE computing emSize).
      px = len.value * emPx;
      break;
    case CssUnit::Points:
      px = len.value * PT_TO_PX;
      break;
    case CssUnit::Percent:
      // Not valid CSS for letter-spacing. Treat as no tracking rather than guessing a base.
      return 0;
    case CssUnit::Pixels:
    default:
      px = len.value;
      break;
  }
  if (!(px > -1000.0f && px < 1000.0f)) return 0;  // rejects NaN and absurd values in one test

  const int rounded = static_cast<int>(std::lround(px));
  // Whole pixels, because the value is added AFTER the fixed-point advance is snapped, in both
  // the measurement and the draw -- an integer is what makes those two agree exactly.
  // A declaration that rounds to 0 is honoured as 0: sub-pixel tracking this panel cannot show.
  return static_cast<int8_t>(
      std::clamp(rounded, static_cast<int>(CSS_LETTER_SPACING_MIN_PX), static_cast<int>(CSS_LETTER_SPACING_MAX_PX)));
}

int cssBlockFontId(const CssStyle& style, const int baseFontId) {
  const float scale = cssFontSizeScale(style);
  if (scale <= 0.0f) return 0;

  size_t count = 0;
  uint8_t basePt = 0;
  const SizeStep* ladder = ladderFor(baseFontId, count, basePt);
  if (ladder == nullptr) return 0;

  const int wantedPt = static_cast<int>(std::lround(static_cast<float>(basePt) * scale));
  const SizeStep& step = snapToLadder(ladder, count, wantedPt);
  if (step.pt == basePt) return 0;  // no change: keep the base font id, not a duplicate of it

  if (step.pt != wantedPt) {
    LOG_DBG("CFS", "font-size %.2fx of %upt wants %dpt; snapped to the nearest loaded size %upt", scale, basePt,
            wantedPt, step.pt);
  }
  return step.fontId;
}
