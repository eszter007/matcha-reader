#include "CssParser.h"

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstring>
#include <string_view>

namespace {

// Stack-allocated string buffer to avoid heap reallocations during parsing
// Provides string-like interface with fixed capacity
struct StackBuffer {
  static constexpr size_t CAPACITY = 1024;
  char data[CAPACITY];
  size_t len = 0;

  void push_back(char c) {
    if (len < CAPACITY - 1) {
      data[len++] = c;
    }
  }

  void clear() { len = 0; }
  bool empty() const { return len == 0; }
  size_t size() const { return len; }

  // Get string view of current content (zero-copy)
  std::string_view view() const { return std::string_view(data, len); }
  operator std::string_view() const noexcept { return view(); }
};

// Buffer size for reading CSS files
constexpr size_t READ_BUFFER_SIZE = 512;

// Maximum number of CSS rules to store in the selector map
// Prevents unbounded memory growth from pathological CSS files
constexpr size_t MAX_RULES = 1500;

// Minimum free heap required to apply CSS during rendering
// If below this threshold, we skip CSS to avoid display artifacts.
constexpr size_t MIN_FREE_HEAP_FOR_CSS = 48 * 1024;

// Maximum length for a single selector string
// Prevents parsing of extremely long or malformed selectors
constexpr size_t MAX_SELECTOR_LENGTH = 256;

// Check if character is CSS whitespace
constexpr bool isCssWhitespace(const char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

constexpr std::string_view trimCssWhitespace(std::string_view s) {
  while (!s.empty() && isCssWhitespace(s.front())) s.remove_prefix(1);
  while (!s.empty() && isCssWhitespace(s.back())) s.remove_suffix(1);
  return s;
}

constexpr char asciiToLower(const char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

// Case-insensitive equality on ASCII. lowercaseKeyword MUST already be
// lowercase; CSS keywords are ASCII by spec so byte-wise tolower is safe.
constexpr bool iequalsAscii(std::string_view value, std::string_view lowercaseKeyword) {
  return std::equal(value.begin(), value.end(), lowercaseKeyword.begin(), lowercaseKeyword.end(),
                    [](char a, char b) { return asciiToLower(a) == b; });
}

// Case-insensitive ASCII substring search. Only needed by text-decoration,
// which accepts multi-value strings like "underline solid red".
constexpr bool icontainsAscii(std::string_view value, std::string_view lowercaseKeyword) {
  return std::search(value.begin(), value.end(), lowercaseKeyword.begin(), lowercaseKeyword.end(),
                     [](char a, char b) { return asciiToLower(a) == b; }) != value.end();
}

// Walk s and invoke fn(token) for each non-empty run between delimiters.
// Tokens are boundary-trimmed and yielded as string_views into s; no
// allocation. Runs of consecutive delimiters coalesce — no empty tokens are
// emitted. `isDelimiter` is invoked once per character.
template <typename Pred, typename F>
void forEachDelimitedToken(std::string_view s, Pred isDelimiter, F&& fn) {
  size_t start = 0;
  for (size_t i = 0; i <= s.size(); ++i) {
    if (i == s.size() || isDelimiter(s[i])) {
      const std::string_view trimmed = trimCssWhitespace(s.substr(start, i - start));
      if (!trimmed.empty()) {
        fn(trimmed);
      }
      start = i + 1;
    }
  }
}

// FNV-1a per Fowler/Noll/Vo, sized to match size_t on the target. The firmware
// runs on a 32-bit core where size_t is 32 bits, so naively using the 64-bit
// constants would silently truncate FNV_PRIME to a non-prime and wreck hash
// distribution. The selection below picks the canonical 32- or 64-bit
// constants at compile time so the same source works in a 64-bit host
// simulator. `fnv1aMix` is the per-byte mix step; callers apply any
// byte-level transform (e.g. asciiToLower) first.
static_assert(sizeof(size_t) == 4 || sizeof(size_t) == 8, "FNV constants are only defined for 32- or 64-bit size_t");
constexpr size_t FNV_OFFSET_BASIS =
    sizeof(size_t) == 8 ? static_cast<size_t>(14695981039346656037ULL) : static_cast<size_t>(2166136261U);
constexpr size_t FNV_PRIME =
    sizeof(size_t) == 8 ? static_cast<size_t>(1099511628211ULL) : static_cast<size_t>(16777619U);

constexpr size_t fnv1aMix(size_t hash, unsigned char byte) { return (hash ^ byte) * FNV_PRIME; }

// Parse the entirety of s as a number into `out`. Accepts an optional leading
// '+' (which std::from_chars rejects by spec) so callers can pass CSS-style
// signed numbers without manual trimming. Returns false on empty input, a
// non-numeric suffix, or any from_chars error.
template <typename T>
bool tryParseNumber(std::string_view s, T& out) {
  const char* begin = s.data();
  const char* end = s.data() + s.size();
  if (begin < end && *begin == '+') ++begin;
  const auto r = std::from_chars(begin, end, out);
  return r.ec == std::errc{} && r.ptr == end;
}

// Collect up to 4 whitespace-separated tokens for a CSS edge-value shorthand
// (margin, padding, and the border-* family). Returns the number of tokens
// written; extras are silently dropped. Callers apply the 1/2/3/4-value
// fallback rule using the returned count.
size_t collectEdgeValueTokens(std::string_view s, std::string_view (&out)[4]) {
  size_t count = 0;
  forEachDelimitedToken(s, isCssWhitespace, [&](std::string_view tok) {
    if (count < 4) out[count++] = tok;
  });
  return count;
}

std::string_view stripTrailingImportant(std::string_view value) {
  constexpr std::string_view IMPORTANT = "!important";

  while (!value.empty() && isCssWhitespace(value.back())) {
    value.remove_suffix(1);
  }

  if (value.size() < IMPORTANT.size()) {
    return value;
  }

  const size_t suffixPos = value.size() - IMPORTANT.size();
  if (!iequalsAscii(value.substr(suffixPos), IMPORTANT)) {
    return value;
  }

  value.remove_suffix(IMPORTANT.size());
  while (!value.empty() && isCssWhitespace(value.back())) {
    value.remove_suffix(1);
  }
  return value;
}

}  // anonymous namespace

// Transparent case-insensitive hash/equal. Bodies live here (rather than
// inline in the header) so they can share the anonymous-namespace asciiToLower
// with the other ASCII helpers in this translation unit.

size_t CssParser::SvHash::operator()(std::string_view sv) const noexcept {
  size_t h = FNV_OFFSET_BASIS;
  for (char c : sv) h = fnv1aMix(h, asciiToLower(c));
  return h;
}

size_t CssParser::SvHash::operator()(const std::string& s) const noexcept { return operator()(std::string_view(s)); }

size_t CssParser::SvHash::operator()(CompositeKey k) const noexcept {
  // Hash the case-folded concatenation of every piece without materializing
  // it — the running hash continues across pieces as if they were one buffer.
  size_t h = FNV_OFFSET_BASIS;
  for (std::string_view piece : k.pieces) {
    for (char c : piece) h = fnv1aMix(h, asciiToLower(c));
  }
  return h;
}

bool CssParser::SvEqual::operator()(std::string_view a, std::string_view b) const noexcept {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (asciiToLower(a[i]) != asciiToLower(b[i])) return false;
  }
  return true;
}

bool CssParser::SvEqual::operator()(const std::string& a, std::string_view b) const noexcept {
  return operator()(std::string_view(a), b);
}

bool CssParser::SvEqual::operator()(std::string_view a, const std::string& b) const noexcept {
  return operator()(a, std::string_view(b));
}

bool CssParser::SvEqual::operator()(const std::string& a, const std::string& b) const noexcept {
  return operator()(std::string_view(a), std::string_view(b));
}

bool CssParser::SvEqual::operator()(CompositeKey k, std::string_view sv) const noexcept {
  size_t total = 0;
  for (std::string_view piece : k.pieces) total += piece.size();
  if (total != sv.size()) return false;
  size_t i = 0;
  for (std::string_view piece : k.pieces) {
    for (char c : piece) {
      if (asciiToLower(c) != asciiToLower(sv[i++])) return false;
    }
  }
  return true;
}

bool CssParser::SvEqual::operator()(std::string_view sv, CompositeKey k) const noexcept { return operator()(k, sv); }

// Property value interpreters

CssTextAlign CssParser::interpretAlignment(std::string_view val) {
  val = trimCssWhitespace(val);

  if (iequalsAscii(val, "left") || iequalsAscii(val, "start")) return CssTextAlign::Left;
  if (iequalsAscii(val, "right") || iequalsAscii(val, "end")) return CssTextAlign::Right;
  if (iequalsAscii(val, "center")) return CssTextAlign::Center;
  if (iequalsAscii(val, "justify")) return CssTextAlign::Justify;

  return CssTextAlign::Left;
}

CssFontStyle CssParser::interpretFontStyle(std::string_view val) {
  val = trimCssWhitespace(val);

  if (iequalsAscii(val, "italic") || iequalsAscii(val, "oblique")) return CssFontStyle::Italic;
  return CssFontStyle::Normal;
}

CssFontWeight CssParser::interpretFontWeight(std::string_view val) {
  val = trimCssWhitespace(val);

  // Named values
  if (iequalsAscii(val, "bold") || iequalsAscii(val, "bolder")) return CssFontWeight::Bold;
  if (iequalsAscii(val, "normal") || iequalsAscii(val, "lighter")) return CssFontWeight::Normal;

  // Numeric values: 100-900
  // CSS spec: 400 = normal, 700 = bold
  // We use: 0-400 = normal, 700+ = bold, 500-600 = normal (conservative)
  long numericWeight = 0;
  if (tryParseNumber(val, numericWeight)) {
    return numericWeight >= 700 ? CssFontWeight::Bold : CssFontWeight::Normal;
  }
  return CssFontWeight::Normal;
}

CssTextDecoration CssParser::interpretDecoration(std::string_view val) {
  // text-decoration can have multiple space-separated values
  if (icontainsAscii(val, "underline")) {
    return CssTextDecoration::Underline;
  }
  return CssTextDecoration::None;
}

CssTextEmphasis CssParser::interpretTextEmphasis(std::string_view val) {
  // Value is fill + shape in either order ("filled sesame", "open dot", ...).
  // "none" wins outright; a missing fill keyword means filled per the CSS spec.
  if (icontainsAscii(val, "none")) return CssTextEmphasis::None;
  const bool open = icontainsAscii(val, "open");
  if (icontainsAscii(val, "sesame")) return open ? CssTextEmphasis::OpenSesame : CssTextEmphasis::FilledSesame;
  if (icontainsAscii(val, "double-circle"))
    return open ? CssTextEmphasis::OpenDoubleCircle : CssTextEmphasis::FilledDoubleCircle;
  if (icontainsAscii(val, "circle")) return open ? CssTextEmphasis::OpenCircle : CssTextEmphasis::FilledCircle;
  if (icontainsAscii(val, "triangle")) return open ? CssTextEmphasis::OpenTriangle : CssTextEmphasis::FilledTriangle;
  if (icontainsAscii(val, "dot")) return open ? CssTextEmphasis::OpenDot : CssTextEmphasis::FilledDot;
  // Bare "filled"/"open" (or a string mark we don't support): default shape.
  // JP bouten convention is the sesame dot, which is also what EBPAJ books use.
  return open ? CssTextEmphasis::OpenSesame : CssTextEmphasis::FilledSesame;
}

CssListStyleType CssParser::interpretListStyleType(std::string_view val) {
  if (icontainsAscii(val, "none")) return CssListStyleType::NoMarker;
  if (icontainsAscii(val, "square")) return CssListStyleType::Square;
  if (icontainsAscii(val, "circle")) return CssListStyleType::Circle;
  // Numbered and alphabetic/roman ordered types all render as decimal numbers.
  if (icontainsAscii(val, "decimal") || icontainsAscii(val, "alpha") || icontainsAscii(val, "roman") ||
      icontainsAscii(val, "latin")) {
    return CssListStyleType::Decimal;
  }
  return CssListStyleType::Disc;
}

CssLength CssParser::interpretLength(std::string_view val) {
  CssLength result;
  tryInterpretLength(val, result);
  return result;
}

bool CssParser::tryInterpretLength(std::string_view val, CssLength& out) {
  val = trimCssWhitespace(val);
  if (val.empty()) {
    out = CssLength{};
    return false;
  }

  size_t unitStart = val.size();
  for (size_t i = 0; i < val.size(); ++i) {
    const char c = val[i];
    if (!std::isdigit(c) && c != '.' && c != '-' && c != '+') {
      unitStart = i;
      break;
    }
  }

  float numericValue;
  if (!tryParseNumber(val.substr(0, unitStart), numericValue)) {
    out = CssLength{};
    return false;  // No number parsed (e.g. auto, inherit, initial)
  }

  const std::string_view unitPart = val.substr(unitStart);
  auto unit = CssUnit::Pixels;
  if (iequalsAscii(unitPart, "em")) {
    unit = CssUnit::Em;
  } else if (iequalsAscii(unitPart, "rem")) {
    unit = CssUnit::Rem;
  } else if (iequalsAscii(unitPart, "pt")) {
    unit = CssUnit::Points;
  } else if (unitPart == "%") {
    unit = CssUnit::Percent;
  }

  out = CssLength{numericValue, unit};
  return true;
}

// Declaration parsing

void CssParser::parseDeclarationIntoStyle(std::string_view decl, CssStyle& style) {
  const size_t colonPos = decl.find(':');
  if (colonPos == std::string_view::npos || colonPos == 0) return;

  const std::string_view name = trimCssWhitespace(decl.substr(0, colonPos));
  const std::string_view value = trimCssWhitespace(decl.substr(colonPos + 1));

  if (name.empty() || value.empty()) return;

  if (iequalsAscii(name, "text-align")) {
    style.textAlign = interpretAlignment(value);
    style.defined.textAlign = 1;
  } else if (iequalsAscii(name, "font-style")) {
    style.fontStyle = interpretFontStyle(value);
    style.defined.fontStyle = 1;
  } else if (iequalsAscii(name, "font-weight")) {
    style.fontWeight = interpretFontWeight(value);
    style.defined.fontWeight = 1;
  } else if (iequalsAscii(name, "text-decoration") || iequalsAscii(name, "text-decoration-line")) {
    style.textDecoration = interpretDecoration(value);
    style.defined.textDecoration = 1;
  } else if (iequalsAscii(name, "text-indent")) {
    style.textIndent = interpretLength(value);
    style.defined.textIndent = 1;
  } else if (iequalsAscii(name, "text-emphasis-style") || iequalsAscii(name, "text-emphasis") ||
             iequalsAscii(name, "-epub-text-emphasis-style") || iequalsAscii(name, "-webkit-text-emphasis-style") ||
             iequalsAscii(name, "-epub-text-emphasis") || iequalsAscii(name, "-webkit-text-emphasis")) {
    style.textEmphasis = interpretTextEmphasis(value);
    style.defined.textEmphasis = 1;
  } else if (iequalsAscii(name, "font-variant") || iequalsAscii(name, "font-variant-caps")) {
    // Only small-caps matters for rendering; anything else resets to normal.
    style.fontVariant =
        (value.find("small-caps") != std::string_view::npos) ? CssFontVariant::SmallCaps : CssFontVariant::Normal;
    style.defined.fontVariant = 1;
  } else if (iequalsAscii(name, "list-style-type") || iequalsAscii(name, "list-style")) {
    style.listStyleType = interpretListStyleType(value);
    style.defined.listStyleType = 1;
  } else if (iequalsAscii(name, "margin-top")) {
    style.marginTop = interpretLength(value);
    style.defined.marginTop = 1;
  } else if (iequalsAscii(name, "margin-bottom")) {
    style.marginBottom = interpretLength(value);
    style.defined.marginBottom = 1;
  } else if (iequalsAscii(name, "margin-left")) {
    style.marginLeft = interpretLength(value);
    style.defined.marginLeft = 1;
  } else if (iequalsAscii(name, "margin-right")) {
    style.marginRight = interpretLength(value);
    style.defined.marginRight = 1;
  } else if (iequalsAscii(name, "margin")) {
    std::string_view margins[4];
    const size_t count = collectEdgeValueTokens(value, margins);
    if (count > 0) {
      style.marginTop = interpretLength(margins[0]);
      style.marginRight = count >= 2 ? interpretLength(margins[1]) : style.marginTop;
      style.marginBottom = count >= 3 ? interpretLength(margins[2]) : style.marginTop;
      style.marginLeft = count >= 4 ? interpretLength(margins[3]) : style.marginRight;
      style.defined.marginTop = style.defined.marginRight = style.defined.marginBottom = style.defined.marginLeft = 1;
    }
  } else if (iequalsAscii(name, "border-style")) {
    // Per-edge mask following the CSS 1/2/3/4-value edge rule (top, right, bottom, left).
    // A full mask is a boxed/kakomi block; a partial mask (e.g. "solid none none none",
    // EBPAJ .k-solid-top) is a separator rule on that edge.
    std::string_view sides[4];
    const size_t count = collectEdgeValueTokens(value, sides);
    uint8_t edges = 0;
    if (count > 0) {
      auto styled = [&](const std::string_view v) { return !iequalsAscii(v, "none") && !iequalsAscii(v, "hidden"); };
      const std::string_view top = sides[0];
      const std::string_view right = count >= 2 ? sides[1] : sides[0];
      const std::string_view bottom = count >= 3 ? sides[2] : sides[0];
      const std::string_view left = count >= 4 ? sides[3] : right;
      if (styled(top)) edges |= CssStyle::BORDER_TOP;
      if (styled(right)) edges |= CssStyle::BORDER_RIGHT;
      if (styled(bottom)) edges |= CssStyle::BORDER_BOTTOM;
      if (styled(left)) edges |= CssStyle::BORDER_LEFT;
    }
    style.borderEdges = edges;
    style.defined.border = 1;
  } else if (iequalsAscii(name, "border")) {
    // Shorthand ("1px solid #000" / "none"): a stroke-style keyword means all four sides.
    const bool styled = value.find("solid") != std::string_view::npos ||
                        value.find("double") != std::string_view::npos ||
                        value.find("dashed") != std::string_view::npos ||
                        value.find("dotted") != std::string_view::npos;
    style.borderEdges = styled ? CssStyle::BORDER_ALL : 0;
    style.defined.border = 1;
  } else if (iequalsAscii(name, "padding-top")) {
    style.paddingTop = interpretLength(value);
    style.defined.paddingTop = 1;
  } else if (iequalsAscii(name, "padding-bottom")) {
    style.paddingBottom = interpretLength(value);
    style.defined.paddingBottom = 1;
  } else if (iequalsAscii(name, "padding-left")) {
    style.paddingLeft = interpretLength(value);
    style.defined.paddingLeft = 1;
  } else if (iequalsAscii(name, "padding-right")) {
    style.paddingRight = interpretLength(value);
    style.defined.paddingRight = 1;
  } else if (iequalsAscii(name, "padding")) {
    std::string_view paddings[4];
    const size_t count = collectEdgeValueTokens(value, paddings);
    if (count > 0) {
      style.paddingTop = interpretLength(paddings[0]);
      style.paddingRight = count >= 2 ? interpretLength(paddings[1]) : style.paddingTop;
      style.paddingBottom = count >= 3 ? interpretLength(paddings[2]) : style.paddingTop;
      style.paddingLeft = count >= 4 ? interpretLength(paddings[3]) : style.paddingRight;
      style.defined.paddingTop = style.defined.paddingRight = style.defined.paddingBottom = style.defined.paddingLeft =
          1;
    }
  } else if (iequalsAscii(name, "height")) {
    CssLength len;
    if (tryInterpretLength(value, len)) {
      style.imageHeight = len;
      style.defined.imageHeight = 1;
    }
  } else if (iequalsAscii(name, "width")) {
    CssLength len;
    if (tryInterpretLength(value, len)) {
      style.imageWidth = len;
      style.defined.imageWidth = 1;
    }
  } else if (iequalsAscii(name, "display")) {
    const std::string_view displayValue = stripTrailingImportant(value);
    style.display = iequalsAscii(displayValue, "none") ? CssDisplay::None : CssDisplay::Block;
    style.defined.display = 1;
  } else if (iequalsAscii(name, "direction")) {
    const std::string_view directionValue = stripTrailingImportant(value);
    if (iequalsAscii(directionValue, "rtl")) {
      style.direction = CssTextDirection::Rtl;
      style.defined.direction = 1;
    } else if (iequalsAscii(directionValue, "ltr")) {
      style.direction = CssTextDirection::Ltr;
      style.defined.direction = 1;
    }
  } else if (iequalsAscii(name, "vertical-align")) {
    if (iequalsAscii(value, "super")) {
      style.verticalAlign = CssVerticalAlign::Super;
      style.defined.verticalAlign = 1;
    } else if (iequalsAscii(value, "sub")) {
      style.verticalAlign = CssVerticalAlign::Sub;
      style.defined.verticalAlign = 1;
    } else {
      // Numeric offsets: publishers commonly write footnote references as
      // ".apnb { vertical-align: 70%; font-size: 60% }" instead of the super/sub keywords.
      // Any positive raise reads as superscript, any negative as subscript; keyword values
      // like baseline/middle fail tryInterpretLength and stay ignored.
      CssLength len;
      if (tryInterpretLength(stripTrailingImportant(value), len) && len.value != 0.0f) {
        style.verticalAlign = len.value > 0 ? CssVerticalAlign::Super : CssVerticalAlign::Sub;
        style.defined.verticalAlign = 1;
      }
    }
  }
}

CssStyle CssParser::parseDeclarations(std::string_view declBlock) {
  CssStyle style;

  size_t start = 0;
  for (size_t i = 0; i <= declBlock.size(); ++i) {
    if (i == declBlock.size() || declBlock[i] == ';') {
      if (i > start) {
        parseDeclarationIntoStyle(declBlock.substr(start, i - start), style);
      }
      start = i + 1;
    }
  }

  return style;
}

// Rule processing

void CssParser::processRuleBlockWithStyle(std::string_view selectorGroup, const CssStyle& style) {
  // With an active incremental cache append, keep the resident map SMALL by flushing it to the
  // cache file periodically -- even mid-stylesheet. One real book's 818-rule stylesheet needs
  // ~100KB as a map, more than the warm-path heap has, so without this the parse truncates at
  // ~90% every time and the (partial) cache is discarded on every open. The map is only a write
  // buffer here: duplicate selectors across flush boundaries become separate records that
  // loadFromCache merges back in order (applyOver), same as a live parse would.
  constexpr size_t CACHE_FLUSH_RULE_THRESHOLD = 200;  // ~25KB resident worst case
  if (cacheAppendActive_ && rulesBySelector_.size() >= CACHE_FLUSH_RULE_THRESHOLD) {
    appendRulesToCache();
    rulesBySelector_.clear();
  }

  // Check if we've reached the rule limit before processing
  if (rulesBySelector_.size() >= MAX_RULES) {
    LOG_DBG("CSS", "Reached max rules limit (%zu), stopping CSS parsing", MAX_RULES);
    return;
  }

  // Walk comma-separated selectors in place — no vector allocation. Selectors
  // with unsupported syntax (combinators, attributes, pseudo, etc.) are skipped
  // silently; the only heap allocation per kept selector is the std::string
  // map key, which is unavoidable since the map owns its keys.
  bool limitReached = false;
  forEachDelimitedToken(
      selectorGroup, [](char c) { return c == ','; },
      [&](std::string_view sel) {
        if (limitReached) return;

        if (sel.size() > MAX_SELECTOR_LENGTH) {
          LOG_DBG("CSS", "Selector too long (%zu > %zu), skipping", sel.size(), MAX_SELECTOR_LENGTH);
          return;
        }

        // TODO: Support richer CSS selector syntax in the future. For now we only
        // handle `tag`, `.class`, or `tag.class`. Reject anything containing a
        // character that introduces unsupported syntax:
        //   '+'  adjacent sibling combinator
        //   '>'  child combinator
        //   '['  attribute selector
        //   ':'  pseudo class/element
        //   '#'  ID selector
        //   '~'  general sibling combinator
        //   '*'  wildcard
        //   ' '  descendant combinator
        // Single-pass scan via find_first_of instead of eight sequential find() calls.
        //
        // ONE descendant form IS supported: the EBPAJ template's writing-mode scoping,
        // `.hltr X` / `.vrtl X` (the body carries class hltr or vrtl). These carry the entire
        // h/v split of the standard Japanese template (margins, indents, rules) -- dropping
        // them dropped all of that styling. They are stored under a scope-prefixed key
        // ("h|X" / "v|X"; '|' can never appear in a real selector) that only scope-aware
        // lookups (resolveStyle's "h|", the vertical collector's "v|") ever find.
        constexpr std::string_view kUnsupportedSelectorChars = "+>[:#~*";
        std::string scopedKey;
        if (const size_t sp = sel.find(' '); sp != std::string_view::npos) {
          const std::string_view first = trimCssWhitespace(sel.substr(0, sp));
          const std::string_view rest = trimCssWhitespace(sel.substr(sp + 1));
          const bool hScope = iequalsAscii(first, ".hltr");
          const bool vScope = iequalsAscii(first, ".vrtl");
          if ((!hScope && !vScope) || rest.empty() ||
              rest.find_first_of(kUnsupportedSelectorChars) != std::string_view::npos ||
              rest.find(' ') != std::string_view::npos) {
            return;  // unsupported descendant selector
          }
          scopedKey.reserve(rest.size() + 2);
          scopedKey += hScope ? "h|" : "v|";
          scopedKey.append(rest.data(), rest.size());
          sel = scopedKey;
        } else if (sel.find_first_of(kUnsupportedSelectorChars) != std::string_view::npos) {
          return;
        }

        // Skip if this would exceed the rule limit
        if (rulesBySelector_.size() >= MAX_RULES) {
          LOG_DBG("CSS", "Reached max rules limit, stopping selector processing");
          limitReached = true;
          return;
        }

        // Store or merge with existing. Hash/equal are case-insensitive, so two
        // selectors that differ only in ASCII case collide on insert and merge.
        auto it = rulesBySelector_.find(sel);
        if (it != rulesBySelector_.end()) {
          it->second.applyOver(style);
        } else {
          // unordered_map::emplace() allocates a hash node internally via bare operator new,
          // which aborts the process on OOM under -fno-exceptions (same hazard as the two sites
          // already fixed in loadFromCache() -- confirmed via a real device crash report:
          // abort() inside this exact emplace() while parsing a large CSS file). Skip the
          // remaining rules in this file rather than crash; already-parsed rules are kept.
          //
          // MIN_FREE_HEAP_FOR_CSS (48KB) is sized for the bulk cache-load path -- using it here
          // for a single hash-node insert (a selector string + CssStyle, a few hundred bytes) was
          // confirmed on a real device to flood-reject nearly every remaining rule the moment free
          // heap dipped anywhere below 48KB, silently discarding most of a chapter's styling.
          constexpr uint32_t MIN_FREE_HEAP_FOR_ONE_RULE = 4 * 1024;
          if (ESP.getMaxAllocHeap() < MIN_FREE_HEAP_FOR_ONE_RULE) {
            LOG_ERR("CSS", "Low heap (%u bytes) while parsing CSS rules; skipping remaining selectors",
                    ESP.getMaxAllocHeap());
            limitReached = true;
            heapTruncated_ = true;  // transient drop -- blocks saveToCache (unlike the MAX_RULES cap)
            return;
          }
          rulesBySelector_.emplace(std::string(sel), style);
        }
      });
}

// Main parsing entry point

bool CssParser::loadFromStream(HalFile& source) {
  if (!source) {
    LOG_ERR("CSS", "Cannot read from invalid file");
    return false;
  }

  size_t totalRead = 0;

  // Use stack-allocated buffers for parsing to avoid heap reallocations
  StackBuffer selector;
  StackBuffer declBuffer;

  bool inComment = false;
  bool maybeSlash = false;
  bool prevStar = false;

  bool inAtRule = false;
  int atDepth = 0;

  int bodyDepth = 0;
  bool skippingRule = false;
  CssStyle currentStyle;

  auto handleChar = [&](const char c) {
    if (inAtRule) {
      if (c == '{') {
        ++atDepth;
      } else if (c == '}') {
        if (atDepth > 0) --atDepth;
        if (atDepth == 0) inAtRule = false;
      } else if (c == ';' && atDepth == 0) {
        inAtRule = false;
      }
      return;
    }

    if (bodyDepth == 0) {
      if (selector.empty() && isCssWhitespace(c)) {
        return;
      }
      if (c == '@' && selector.empty()) {
        inAtRule = true;
        atDepth = 0;
        return;
      }
      if (c == '{') {
        bodyDepth = 1;
        currentStyle = CssStyle{};
        declBuffer.clear();
        if (selector.size() > MAX_SELECTOR_LENGTH * 4) {
          skippingRule = true;
        }
        return;
      }
      selector.push_back(c);
      return;
    }

    // bodyDepth > 0
    if (c == '{') {
      ++bodyDepth;
      return;
    }
    if (c == '}') {
      --bodyDepth;
      if (bodyDepth == 0) {
        if (!skippingRule && !declBuffer.empty()) {
          parseDeclarationIntoStyle(declBuffer, currentStyle);
        }
        if (!skippingRule) {
          processRuleBlockWithStyle(selector, currentStyle);
        }
        selector.clear();
        declBuffer.clear();
        skippingRule = false;
        return;
      }
      return;
    }
    if (bodyDepth > 1) {
      return;
    }
    if (!skippingRule) {
      if (c == ';') {
        if (!declBuffer.empty()) {
          parseDeclarationIntoStyle(declBuffer, currentStyle);
          declBuffer.clear();
        }
      } else {
        declBuffer.push_back(c);
      }
    }
  };

  char buffer[READ_BUFFER_SIZE];
  while (source.available()) {
    int bytesRead = source.read(buffer, sizeof(buffer));
    if (bytesRead <= 0) break;

    totalRead += static_cast<size_t>(bytesRead);

    for (int i = 0; i < bytesRead; ++i) {
      const char c = buffer[i];

      if (inComment) {
        if (prevStar && c == '/') {
          inComment = false;
          prevStar = false;
          continue;
        }
        prevStar = c == '*';
        continue;
      }

      if (maybeSlash) {
        if (c == '*') {
          inComment = true;
          maybeSlash = false;
          prevStar = false;
          continue;
        }
        handleChar('/');
        maybeSlash = false;
        // fall through to process current char
      }

      if (c == '/') {
        maybeSlash = true;
        continue;
      }

      handleChar(c);
    }
  }

  if (maybeSlash) {
    handleChar('/');
  }

  LOG_DBG("CSS", "Parsed %zu rules from %zu bytes", rulesBySelector_.size(), totalRead);
  return true;
}

// Style resolution

CssStyle CssParser::resolveStyle(std::string_view tagName, std::string_view classAttr) const {
  static bool lowHeapWarningLogged = false;
  if (ESP.getMaxAllocHeap() < MIN_FREE_HEAP_FOR_CSS) {
    if (!lowHeapWarningLogged) {
      lowHeapWarningLogged = true;
      LOG_DBG("CSS", "Warning: low heap (%u bytes) below MIN_FREE_HEAP_FOR_CSS (%u), returning empty style",
              ESP.getMaxAllocHeap(), static_cast<unsigned>(MIN_FREE_HEAP_FOR_CSS));
    }
    return CssStyle{};
  }

  CssStyle result;

  // At each cascade level, the base rule applies first and the "h|"-scoped rule (the EBPAJ
  // template's `.hltr X` horizontal-mode variant) applies over it: this resolver feeds the
  // HORIZONTAL layout engine, which renders exactly what those rules describe. The vertical
  // engine reads the "v|" scope through collectVerticalStyles() instead.
  auto applyBoth = [&](auto&&... keyPieces) {
    if (auto it = rulesBySelector_.find(CompositeKey{keyPieces...}); it != rulesBySelector_.end()) {
      result.applyOver(it->second);
    }
    if (auto it = rulesBySelector_.find(CompositeKey{"h|", keyPieces...}); it != rulesBySelector_.end()) {
      result.applyOver(it->second);
    }
  };

  // 1. Apply element-level style (lowest priority). The map's hash/equal are
  // case-insensitive, so the raw tagName view can be used as the lookup key.
  applyBoth(tagName);

  if (classAttr.empty()) return result;

  // TODO: Support combinations of classes (e.g. style on .class1.class2)
  // 2. Apply class styles (medium priority). The transparent hash/equal accept
  // a CompositeKey, so we never materialize the concatenation.
  forEachDelimitedToken(classAttr, isCssWhitespace, [&](std::string_view cls) { applyBoth(".", cls); });

  // TODO: Support combinations of classes (e.g. style on p.class1.class2)
  // 3. Apply element.class styles (higher priority).
  forEachDelimitedToken(classAttr, isCssWhitespace, [&](std::string_view cls) { applyBoth(tagName, ".", cls); });

  return result;
}

// Inline style parsing (static - doesn't need rule database)

CssStyle CssParser::parseInlineStyle(std::string_view styleValue) { return parseDeclarations(styleValue); }

// Cache serialization

// Cache file name (version is CssParser::CSS_CACHE_VERSION)
constexpr char rulesCache[] = "/css_rules.cache";

bool CssParser::hasCache() const { return Storage.exists((cachePath + rulesCache).c_str()); }

void CssParser::deleteCache() const {
  if (hasCache()) Storage.remove((cachePath + rulesCache).c_str());
}

bool CssParser::saveToCache() const {
  if (cachePath.empty()) {
    return false;
  }
  // A heap-truncated parse dropped selectors mid-file. Persisting the partial table would make
  // it THE styling for every future open of this book (confirmed on an X3: 604 of 818 rules
  // cached after a low-heap re-parse). Skip the save; the next open re-parses with -- ideally --
  // a healthier heap.
  if (heapTruncated_) {
    LOG_ERR("CSS", "Parse was heap-truncated (%zu rules); refusing to cache partial rule table",
            rulesBySelector_.size());
    return false;
  }

  HalFile file;
  if (!Storage.openFileForWrite("CSS", cachePath + rulesCache, file)) {
    return false;
  }

  // Write version
  file.write(CssParser::CSS_CACHE_VERSION);

  // Write rule count
  const auto ruleCount = static_cast<uint16_t>(rulesBySelector_.size());
  file.write(reinterpret_cast<const uint8_t*>(&ruleCount), sizeof(ruleCount));

  // Write each rule: selector string + CssStyle fields
  for (const auto& pair : rulesBySelector_) {
    writeRuleRecord(file, pair.first, pair.second);
  }

  LOG_DBG("CSS", "Saved %u rules to cache", ruleCount);
  return true;
}

// One serialized rule record: selectorLen(2) + selector + 5 enum bytes + 11 CssLength
// (float value + unit byte) + display + verticalAlign + borderEdges + textEmphasis +
// fontVariant + listStyleType + definedBits(4).
void CssParser::writeRuleRecord(HalFile& file, const std::string& selector, const CssStyle& style) {
  const auto selectorLen = static_cast<uint16_t>(selector.size());
  file.write(reinterpret_cast<const uint8_t*>(&selectorLen), sizeof(selectorLen));
  file.write(reinterpret_cast<const uint8_t*>(selector.data()), selectorLen);

  file.write(static_cast<uint8_t>(style.textAlign));
  file.write(static_cast<uint8_t>(style.fontStyle));
  file.write(static_cast<uint8_t>(style.fontWeight));
  file.write(static_cast<uint8_t>(style.textDecoration));
  file.write(static_cast<uint8_t>(style.direction));

  auto writeLength = [&file](const CssLength& len) {
    file.write(reinterpret_cast<const uint8_t*>(&len.value), sizeof(len.value));
    file.write(static_cast<uint8_t>(len.unit));
  };

  writeLength(style.textIndent);
  writeLength(style.marginTop);
  writeLength(style.marginBottom);
  writeLength(style.marginLeft);
  writeLength(style.marginRight);
  writeLength(style.paddingTop);
  writeLength(style.paddingBottom);
  writeLength(style.paddingLeft);
  writeLength(style.paddingRight);
  writeLength(style.imageHeight);
  writeLength(style.imageWidth);
  file.write(static_cast<uint8_t>(style.display));
  file.write(static_cast<uint8_t>(style.verticalAlign));
  file.write(style.borderEdges);
  file.write(static_cast<uint8_t>(style.textEmphasis));
  file.write(static_cast<uint8_t>(style.fontVariant));
  file.write(static_cast<uint8_t>(style.listStyleType));

  uint32_t definedBits = 0;
  if (style.defined.textAlign) definedBits |= 1 << 0;
  if (style.defined.fontStyle) definedBits |= 1 << 1;
  if (style.defined.fontWeight) definedBits |= 1 << 2;
  if (style.defined.textDecoration) definedBits |= 1 << 3;
  if (style.defined.textIndent) definedBits |= 1 << 4;
  if (style.defined.marginTop) definedBits |= 1 << 5;
  if (style.defined.marginBottom) definedBits |= 1 << 6;
  if (style.defined.marginLeft) definedBits |= 1 << 7;
  if (style.defined.marginRight) definedBits |= 1 << 8;
  if (style.defined.paddingTop) definedBits |= 1 << 9;
  if (style.defined.paddingBottom) definedBits |= 1 << 10;
  if (style.defined.paddingLeft) definedBits |= 1 << 11;
  if (style.defined.paddingRight) definedBits |= 1 << 12;
  if (style.defined.imageHeight) definedBits |= 1 << 13;
  if (style.defined.imageWidth) definedBits |= 1 << 14;
  if (style.defined.display) definedBits |= 1 << 15;
  if (style.defined.direction) definedBits |= 1 << 16;
  if (style.defined.verticalAlign) definedBits |= 1 << 17;
  if (style.defined.border) definedBits |= 1 << 18;
  if (style.defined.textEmphasis) definedBits |= 1 << 19;
  if (style.defined.fontVariant) definedBits |= 1 << 20;
  if (style.defined.listStyleType) definedBits |= 1 << 21;
  file.write(reinterpret_cast<const uint8_t*>(&definedBits), sizeof(definedBits));
}

// See CssParser.h. Runs at every book open in place of a full loadFromCache -- must not
// allocate; a heavy book's rule map (818 rules) is what used to fail here.
bool CssParser::validateCache() const {
  if (cachePath.empty()) return false;

  HalFile file;
  if (!Storage.openFileForRead("CSS", cachePath + rulesCache, file)) return false;

  uint8_t version = 0;
  if (file.read(&version, 1) != 1 || version != CssParser::CSS_CACHE_VERSION) {
    LOG_DBG("CSS", "Cache version mismatch (got %u, expected %u), removing stale cache for rebuild", version,
            CssParser::CSS_CACHE_VERSION);
    file.close();
    Storage.remove((cachePath + rulesCache).c_str());
    return false;
  }

  uint16_t ruleCount = 0;
  if (file.read(&ruleCount, sizeof(ruleCount)) != sizeof(ruleCount)) return false;
  if (ruleCount == 0 || ruleCount > MAX_RULES) {
    LOG_DBG("CSS", "Invalid cache rule count (%u)", ruleCount);
    return false;
  }

  // selectorLen is followed by this many fixed bytes (see writeRuleRecord).
  constexpr size_t RULE_FIXED_BYTES = 5 + 11 * (sizeof(float) + 1) + 6 + sizeof(uint32_t);
  for (uint16_t i = 0; i < ruleCount; ++i) {
    uint16_t selectorLen = 0;
    if (file.read(&selectorLen, sizeof(selectorLen)) != sizeof(selectorLen)) return false;
    if (selectorLen == 0 || selectorLen > MAX_SELECTOR_LENGTH) {
      LOG_DBG("CSS", "Invalid selector length in cache: %u", selectorLen);
      return false;
    }
    const size_t skip = selectorLen + RULE_FIXED_BYTES;
    if (static_cast<size_t>(file.available()) < skip) {
      LOG_DBG("CSS", "Truncated CSS cache at rule %u/%u", i, ruleCount);
      return false;
    }
    if (!file.seekCur(static_cast<int64_t>(skip))) return false;
  }
  return true;
}

// Stream the on-disk rules cache and collect (selector -> VerticalBlockStyle) for selectors
// with vertical-relevant properties. Unscoped rules and the EBPAJ "v|" scope both apply (the
// "v|" record's fields override the unscoped ones per property); "h|"-scoped rules are the
// horizontal engine's business (resolveStyle). No rule map is materialized.
size_t CssParser::collectVerticalStyles(std::vector<std::pair<std::string, VerticalBlockStyle>>& out,
                                        const size_t maxOut) const {
  out.clear();
  if (cachePath.empty()) return 0;

  HalFile file;
  if (!Storage.openFileForRead("CSS", cachePath + rulesCache, file)) return 0;

  uint8_t version = 0;
  if (file.read(&version, 1) != 1 || version != CssParser::CSS_CACHE_VERSION) return 0;
  uint16_t ruleCount = 0;
  if (file.read(&ruleCount, sizeof(ruleCount)) != sizeof(ruleCount)) return 0;
  if (ruleCount > MAX_RULES) return 0;

  auto emOf = [](const float v, const uint8_t unit) -> float {
    return unit == static_cast<uint8_t>(CssUnit::Em) || unit == static_cast<uint8_t>(CssUnit::Rem) ? v : 0.0f;
  };

  std::string selector;
  for (uint16_t i = 0; i < ruleCount && out.size() < maxOut; i++) {
    uint16_t selectorLen = 0;
    if (file.read(&selectorLen, sizeof(selectorLen)) != sizeof(selectorLen)) break;
    if (selectorLen == 0 || selectorLen > MAX_SELECTOR_LENGTH) break;
    selector.resize(selectorLen);
    if (file.read(selector.data(), selectorLen) != selectorLen) break;

    uint8_t enums[5];  // textAlign, fontStyle, fontWeight, textDecoration, direction
    if (file.read(enums, 5) != 5) break;
    struct RawLen {
      float v;
      uint8_t u;
    } lens[11];  // textIndent, mT, mB, mL, mR, pT, pB, pL, pR, imgH, imgW
    bool lenOk = true;
    for (auto& l : lens) {
      if (file.read(&l.v, sizeof(float)) != sizeof(float) || file.read(&l.u, 1) != 1) {
        lenOk = false;
        break;
      }
    }
    if (!lenOk) break;
    uint8_t displayVal, verticalAlignVal, borderVal;
    uint8_t emphasisVal, variantVal, listTypeVal;  // v11 record tail; unused by the vertical engine
    uint32_t definedBits = 0;
    if (file.read(&displayVal, 1) != 1 || file.read(&verticalAlignVal, 1) != 1 || file.read(&borderVal, 1) != 1 ||
        file.read(&emphasisVal, 1) != 1 || file.read(&variantVal, 1) != 1 || file.read(&listTypeVal, 1) != 1 ||
        file.read(&definedBits, sizeof(definedBits)) != sizeof(definedBits)) {
      break;
    }

    // Only unscoped and "v|"-scoped selectors feed the vertical engine.
    if (selector.size() >= 2 && selector[1] == '|') {
      if (selector[0] != 'v') continue;
      selector.erase(0, 2);
    }

    VerticalBlockStyle vs;
    const bool defTextIndent = definedBits & (1u << 4);
    const bool defMarginTop = definedBits & (1u << 5);
    const bool defMarginLeft = definedBits & (1u << 7);
    const bool defMarginRight = definedBits & (1u << 8);
    const bool defPaddingTop = definedBits & (1u << 9);
    if (defMarginTop) vs.startEm = emOf(lens[1].v, lens[1].u);
    if (defMarginRight) vs.beforeEm = emOf(lens[4].v, lens[4].u);
    if (defMarginLeft) vs.afterEm = emOf(lens[3].v, lens[3].u);
    if (defPaddingTop && defTextIndent && emOf(lens[0].v, lens[0].u) < 0) {
      vs.hangEm = emOf(lens[5].v, lens[5].u);
    }
    if ((definedBits & (1u << 0)) && enums[0] == static_cast<uint8_t>(CssTextAlign::Center)) vs.alignCenter = true;
    if (definedBits & (1u << 18)) vs.borderEdges = borderVal;
    if (!vs.any()) continue;

    // Merge with an existing entry for the same selector (later record overrides per property).
    bool merged = false;
    for (auto& [sel, existing] : out) {
      if (sel == selector) {
        if (vs.startEm > 0) existing.startEm = vs.startEm;
        if (vs.beforeEm > 0) existing.beforeEm = vs.beforeEm;
        if (vs.afterEm > 0) existing.afterEm = vs.afterEm;
        if (vs.hangEm > 0) existing.hangEm = vs.hangEm;
        existing.alignCenter = existing.alignCenter || vs.alignCenter;
        if (vs.borderEdges != 0) existing.borderEdges = vs.borderEdges;
        merged = true;
        break;
      }
    }
    if (!merged) out.emplace_back(selector, vs);
  }
  if (out.size() >= maxOut) {
    LOG_ERR("CSS", "collectVerticalStyles hit the %u-entry cap; later rules (e.g. borders) may be dropped",
            static_cast<unsigned>(maxOut));
  }
  return out.size();
}

bool CssParser::beginCacheAppend() {
  appendedRuleCount_ = 0;
  cacheAppendActive_ = false;
  if (cachePath.empty()) return false;
  if (!Storage.openFileForWrite("CSS", cachePath + rulesCache, cacheAppendFile_)) return false;
  cacheAppendFile_.write(CssParser::CSS_CACHE_VERSION);
  // Rule-count placeholder; patched by endCacheAppend (write mode is O_RDWR, not append, so the
  // seek-back write lands in place -- same pattern as VerticalSection's header patch).
  const uint16_t placeholder = 0;
  cacheAppendFile_.write(reinterpret_cast<const uint8_t*>(&placeholder), sizeof(placeholder));
  cacheAppendActive_ = true;
  return true;
}

bool CssParser::appendRulesToCache() {
  if (!cacheAppendActive_) return false;
  for (const auto& pair : rulesBySelector_) {
    if (appendedRuleCount_ >= MAX_RULES) {
      LOG_DBG("CSS", "Reached max rules limit (%zu) while caching, dropping remainder", MAX_RULES);
      break;
    }
    writeRuleRecord(cacheAppendFile_, pair.first, pair.second);
    ++appendedRuleCount_;
  }
  return true;
}

bool CssParser::endCacheAppend(const bool discard) {
  if (!cacheAppendActive_) return false;
  cacheAppendActive_ = false;
  if (discard || appendedRuleCount_ == 0) {
    cacheAppendFile_.close();
    Storage.remove((cachePath + rulesCache).c_str());
    return false;
  }
  if (!cacheAppendFile_.seek(1)) {  // patch the rule-count placeholder after the version byte
    cacheAppendFile_.close();
    Storage.remove((cachePath + rulesCache).c_str());
    return false;
  }
  cacheAppendFile_.write(reinterpret_cast<const uint8_t*>(&appendedRuleCount_), sizeof(appendedRuleCount_));
  cacheAppendFile_.close();
  LOG_DBG("CSS", "Saved %u rules to cache (incremental)", appendedRuleCount_);
  return true;
}

bool CssParser::loadFromCache(const std::vector<std::string>* usedClasses) {
  cacheLoadFailedForHeap_ = false;
  if (cachePath.empty()) {
    return false;
  }

  HalFile file;
  if (!Storage.openFileForRead("CSS", cachePath + rulesCache, file)) {
    return false;
  }

  // Clear existing rules
  clear();

  // Read and verify version
  uint8_t version = 0;
  if (file.read(&version, 1) != 1 || version != CssParser::CSS_CACHE_VERSION) {
    LOG_DBG("CSS", "Cache version mismatch (got %u, expected %u), removing stale cache for rebuild", version,
            CssParser::CSS_CACHE_VERSION);
    // Explicitly close() file before calling Storage.remove()
    file.close();
    Storage.remove((cachePath + rulesCache).c_str());
    return false;
  }

  // Read rule count
  uint16_t ruleCount = 0;
  if (file.read(&ruleCount, sizeof(ruleCount)) != sizeof(ruleCount)) {
    return false;
  }

  if (ruleCount > MAX_RULES) {
    LOG_DBG("CSS", "Invalid cache rule count (%u > %zu)", ruleCount, MAX_RULES);
    rulesBySelector_.clear();
    return false;
  }

  auto hasRemainingBytes = [&file](const size_t neededBytes) -> bool {
    return static_cast<size_t>(file.available()) >= neededBytes;
  };

  constexpr size_t CSS_LENGTH_FIELD_COUNT = 11;
  constexpr size_t CSS_LENGTH_BYTES = sizeof(float) + sizeof(uint8_t);
  constexpr size_t CSS_FIXED_STYLE_BYTES =
      5 * sizeof(uint8_t) + (CSS_LENGTH_FIELD_COUNT * CSS_LENGTH_BYTES) + sizeof(uint8_t) + sizeof(uint32_t);

  // Below this, `rulesBySelector_[selector] = style` allocates a new map node every iteration.
  // std::map's internal allocator (like every other unguarded STL allocation in this loop) aborts
  // the whole process on OOM under -fno-exceptions -- there is no way to catch that at the call
  // site. Bail out gracefully (drop the cache, fall back to unstyled rendering) instead of letting
  // a large SD-card font's memory footprint plus CSS rule growth crash the device mid-loop.
  // Reuses MIN_FREE_HEAP_FOR_CSS (see file-scope constant) for consistency with the same guard
  // in processRuleBlockWithStyle() -- a device crash at ~28KB free showed the earlier 24KB
  // threshold here cut it too close.

  // Read each rule
  for (uint16_t i = 0; i < ruleCount; ++i) {
    if (ESP.getMaxAllocHeap() < MIN_FREE_HEAP_FOR_CSS) {
      LOG_ERR("CSS", "Low heap (%u bytes) while loading CSS cache at rule %u/%u; aborting cache load",
              ESP.getMaxAllocHeap(), i, ruleCount);
      rulesBySelector_.clear();
      cacheLoadFailedForHeap_ = true;  // cache file is VALID -- caller must not delete/rebuild it
      return false;
    }

    // Read selector string
    uint16_t selectorLen = 0;
    if (!hasRemainingBytes(sizeof(selectorLen))) {
      rulesBySelector_.clear();
      return false;
    }
    if (file.read(&selectorLen, sizeof(selectorLen)) != sizeof(selectorLen)) {
      rulesBySelector_.clear();
      return false;
    }

    if (selectorLen == 0 || selectorLen > MAX_SELECTOR_LENGTH || !hasRemainingBytes(selectorLen)) {
      LOG_DBG("CSS", "Invalid selector length in cache: %u", selectorLen);
      rulesBySelector_.clear();
      return false;
    }

    // Read into a nothrow-allocated buffer first, not directly into a resized std::string:
    // std::string::resize()'s internal allocator calls bare operator new, which aborts the
    // process on OOM under -fno-exceptions instead of returning an error (see the identical
    // fix applied to FontDecompressor::getBitmap() this session). selectorLen is bounded to
    // MAX_SELECTOR_LENGTH above, so this is only reachable under genuine heap exhaustion
    // (e.g. a large SD-card font's kern/advance tables competing for the same heap) -- in
    // that case we want to fail this cache load gracefully, not crash the whole device.
    auto selectorBuf = makeUniqueNoThrow<char[]>(selectorLen);
    if (!selectorBuf) {
      LOG_ERR("CSS", "OOM allocating %u-byte selector buffer during cache load", selectorLen);
      rulesBySelector_.clear();
      return false;
    }
    if (file.read(reinterpret_cast<uint8_t*>(selectorBuf.get()), selectorLen) != selectorLen) {
      rulesBySelector_.clear();
      return false;
    }
    std::string selector(selectorBuf.get(), selectorLen);

    if (!hasRemainingBytes(CSS_FIXED_STYLE_BYTES)) {
      LOG_DBG("CSS", "Truncated CSS cache while reading style payload");
      rulesBySelector_.clear();
      return false;
    }

    // Read CssStyle fields
    CssStyle style;
    uint8_t enumVal;

    if (file.read(&enumVal, 1) != 1) {
      rulesBySelector_.clear();
      return false;
    }
    style.textAlign = static_cast<CssTextAlign>(enumVal);

    if (file.read(&enumVal, 1) != 1) {
      rulesBySelector_.clear();
      return false;
    }
    style.fontStyle = static_cast<CssFontStyle>(enumVal);

    if (file.read(&enumVal, 1) != 1) {
      rulesBySelector_.clear();
      return false;
    }
    style.fontWeight = static_cast<CssFontWeight>(enumVal);

    if (file.read(&enumVal, 1) != 1) {
      rulesBySelector_.clear();
      return false;
    }
    style.textDecoration = static_cast<CssTextDecoration>(enumVal);

    if (file.read(&enumVal, 1) != 1) {
      rulesBySelector_.clear();
      return false;
    }
    style.direction = static_cast<CssTextDirection>(enumVal);

    // Read CssLength fields
    auto readLength = [&file](CssLength& len) -> bool {
      if (file.read(&len.value, sizeof(len.value)) != sizeof(len.value)) {
        return false;
      }
      uint8_t unitVal;
      if (file.read(&unitVal, 1) != 1) {
        return false;
      }
      len.unit = static_cast<CssUnit>(unitVal);
      return true;
    };

    if (!readLength(style.textIndent) || !readLength(style.marginTop) || !readLength(style.marginBottom) ||
        !readLength(style.marginLeft) || !readLength(style.marginRight) || !readLength(style.paddingTop) ||
        !readLength(style.paddingBottom) || !readLength(style.paddingLeft) || !readLength(style.paddingRight) ||
        !readLength(style.imageHeight) || !readLength(style.imageWidth)) {
      rulesBySelector_.clear();
      return false;
    }

    // Read display value
    uint8_t displayVal;
    if (file.read(&displayVal, 1) != 1) {
      rulesBySelector_.clear();
      return false;
    }
    style.display = static_cast<CssDisplay>(displayVal);

    // Read verticalAlign value
    uint8_t verticalAlignVal;
    if (file.read(&verticalAlignVal, 1) != 1) {
      rulesBySelector_.clear();
      return false;
    }
    style.verticalAlign = static_cast<CssVerticalAlign>(verticalAlignVal);

    // Read border edge mask (v9+)
    uint8_t borderVal;
    if (file.read(&borderVal, 1) != 1) {
      rulesBySelector_.clear();
      return false;
    }
    style.borderEdges = borderVal;

    // Read textEmphasis + fontVariant + listStyleType (v11+)
    uint8_t emphasisVal, variantVal, listTypeVal;
    if (file.read(&emphasisVal, 1) != 1 || file.read(&variantVal, 1) != 1 || file.read(&listTypeVal, 1) != 1) {
      rulesBySelector_.clear();
      return false;
    }
    style.textEmphasis = static_cast<CssTextEmphasis>(emphasisVal);
    style.fontVariant = static_cast<CssFontVariant>(variantVal);
    style.listStyleType = static_cast<CssListStyleType>(listTypeVal);

    // Read defined flags
    uint32_t definedBits = 0;
    if (file.read(&definedBits, sizeof(definedBits)) != sizeof(definedBits)) {
      rulesBySelector_.clear();
      return false;
    }
    style.defined.textAlign = (definedBits & 1 << 0) != 0;
    style.defined.fontStyle = (definedBits & 1 << 1) != 0;
    style.defined.fontWeight = (definedBits & 1 << 2) != 0;
    style.defined.textDecoration = (definedBits & 1 << 3) != 0;
    style.defined.textIndent = (definedBits & 1 << 4) != 0;
    style.defined.marginTop = (definedBits & 1 << 5) != 0;
    style.defined.marginBottom = (definedBits & 1 << 6) != 0;
    style.defined.marginLeft = (definedBits & 1 << 7) != 0;
    style.defined.marginRight = (definedBits & 1 << 8) != 0;
    style.defined.paddingTop = (definedBits & 1 << 9) != 0;
    style.defined.paddingBottom = (definedBits & 1 << 10) != 0;
    style.defined.paddingLeft = (definedBits & 1 << 11) != 0;
    style.defined.paddingRight = (definedBits & 1 << 12) != 0;
    style.defined.imageHeight = (definedBits & 1 << 13) != 0;
    style.defined.imageWidth = (definedBits & 1 << 14) != 0;
    style.defined.display = (definedBits & 1 << 15) != 0;
    style.defined.direction = (definedBits & 1 << 16) != 0;
    style.defined.verticalAlign = (definedBits & 1 << 17) != 0;
    style.defined.border = (definedBits & 1 << 18) != 0;
    style.defined.textEmphasis = (definedBits & 1 << 19) != 0;
    style.defined.fontVariant = (definedBits & 1 << 20) != 0;
    style.defined.listStyleType = (definedBits & 1 << 21) != 0;

    // Vertical-scoped rules ("v|...") are consumed exclusively through the streaming
    // collectVerticalStyles() -- loadFromCache feeds the HORIZONTAL layout engine only.
    // Materializing them here grew the resident map by hundreds of EBPAJ rules and pushed the
    // in-session section build over its heap budget (observed: every rebuild aborting with
    // "CSS cache didn't fit in heap").
    if (selector.size() >= 2 && selector[0] == 'v' && selector[1] == '|') continue;

    // Chapter-usage filter: skip class selectors the chapter never references (see header doc).
    if (usedClasses != nullptr) {
      std::string_view sel(selector);
      if (sel.size() >= 2 && sel[0] == 'h' && sel[1] == '|') sel.remove_prefix(2);
      if (const size_t dot = sel.find('.'); dot != std::string_view::npos) {
        const std::string_view cls = sel.substr(dot + 1);
        bool used = false;
        for (const auto& u : *usedClasses) {
          if (u.size() == cls.size() && strncasecmp(u.c_str(), cls.data(), cls.size()) == 0) {
            used = true;
            break;
          }
        }
        if (!used) continue;
      }
    }

    // The incremental (per-file) cache writer can emit the same selector once per CSS file, so
    // replicate the parser's semantics here: later occurrences MERGE onto the earlier entry
    // (applyOver), exactly like a later file's rule block does during a live parse.
    if (auto it = rulesBySelector_.find(selector); it != rulesBySelector_.end()) {
      it->second.applyOver(style);
    } else {
      rulesBySelector_[selector] = style;
    }
  }

  LOG_DBG("CSS", "Loaded %u rules from cache", ruleCount);
  return true;
}
