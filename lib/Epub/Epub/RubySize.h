#pragma once

#include <algorithm>
#include <cstdint>

// Furigana (ruby) size, relative to its base text. Layout and drawing both use these figures so
// that what a page reserves for ruby is what it draws. Measuring and drawing are in RubyText.h.
namespace ruby {

// 8.8 fixed point, relative to the base text.
inline constexpr uint16_t HALF = 128;
inline constexpr uint8_t SIZE_COUNT = 4;
inline constexpr uint8_t DEFAULT_SIZE = 1;  // 50 %
inline constexpr uint16_t SIZE_SCALES[SIZE_COUNT] = {102, 128, 154, 179};  // 40, 50, 60, 70 %
// Ruby smaller than this is not readable on the panel; a smaller share draws at this size.
inline constexpr int MIN_POINT_SIZE = 8;

constexpr uint16_t scaleForSize(const uint8_t sizeIndex) {
  return SIZE_SCALES[sizeIndex < SIZE_COUNT ? sizeIndex : DEFAULT_SIZE];
}

// The scale ruby draws at over text set at bodyPointSize: `scale`, raised so the ruby is never
// smaller than MIN_POINT_SIZE, that is max(scale, 8 pt / body pt). Points, like the reader's font
// size setting: pixels follow from the body font, so the share of it is all that is needed.
constexpr uint16_t effectiveScale(const uint16_t scale, const int bodyPointSize) {
  if (bodyPointSize <= 0) return scale;
  const int floor = (MIN_POINT_SIZE * 256 + bodyPointSize - 1) / bodyPointSize;
  return static_cast<uint16_t>(std::min<int>(256, std::max<int>(scale, floor)));
}

// v at the ruby scale, rounded down and up. Half size keeps the integer halving used before
// sizes existed, so the default layout is bit-identical.
constexpr int part(const int v, const uint16_t scale) { return scale == HALF ? v / 2 : v * scale / 256; }
constexpr int partUp(const int v, const uint16_t scale) {
  return scale == HALF ? (v + 1) / 2 : (v * scale + 255) / 256;
}

// The vertical column gap (行間): `quarterEms` of the em. Ruby is drawn in this gap beside its
// base column, so with ruby shown at more than half size the gap is never narrower than the ruby
// plus one pixel, and the ruby cannot touch the next column.
constexpr int verticalColumnGapPx(const int emPx, const int quarterEms, const bool rubyShown, const uint16_t scale) {
  const int gap = std::max(2, emPx * quarterEms / 4);
  if (!rubyShown || scale <= HALF) return gap;
  return std::max(gap, partUp(emPx, scale) + 1);
}

}  // namespace ruby
