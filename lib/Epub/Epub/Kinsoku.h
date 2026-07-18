#pragma once

#include <cstdint>

// Kinsoku shori (禁則処理): rules that forbid certain characters from
// starting or ending a line/column in Japanese typesetting.
//
// This is a deliberately compact, "good enough for v1" rule set covering the
// characters that show up in the vast majority of real-world Japanese prose
// (fiction, light novels, nonfiction). It is NOT the full JIS X 4051 rule
// set (which also covers things like prohibiting splitting numeral groups,
// EAW-class line breaking, etc.) -- that level of completeness is a
// reasonable v2 follow-up once the column-layout plumbing below is proven
// out on a device.
//
// Used by VerticalParsedText to decide whether a character is allowed to be
// the first or last character of a column.
namespace Kinsoku {

// Characters that must NOT appear as the first character of a new column
// (line-start prohibition / 行頭禁則文字).
// Covers: closing brackets/quotes, small kana (sokuon/yoon), the long vowel
// mark, common ideographic punctuation, and the iteration mark.
bool isLineStartProhibited(uint32_t codepoint);

// Characters that must NOT appear as the last character of a column
// (line-end prohibition / 行末禁則文字).
// Covers: opening brackets/quotes.
bool isLineEndProhibited(uint32_t codepoint);

// True for characters that should be drawn upright in vertical text even
// though they're "non-CJK" by codepoint range (e.g. the ideographic full
// stop is technically punctuation but always stays upright). This is used
// to distinguish "upright single cell" characters from "needs 90-degree
// rotation" runs (Latin letters, digits, most Western punctuation).
bool isAlwaysUpright(uint32_t codepoint);

// True for codepoints that should be batched into a sideways-rotated run
// (rendered via GfxRenderer::drawTextRotated90CW) rather than placed
// upright in their own column cell. This is primarily ASCII Latin letters
// and digits, which is the common case of an English word, acronym, or
// number embedded in Japanese running text.
bool isRotatedRunCharacter(uint32_t codepoint);

// Returns a shift category for vertical punctuation positioning:
// 0 = no shift, 1 = comma/period (shift up+right)
int verticalShiftType(uint32_t codepoint);

// True for paired brackets/parens and dashes that should be rotated 90° in
// vertical text rather than drawn upright (they need to open/close or run
// along the vertical axis).
bool needsVerticalRotation(uint32_t codepoint);

// True for small (yoon/sokuon) kana, which in tategaki are shifted toward the
// upper-right of their cell relative to a centered position.
bool isSmallKana(uint32_t codepoint);

}  // namespace Kinsoku
