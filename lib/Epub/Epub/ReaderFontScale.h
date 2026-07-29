#pragma once

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
