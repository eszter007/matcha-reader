#include "Kinsoku.h"

namespace Kinsoku {

namespace {

// Closing brackets / quotation marks (both halfwidth-in-fullwidth-box JIS
// punctuation and the CJK bracket block).
bool isClosingBracket(uint32_t cp) {
  switch (cp) {
    case 0x3009:  // 〉
    case 0x300B:  // 》
    case 0x300D:  // 」
    case 0x300F:  // 』
    case 0x3011:  // 】
    case 0x3015:  // 〕
    case 0x3017:  // 〗
    case 0x3019:  // 〙
    case 0x301B:  // 〛
    case 0x301E:  // 〞
    case 0x301F:  // 〟
    case 0xFF09:  // ）
    case 0xFF3D:  // ］
    case 0xFF5D:  // ｝
    case 0x2019:  // ’
    case 0x201D:  // ”
      return true;
    default:
      return false;
  }
}

bool isOpeningBracket(uint32_t cp) {
  switch (cp) {
    case 0x3008:  // 〈
    case 0x300A:  // 《
    case 0x300C:  // 「
    case 0x300E:  // 『
    case 0x3010:  // 【
    case 0x3014:  // 〔
    case 0x3016:  // 〖
    case 0x3018:  // 〘
    case 0x301A:  // 〚
    case 0x301D:  // 〝
    case 0xFF08:  // （
    case 0xFF3B:  // ［
    case 0xFF5B:  // ｛
    case 0x2018:  // ‘
    case 0x201C:  // “
      return true;
    default:
      return false;
  }
}

// Small (yoon/sokuon) kana that cannot start a line, plus the
// chouonpu (long vowel mark) and iteration marks.
bool isSmallKanaOrMark(uint32_t cp) {
  switch (cp) {
    case 0x3041:  // ぁ
    case 0x3043:  // ぃ
    case 0x3045:  // ぅ
    case 0x3047:  // ぇ
    case 0x3049:  // ぉ
    case 0x3063:  // っ
    case 0x3083:  // ゃ
    case 0x3085:  // ゅ
    case 0x3087:  // ょ
    case 0x308E:  // ゎ
    case 0x30A1:  // ァ
    case 0x30A3:  // ィ
    case 0x30A5:  // ゥ
    case 0x30A7:  // ェ
    case 0x30A9:  // ォ
    case 0x30C3:  // ッ
    case 0x30E3:  // ャ
    case 0x30E5:  // ュ
    case 0x30E7:  // ョ
    case 0x30EE:  // ヮ
    case 0x30F5:  // ヵ
    case 0x30F6:  // ヶ
    case 0x30FC:  // ー (chouonpu)
    case 0x3005:  // 々
    case 0x309D:  // ゝ
    case 0x309E:  // ゞ
    case 0x30FD:  // ヽ
    case 0x30FE:  // ヾ
      return true;
    default:
      return false;
  }
}

// Ideographic / fullwidth punctuation that can't start a line: commas,
// fullstops, middle dot, question/exclamation marks, prolonged sound marks.
bool isLineStartPunctuation(uint32_t cp) {
  switch (cp) {
    case 0x3001:  // 、
    case 0x3002:  // 。
    case 0x30FB:  // ・
    case 0xFF0C:  // ，
    case 0xFF0E:  // ．
    case 0xFF1A:  // ：
    case 0xFF1B:  // ；
    case 0xFF1F:  // ？
    case 0xFF01:  // ！
    case 0x2026:  // … (ellipsis)
    case 0x2025:  // ‥
      return true;
    default:
      return false;
  }
}

}  // namespace

bool isLineStartProhibited(uint32_t codepoint) {
  return isClosingBracket(codepoint) || isSmallKanaOrMark(codepoint) || isLineStartPunctuation(codepoint);
}

bool isLineEndProhibited(uint32_t codepoint) { return isOpeningBracket(codepoint); }

bool isAlwaysUpright(uint32_t codepoint) {
  // CJK ideographs, hiragana, katakana, and ideographic punctuation are
  // always drawn upright in tategaki, regardless of the kinsoku rules
  // above (kinsoku only governs *position*, not orientation).
  if (codepoint >= 0x3040 && codepoint <= 0x30FF) return true;  // Hiragana + Katakana
  if (codepoint >= 0x3400 && codepoint <= 0x9FFF) return true;  // CJK Unified + Ext A
  if (codepoint >= 0xF900 && codepoint <= 0xFAFF) return true;  // CJK Compat Ideographs
  if (codepoint >= 0x3000 && codepoint <= 0x303F) return true;  // CJK punctuation
  if (codepoint >= 0xFF00 && codepoint <= 0xFFEF) return true;  // Fullwidth forms
  if (codepoint >= 0x2460 && codepoint <= 0x24FF) return true;  // Enclosed Alphanumerics ①②③
  if (codepoint >= 0x2500 && codepoint <= 0x257F) return true;  // Box Drawing
  if (codepoint >= 0x25A0 && codepoint <= 0x25FF) return true;  // Geometric Shapes ■●▲
  if (codepoint >= 0x2600 && codepoint <= 0x26FF) return true;  // Miscellaneous Symbols ☀☁☂
  if (codepoint >= 0x2700 && codepoint <= 0x27BF) return true;  // Dingbats ✓✗✠
  if (codepoint >= 0x3200 && codepoint <= 0x32FF) return true;  // Enclosed CJK ㈠㊀
  if (codepoint >= 0x3300 && codepoint <= 0x33FF) return true;  // CJK Compatibility ㌀㍻
  return false;
}

bool isRotatedRunCharacter(uint32_t codepoint) {
  // Basic Latin letters and a handful of common inline symbols.
  // Anything in this set gets batched together and rendered sideways via
  // drawTextRotated90CW so an embedded English phrase reads
  // left-to-right when the reader tilts their head (or the device),
  // matching standard tategaki convention.
  if (codepoint >= 'A' && codepoint <= 'Z') return true;
  if (codepoint >= 'a' && codepoint <= 'z') return true;
  if (codepoint == ' ') return true;
  switch (codepoint) {
    case '.':
    case ',':
    case '-':
    case '/':
    case ':':
    case '%':
    case '+':
    case '#':
    case '@':
    case '&':
    case '\'':
    case '"':
    case '(':
    case ')':
    case '*':
    case '!':
    case '?':
    case ';':
    case 0x00A9:  // ©
    case 0x00AE:  // ®
    case 0x2122:  // ™
      return true;
    default:
      return false;
  }
}

// Returns a shift category for vertical punctuation positioning:
// 0 = no shift needed
// 1 = comma/period: shift right and up (bottom-left → upper-right)
// 2 = closing bracket/quote: shift right and up
// 3 = opening bracket/quote: shift up only (already on right side of em-box)
int verticalShiftType(uint32_t cp) {
  switch (cp) {
    case 0x3001:  // 、
    case 0x3002:  // 。
    case 0xFF0C:  // ，
    case 0xFF0E:  // ．
      return 1;
    case 0x3009:  // 〉
    case 0x300B:  // 》
    case 0x300D:  // 」
    case 0x300F:  // 』
    case 0x3011:  // 】
    case 0x3015:  // 〕
    case 0x3017:  // 〗
    case 0x3019:  // 〙
    case 0x301B:  // 〛
    case 0x301E:  // 〞
    case 0x301F:  // 〟
    case 0xFF09:  // ）
    case 0xFF3D:  // ］
    case 0xFF5D:  // ｝
    case 0x2019:  // ’
    case 0x201D:  // ”
      return 2;
    case 0x3008:  // 〈
    case 0x300A:  // 《
    case 0x300C:  // 「
    case 0x300E:  // 『
    case 0x3010:  // 【
    case 0x3014:  // 〔
    case 0x3016:  // 〖
    case 0x3018:  // 〘
    case 0x301A:  // 〚
    case 0x301D:  // 〝
    case 0xFF08:  // （
    case 0xFF3B:  // ［
    case 0xFF5B:  // ｛
    case 0x2018:  // ‘
    case 0x201C:  // “
      return 3;
    case 0x30FC:  // ー prolonged sound mark (chōonpu)
    case 0x2010:  // ‐ hyphen
    case 0x2012:  // ‒ figure dash
    case 0x2013:  // – en dash
    case 0x2014:  // — em dash
    case 0x2015:  // ― horizontal bar
    case 0x2212:  // − minus sign
    case 0xFF0D:  // － fullwidth hyphen-minus
    case 0x301C:  // 〜 wave dash
    case 0xFF5E:  // ～ fullwidth tilde
    case 0x2500:  // ─ box drawings light horizontal
      return 4;
    default:
      return 0;
  }
}

bool needsVerticalRotation(uint32_t cp) {
  switch (cp) {
    case 0x300C:  // 「
    case 0x300D:  // 」
    case 0x300E:  // 『
    case 0x300F:  // 』
    case 0x3008:  // 〈
    case 0x3009:  // 〉
    case 0x300A:  // 《
    case 0x300B:  // 》
    case 0x3010:  // 【
    case 0x3011:  // 】
    case 0x3014:  // 〔
    case 0x3015:  // 〕
    case 0x301D:  // 〝
    case 0x301E:  // 〞
    case 0x301F:  // 〟
    case 0xFF08:  // （
    case 0xFF09:  // ）
    case 0xFF3B:  // ［
    case 0xFF3D:  // ］
    case 0xFF5B:  // ｛
    case 0xFF5D:  // ｝
    case 0x30FC:  // ー prolonged sound mark (chōonpu)
    case 0x2025:  // ‥ two dot leader
    case 0x2026:  // … horizontal ellipsis
    case 0x2010:  // ‐ hyphen
    case 0x2012:  // ‒ figure dash
    case 0x2013:  // – en dash
    case 0x2014:  // — em dash
    case 0x2015:  // ― horizontal bar
    case 0x2212:  // − minus sign
    case 0xFF0D:  // － fullwidth hyphen-minus
    case 0x301C:  // 〜 wave dash
    case 0xFF5E:  // ～ fullwidth tilde
    case 0x2500:  // ─ box drawings light horizontal
      return true;
    default:
      return false;
  }
}

bool isSmallKana(uint32_t cp) {
  switch (cp) {
    case 0x3041:
    case 0x3043:
    case 0x3045:
    case 0x3047:
    case 0x3049:  // ぁぃぅぇぉ
    case 0x3063:  // っ
    case 0x3083:
    case 0x3085:
    case 0x3087:  // ゃゅょ
    case 0x308E:  // ゎ
    case 0x30A1:
    case 0x30A3:
    case 0x30A5:
    case 0x30A7:
    case 0x30A9:  // ァィゥェォ
    case 0x30C3:  // ッ
    case 0x30E3:
    case 0x30E5:
    case 0x30E7:  // ャュョ
    case 0x30EE:  // ヮ
    case 0x30F5:
    case 0x30F6:  // ヵヶ
      return true;
    default:
      return false;
  }
}

}  // namespace Kinsoku
