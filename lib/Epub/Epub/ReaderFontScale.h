#pragma once

#include <algorithm>
#include <cstdint>

#include "css/CssStyle.h"

/**
 * CSS font-size -> reader font id.
 *
 * The em base is the READER's font size, not a fixed 16px: the user's size setting keeps
 * authority, and a heading is only ever a multiple of whatever body size they chose.
 *
 * The result is snapped to a size that is actually resident. Every distinct size on a page
 * is a separate loaded font, so the candidate set is deliberately tiny: the built-in Noto
 * families ship at 12/14/16/18pt and all four are registered at boot (main.cpp), which caps
 * a chapter at three sizes beyond the body size no matter how many font-size declarations
 * the book carries.
 */

/** Multiplier the declaration asks for relative to body text; 0 when there is none. */
float cssFontSizeScale(const CssStyle& style);

/**
 * Font id for a block whose CSS sets font-size, given the reader's base font.
 * Returns 0 ("keep the base font") when the property is unset, when the snapped size is the
 * base size, or when the base font is an SD-card family -- only one point size of an SD
 * family is resident at a time (SdCardFontManager::currentPointSize), so scaling one would
 * mean loading a second family's metrics mid-chapter.
 */
int cssBlockFontId(const CssStyle& style, int baseFontId);

/**
 * CSS line-height -> a PERCENTAGE of the leading the reader would have used anyway.
 *
 * Accepted forms, all reduced to a multiple of the element's own font size:
 *   - unitless number  `1.4`   -> 1.4   (CSS's preferred form: a multiple of the font size)
 *   - em / rem         `1.3em` -> 1.3
 *   - percentage       `120%`  -> 1.2
 *   - pt / px          `14pt` / `18px` -> normalised through the CSS initial size (12pt / 16px),
 *                      exactly as font-size does, so an absolute declaration stays relative to
 *                      the READER's chosen size instead of pinning text to a fixed pixel count
 *   - `normal`, inherit, initial, unparseable -> 0 (leave the reader's leading alone)
 *
 * The multiple is divided by CSS's `normal` (1.2 em), because the reader's own line advance
 * ALREADY is that: a font's advanceY is its natural, normal-leading line box. So the returned
 * percentage is what the book wants RELATIVE to normal leading, and the caller multiplies it
 * into `getLineHeight(blockFontId, lineCompression)`:
 *
 *   - the em base is the BLOCK's own font size, because that leading comes from the block's own
 *     font id (Phase 1) -- `h1 { line-height: 1.3em }` resolves against the h1, not the body;
 *   - the user's Line Spacing setting is inside the value being scaled, so Tight stays tighter
 *     and Wide stays wider than the same book on the same page;
 *   - and the result is CLAMPED to 80..200% of it, so no book can crush its text together or
 *     spend the whole screen on four lines.
 *
 * Returns 0 when the property is unset -- distinct from 100, which is an explicit `line-height`
 * that happens to compute to the default and must still stop the cascade from inheriting one.
 */
uint8_t cssLineHeightPercent(const CssStyle& style);

/** Percentage band cssLineHeightPercent() clamps into. See its doc for why the user wins. */
constexpr uint8_t CSS_LINE_HEIGHT_MIN_PCT = 80;
constexpr uint8_t CSS_LINE_HEIGHT_MAX_PCT = 200;

/**
 * Apply a cssLineHeightPercent() result to a computed leading. 0 leaves it untouched.
 * Integer, round-to-nearest: this is the ONE value that becomes a line's vertical advance, so
 * layout, the page-fit test and the stored y all use the same number by construction.
 */
[[nodiscard]] inline int applyCssLineHeight(const int leadingPx, const uint8_t percent) {
  if (percent == 0 || leadingPx <= 0) return leadingPx;
  return std::max(1, (leadingPx * static_cast<int>(percent) + 50) / 100);
}
