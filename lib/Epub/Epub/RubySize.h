#pragma once

#include <algorithm>
#include <cstdint>

// Furigana (ruby) size, relative to its base text. Layout and drawing both use these figures so
// that what a page reserves for ruby is what it draws. Measuring and drawing are in RubyText.h.
namespace ruby {

// 8.8 fixed point, relative to the base text.
inline constexpr uint16_t HALF = 128;
inline constexpr uint8_t SIZE_COUNT = 4;
inline constexpr uint8_t DEFAULT_SIZE = 1;                                 // 50 %
inline constexpr uint16_t SIZE_SCALES[SIZE_COUNT] = {102, 128, 154, 179};  // 40, 50, 60, 70 %

constexpr uint16_t scaleForSize(const uint8_t sizeIndex) {
  return SIZE_SCALES[sizeIndex < SIZE_COUNT ? sizeIndex : DEFAULT_SIZE];
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
