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

// A book that asks for 12em would otherwise walk straight off the ladder; clamping first
// keeps the arithmetic in range and makes the "snapped" log meaningful.
constexpr float MIN_SCALE = 0.5f;
constexpr float MAX_SCALE = 4.0f;

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

  const CssLength& len = style.fontSize;
  float scale;
  switch (len.unit) {
    case CssUnit::Em:
    case CssUnit::Rem:
      // rem is treated as em on purpose: the root font size IS the reader's font size here.
      scale = len.value;
      break;
    case CssUnit::Percent:
      scale = len.value / 100.0f;
      break;
    case CssUnit::Points:
      scale = len.value / CSS_INITIAL_PT;
      break;
    case CssUnit::Pixels:
    default:
      scale = len.value / CSS_INITIAL_PX;
      break;
  }
  if (!(scale > 0.0f)) return 0.0f;  // also rejects NaN
  return std::clamp(scale, MIN_SCALE, MAX_SCALE);
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
