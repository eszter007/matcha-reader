#pragma once

#include <cstddef>
#include <cstdint>

namespace textAdvance {

// CSS tracking is a whole-pixel delta per rendered glyph. Measurement applies this once for
// every glyph in the run; drawing applies the same helper to each cursor step (and the cached
// run advance retains the final glyph's delta for positioning the next separately drawn word).
[[nodiscard]] constexpr int withLetterSpacing(const int baseAdvance, const size_t glyphCount,
                                              const int8_t letterSpacing) {
  return baseAdvance + static_cast<int>(glyphCount) * static_cast<int>(letterSpacing);
}

}  // namespace textAdvance
