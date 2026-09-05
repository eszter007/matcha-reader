#include "ParsedText.h"

#include <BidiUtils.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Utf8.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <vector>

#include "TokenBoundary.h"
#include "hyphenation/HyphenationCommon.h"
#include "hyphenation/Hyphenator.h"

constexpr int MAX_COST = std::numeric_limits<int>::max();

namespace {

// Soft hyphen byte pattern used throughout EPUBs (UTF-8 for U+00AD).
constexpr char SOFT_HYPHEN_UTF8[] = "\xC2\xAD";
constexpr size_t SOFT_HYPHEN_BYTES = 2;
// Paragraph-level direction: scan the first N words to find base direction.
constexpr size_t RTL_PARAGRAPH_PROBE_WORDS = 3;
// Per-word: scan enough chars to see through leading neutrals (quotes, numbers)
// before giving up. 64 is a hedge for pathological cases like long numeric tokens.
constexpr int RTL_PER_WORD_PROBE_DEPTH = 64;
constexpr size_t MIN_JUSTIFY_GAPS = 1;

// Byte-level pre-check: Hebrew UTF-8 lead bytes 0xD6-0xD7, Arabic/Syriac 0xD8-0xDB.
bool mayContainRtlBytes(const char* str) {
  for (const auto* p = reinterpret_cast<const unsigned char*>(str); *p; ++p) {
    if (*p >= 0xD6 && *p <= 0xDB) return true;
  }
  return false;
}

// Returns the first rendered codepoint of a word (skipping leading soft hyphens).
uint32_t firstCodepoint(const std::string& word) {
  const auto* ptr = reinterpret_cast<const unsigned char*>(word.c_str());
  while (true) {
    const uint32_t cp = utf8NextCodepoint(&ptr);
    if (cp == 0) return 0;
    if (cp != 0x00AD) return cp;  // skip soft hyphens
  }
}

// Returns the last codepoint of a word by scanning backward for the start of the last UTF-8 sequence.
uint32_t lastCodepoint(const std::string& word) {
  if (word.empty()) return 0;
  // UTF-8 continuation bytes start with 10xxxxxx; scan backward to find the leading byte.
  size_t i = word.size() - 1;
  while (i > 0 && (static_cast<uint8_t>(word[i]) & 0xC0) == 0x80) {
    --i;
  }
  const auto* ptr = reinterpret_cast<const unsigned char*>(word.c_str() + i);
  return utf8NextCodepoint(&ptr);
}

bool containsSoftHyphen(const std::string& word) { return word.find(SOFT_HYPHEN_UTF8) != std::string::npos; }

bool isNoBreakBeforeCjkPunctuation(const uint32_t cp) {
  switch (cp) {
    case '.':
    case ',':
    case ':':
    case ';':
    case '!':
    case '?':
    case ')':
    case ']':
    case '}':
    case 0x00BB:  // »
    case 0x2019:  // ’
    case 0x201D:  // ”
    case 0x3001:  // 、
    case 0x3002:  // 。
    case 0x3009:  // 〉
    case 0x300B:  // 》
    case 0x300D:  // 」
    case 0x300F:  // 』
    case 0x3011:  // 】
    case 0x3015:  // 〕
    case 0x3017:  // 〗
    case 0x3019:  // 〙
    case 0x301B:  // 〛
    case 0xFF01:  // ！
    case 0xFF09:  // ）
    case 0xFF0C:  // ，
    case 0xFF0E:  // ．
    case 0xFF1A:  // ：
    case 0xFF1B:  // ；
    case 0xFF1F:  // ？
    case 0xFF3D:  // ］
    case 0xFF5D:  // ｝
      return true;
    default:
      return false;
  }
}

bool isNoBreakAfterCjkPunctuation(const uint32_t cp) {
  switch (cp) {
    case '(':
    case '[':
    case '{':
    case 0x00AB:  // «
    case 0x2018:  // ‘
    case 0x201C:  // “
    case 0x3008:  // 〈
    case 0x300A:  // 《
    case 0x300C:  // 「
    case 0x300E:  // 『
    case 0x3010:  // 【
    case 0x3014:  // 〔
    case 0x3016:  // 〖
    case 0x3018:  // 〘
    case 0x301A:  // 〚
    case 0xFF08:  // （
    case 0xFF3B:  // ［
    case 0xFF5B:  // ｛
      return true;
    default:
      return false;
  }
}

bool containsCjkBreakableCodepoint(const std::string& text) {
  const auto* ptr = reinterpret_cast<const unsigned char*>(text.c_str());
  while (*ptr) {
    const uint32_t cp = utf8NextCodepoint(&ptr);
    if (utf8IsCjkBreakable(cp)) {
      return true;
    }
  }
  return false;
}

uint32_t countCodepoints(const std::string_view text) {
  const auto* ptr = reinterpret_cast<const unsigned char*>(text.data());
  const auto* const end = ptr + text.size();
  uint32_t count = 0;
  while (ptr < end) {
    utf8NextCodepoint(&ptr);
    count++;
  }
  return count;
}

bool hasCjkBreakOpportunityBetween(const uint32_t leftCp, const uint32_t rightCp) {
  if (!utf8IsCjkBreakable(leftCp) && !utf8IsCjkBreakable(rightCp)) return false;
  if (isNoBreakAfterCjkPunctuation(leftCp) || isNoBreakBeforeCjkPunctuation(rightCp)) return false;
  if (utf8IsCombiningMark(rightCp)) return false;
  return true;
}

std::vector<size_t> cjkCharacterBreakByteOffsets(const std::string& text) {
  struct CodepointBoundary {
    uint32_t cp;
    size_t endOffset;
  };

  std::vector<CodepointBoundary> codepoints;
  codepoints.reserve(text.size());
  bool hasCjkBreakable = false;

  const auto* ptr = reinterpret_cast<const unsigned char*>(text.c_str());
  const auto* const start = ptr;
  while (*ptr) {
    const uint32_t cp = utf8NextCodepoint(&ptr);
    if (cp == 0) break;
    if (utf8IsCjkBreakable(cp)) {
      hasCjkBreakable = true;
    }
    codepoints.push_back({cp, static_cast<size_t>(ptr - start)});
  }

  if (!hasCjkBreakable || codepoints.size() < 2) return {};

  std::vector<size_t> allowedOffsets;
  allowedOffsets.reserve(codepoints.size() - 1);
  for (size_t i = 0; i + 1 < codepoints.size(); ++i) {
    const uint32_t current = codepoints[i].cp;
    const uint32_t next = codepoints[i + 1].cp;
    if (!hasCjkBreakOpportunityBetween(current, next)) continue;
    allowedOffsets.push_back(codepoints[i].endOffset);
  }
  return allowedOffsets;
}

int computeJustifyExtra(const int spareSpace, const size_t gapCount) {
  if (gapCount < MIN_JUSTIFY_GAPS || spareSpace <= 0) return 0;
  // Distribute the spare space evenly across gaps. Do NOT bail out to 0 when the
  // per-gap stretch is large: a sparse line (few words on a wide page) legitimately
  // needs big gaps to reach the margin. Returning 0 there disables justification for
  // that line, leaving it right-aligned (RTL) / left-aligned (LTR) — the mismatched
  // alignment bug. Match the un-capped behavior of the old code.
  return spareSpace / static_cast<int>(gapCount);
}

// Removes every soft hyphen in-place so rendered glyphs match measured widths.
void stripSoftHyphensInPlace(std::string& word) {
  size_t pos = 0;
  while ((pos = word.find(SOFT_HYPHEN_UTF8, pos)) != std::string::npos) {
    word.erase(pos, SOFT_HYPHEN_BYTES);
  }
}

// Returns the advance width for a word while ignoring soft hyphen glyphs and optionally appending a visible hyphen.
// Uses advance width (sum of glyph advances + kerning) rather than bounding box width so that italic glyph overhangs
// don't inflate inter-word spacing.
// A measured width of zero means the advance data isn't resident (advance-table cap, cold
// font after a change). Never lay a word out at zero width -- that bakes overlapping words
// into the cached layout (device photo: TCY dates overprinting their neighbours). Estimate
// em-uniform CJK at 7/8 em per codepoint instead; keep the estimate OUT of getTextAdvanceX
// itself, whose zero is a meaningful "not resident" answer to probing callers (the vertical
// cell measurement handles it with its own deliberate fallback).
uint16_t fallbackWordWidth(const GfxRenderer& renderer, const int fontId, const std::string& word) {
  const int emPx = renderer.getLineHeight(fontId);
  size_t cpCount = 0;
  for (const char c : word) {
    if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) cpCount++;
  }
  return static_cast<uint16_t>(std::max<size_t>(1, cpCount) * ((emPx * 7) / 8));
}

uint16_t measureWordWidth(const GfxRenderer& renderer, const int fontId, const std::string& word,
                          const EpdFontFamily::Style style, const int8_t letterSpacing,
                          const bool appendHyphen = false) {
  if (word.size() == 1 && word[0] == ' ' && !appendHyphen) {
    return renderer.getSpaceWidth(fontId, style, letterSpacing);
  }
  const bool hasSoftHyphen = containsSoftHyphen(word);
  if (!hasSoftHyphen && !appendHyphen) {
    const int w = renderer.getTextAdvanceX(fontId, word.c_str(), style, letterSpacing);
    return w > 0 ? static_cast<uint16_t>(w) : fallbackWordWidth(renderer, fontId, word);
  }

  std::string sanitized = word;
  if (hasSoftHyphen) {
    stripSoftHyphensInPlace(sanitized);
  }
  if (appendHyphen) {
    sanitized.push_back('-');
  }
  const int w = renderer.getTextAdvanceX(fontId, sanitized.c_str(), style, letterSpacing);
  return w > 0 ? static_cast<uint16_t>(w) : fallbackWordWidth(renderer, fontId, sanitized);
}

bool endsWithBreakableHyphen(const std::string& token) {
  return !token.empty() && TokenBoundary::allowsBreakAfterExplicitHyphen(lastCodepoint(token));
}

// wordFocusBoundary is uint8_t, so 256 bytes cover every representable prefix plus the NUL.
constexpr size_t FOCUS_PREFIX_BUF_SIZE = 256;

uint16_t measureFocusPrefixAdvance(const GfxRenderer& renderer, const int fontId, const std::string& word,
                                   const EpdFontFamily::Style style, const uint8_t focusBoundary,
                                   const int8_t letterSpacing) {
  char prefixBuf[FOCUS_PREFIX_BUF_SIZE];
  const size_t prefixLen = std::min<size_t>(focusBoundary, FOCUS_PREFIX_BUF_SIZE - 1);
  memcpy(prefixBuf, word.data(), prefixLen);
  prefixBuf[prefixLen] = '\0';

  const auto boldStyle = static_cast<EpdFontFamily::Style>(style | EpdFontFamily::BOLD);
  const auto* suffixPtr = reinterpret_cast<const unsigned char*>(word.c_str() + focusBoundary);
  const int kerning = renderer.getKerning(fontId, lastCodepoint(prefixBuf), utf8NextCodepoint(&suffixPtr), boldStyle);
  return static_cast<uint16_t>(renderer.getTextAdvanceX(fontId, prefixBuf, boldStyle, letterSpacing) + kerning);
}

uint16_t measureFocusWordWidth(const GfxRenderer& renderer, const int fontId, const std::string& word,
                               const EpdFontFamily::Style style, const uint8_t focusBoundary,
                               const int8_t letterSpacing, const bool appendHyphen = false) {
  if (focusBoundary == 0) return measureWordWidth(renderer, fontId, word, style, letterSpacing, appendHyphen);
  if (focusBoundary >= word.size()) {
    return measureWordWidth(renderer, fontId, word, static_cast<EpdFontFamily::Style>(style | EpdFontFamily::BOLD),
                            letterSpacing, appendHyphen);
  }
  const uint16_t suffixWidth =
      appendHyphen
          ? measureWordWidth(renderer, fontId, word.substr(focusBoundary), style, letterSpacing, true)
          : static_cast<uint16_t>(renderer.getTextAdvanceX(fontId, word.c_str() + focusBoundary, style, letterSpacing));
  return measureFocusPrefixAdvance(renderer, fontId, word, style, focusBoundary, letterSpacing) + suffixWidth;
}

uint8_t focusBoundaryBefore(const uint8_t focusBoundary, const size_t splitOffset) {
  return static_cast<uint8_t>(std::min<size_t>(focusBoundary, splitOffset));
}

uint8_t focusBoundaryAfter(const uint8_t focusBoundary, const size_t splitOffset) {
  return focusBoundary > splitOffset ? static_cast<uint8_t>(focusBoundary - splitOffset) : 0;
}

// Checks if a UTF-8 codepoint should be counted as part of a word for Focus Reading
bool isWordCharacter(uint32_t cp) {
  // ASCII range (Catches 95%+ of characters immediately)
  if (cp < 128) {
    // Bitwise trick: (cp | 0x20) converts uppercase ASCII to lowercase.
    // This checks for A-Z and a-z mathematically, avoiding memory lookups and <cctype>
    return ((cp | 0x20) >= 'a' && (cp | 0x20) <= 'z') || cp == '\'';
  }

  // General Punctuation Block, Currency, Math, Arrows, & Symbols (0x2000 - 0x2BFF)
  if (cp >= 0x2000 && cp <= 0x2BFF) {
    // Explicitly allow smart quotes, reject all other general punctuation (em-dashes, etc.)
    return cp == 0x2018 || cp == 0x2019;
  }

  // Latin-1 Punctuation Block (0x00A1 - 0x00BF)
  if (cp >= 0x00A1 && cp <= 0x00BF) {
    // Allow ordinal indicators and micro sign, reject the rest (¡, ¿, «, », etc.)
    return cp == 0x00AA || cp == 0x00B5 || cp == 0x00BA;
  }

  // Rejects Two-em dash, Three-em dash, Double oblique hyphen, etc.
  if (cp >= 0x2E00 && cp <= 0x2E7F) return false;

  // Rejects Modifier Minus (0x02D7), Small Hyphen (0xFE63), and Fullwidth Hyphen (0xFF0D)
  if (cp == 0x02D7 || cp == 0xFE63 || cp == 0xFF0D) return false;
  // Assume all other Unicode ranges (accented letters, Cyrillic, Greek, etc.) are valid

  return true;
}

}  // namespace

uint32_t ParsedText::visibleOffsetBaseAt(const size_t wordIndex) const {
  uint32_t base = visibleOffsetBase;
  for (const auto& rebase : visibleOffsetRebases) {
    if (rebase.wordIndex > wordIndex) break;
    base = rebase.base;
  }
  return base;
}

uint32_t ParsedText::visibleOffsetAt(const size_t wordIndex) const {
  if (wordIndex >= wordVisibleOffsetDeltas.size()) return 0;
  return visibleOffsetBaseAt(wordIndex) + wordVisibleOffsetDeltas[wordIndex];
}

void ParsedText::pushVisibleOffset(const uint32_t offset) {
  uint32_t base = visibleOffsetBase;
  if (wordVisibleOffsetDeltas.empty()) {
    visibleOffsetBase = offset;
    base = offset;
  } else if (!visibleOffsetRebases.empty()) {
    base = visibleOffsetRebases.back().base;
  }

  if (offset < base || offset - base > std::numeric_limits<uint16_t>::max()) {
    visibleOffsetRebases.push_back({wordVisibleOffsetDeltas.size(), offset});
    base = offset;
  }
  wordVisibleOffsetDeltas.push_back(static_cast<uint16_t>(offset - base));
}

void ParsedText::insertVisibleOffset(const size_t wordIndex, const uint32_t offset) {
  const uint32_t base = wordIndex > 0 ? visibleOffsetBaseAt(wordIndex - 1) : visibleOffsetBase;
  for (auto& rebase : visibleOffsetRebases) {
    if (rebase.wordIndex >= wordIndex) rebase.wordIndex++;
  }

  uint32_t insertionBase = base;
  if (offset < base || offset - base > std::numeric_limits<uint16_t>::max()) {
    const auto rebaseIt = std::find_if(visibleOffsetRebases.begin(), visibleOffsetRebases.end(),
                                       [wordIndex](const auto& rebase) { return rebase.wordIndex > wordIndex; });
    visibleOffsetRebases.insert(rebaseIt, {wordIndex, offset});
    insertionBase = offset;
  }
  wordVisibleOffsetDeltas.insert(wordVisibleOffsetDeltas.begin() + wordIndex,
                                 static_cast<uint16_t>(offset - insertionBase));
}

void ParsedText::eraseVisibleOffsetPrefix(const size_t count) {
  if (count >= wordVisibleOffsetDeltas.size()) {
    wordVisibleOffsetDeltas.clear();
    visibleOffsetRebases.clear();
    visibleOffsetBase = 0;
    return;
  }

  const uint32_t newBase = visibleOffsetBaseAt(count);
  wordVisibleOffsetDeltas.erase(wordVisibleOffsetDeltas.begin(), wordVisibleOffsetDeltas.begin() + count);
  size_t writeIndex = 0;
  for (auto rebase : visibleOffsetRebases) {
    if (rebase.wordIndex <= count) continue;
    rebase.wordIndex -= count;
    visibleOffsetRebases[writeIndex++] = rebase;
  }
  visibleOffsetRebases.resize(writeIndex);
  visibleOffsetBase = newBase;
}

void ParsedText::addWord(std::string word, const EpdFontFamily::Style fontStyle, const bool underline,
                         const bool attachToPrevious, const int32_t wordFontId, const uint32_t visibleTextOffset,
                         const uint8_t linkId) {
  if (word.empty()) return;

  // Keeps wordFonts index-aligned with words[]. Called AFTER a token is pushed to words[]:
  // the first sized token materializes the vector, backfilling every earlier word with 0
  // ("block font"). Stays empty for the common no-sized-span block -- zero cost there.
  const auto pushTokenFont = [&] {
    if (wordFontId == 0 && wordFonts.empty()) return;
    if (wordFonts.size() < words.size() - 1) {
      wordFonts.resize(words.size() - 1, 0);
    }
    wordFonts.push_back(wordFontId);
  };

  // The device fonts carry no combining-mark positioning, so EPUB text stored in NFD
  // (a base letter followed by separate combining accents -- common for Vietnamese,
  // and used for many EPUB <h1> chapter headings) renders with the marks detached or
  // misplaced. Compose to NFC here, the single funnel every word passes through, so a
  // precomposed glyph is used instead. This runs once per word at layout time (the
  // result is cached in the section file) and is a cheap no-op for mark-free text.
  word = utf8ComposeNfc(word);

  EpdFontFamily::Style baseStyle = fontStyle;
  if (underline) {
    baseStyle = static_cast<EpdFontFamily::Style>(baseStyle | EpdFontFamily::UNDERLINE);
  }
  const bool wordStartsRtl = !hasRtlWord && mayContainRtlBytes(word.c_str()) &&
                             BidiUtils::startsWithRtl(word.c_str(), RTL_PER_WORD_PROBE_DEPTH);

  const auto pushToken = [&](std::string token, const bool continues, const bool noSpaceBefore,
                             const uint8_t focusBoundary, const uint32_t tokenOffset) {
    words.push_back(std::move(token));
    wordStyles.push_back(baseStyle);
    wordContinues.push_back(continues);
    wordNoSpaceBefore.push_back(noSpaceBefore);
    wordFocusBoundary.push_back(focusBoundary);
    wordLinkIds.push_back(linkId);
    pushVisibleOffset(tokenOffset);
    if (!rubyTexts.empty()) {
      rubyTexts.push_back("");
    }
    pushTokenFont();
  };

  bool effectiveAttachToPrevious = attachToPrevious;
  bool effectiveNoSpaceBefore = false;
  // Only a glued token (attachToPrevious == true, i.e. no whitespace separated it from the
  // previous one in the source) may be turned into a gap-less break opportunity. When real
  // whitespace separated the two words, that space is content and must be rendered: Korean
  // is a space-delimited script written in Hangul, which utf8IsCjkBreakable() covers.
  if (attachToPrevious && !words.empty() &&
      hasCjkBreakOpportunityBetween(lastCodepoint(words.back()), firstCodepoint(word))) {
    effectiveAttachToPrevious = false;
    effectiveNoSpaceBefore = true;
  }

  // Bulk-reserve the per-token parallel arrays before a burst of pushes so they
  // don't repeatedly double. Only the std::vector arrays are reserved: words and
  // rubyTexts are std::deque (chunked growth, no reserve()/capacity() and no large
  // contiguous reallocation to avoid). wordStyles' capacity gauges them all since
  // pushToken() keeps every array in lockstep.
  const auto ensureTokenCapacity = [&](const size_t additionalTokens) {
    if (additionalTokens == 0) return;
    const size_t requiredSize = words.size() + additionalTokens;
    if (wordStyles.capacity() >= requiredSize) return;

    size_t newCapacity = wordStyles.capacity() < 16 ? 16 : wordStyles.capacity();
    while (newCapacity < requiredSize) {
      newCapacity *= 2;
    }

    wordStyles.reserve(newCapacity);
    wordContinues.reserve(newCapacity);
    wordNoSpaceBefore.reserve(newCapacity);
    wordFocusBoundary.reserve(newCapacity);
    if (!wordFonts.empty()) {
      wordFonts.reserve(newCapacity);
    }
    wordLinkIds.reserve(newCapacity);
    wordVisibleOffsetDeltas.reserve(newCapacity);
  };

  if (auto breakOffsets = cjkCharacterBreakByteOffsets(word); !breakOffsets.empty()) {
    // CJK-heavy paragraphs can push hundreds of tiny tokens quickly when CSS toggles
    // inline styles. Reserve once up front to avoid repeated vector growth reallocations.
    ensureTokenCapacity(breakOffsets.size() + 1);
    bool firstToken = true;
    size_t tokenStart = 0;
    uint32_t tokenVisibleOffset = visibleTextOffset;
    for (const size_t breakOffset : breakOffsets) {
      if (breakOffset <= tokenStart || breakOffset > word.size()) continue;
      const std::string_view token(word.data() + tokenStart, breakOffset - tokenStart);
      pushToken(std::string(token), firstToken ? effectiveAttachToPrevious : false,
                firstToken ? effectiveNoSpaceBefore : true, 0, tokenVisibleOffset);
      tokenVisibleOffset += countCodepoints(token);
      firstToken = false;
      tokenStart = breakOffset;
    }
    if (tokenStart < word.size()) {
      pushToken(word.substr(tokenStart), firstToken ? effectiveAttachToPrevious : false,
                firstToken ? effectiveNoSpaceBefore : true, 0, tokenVisibleOffset);
    }
    if (wordStartsRtl) {
      hasRtlWord = true;
    }
    return;
  }

  if (containsCjkBreakableCodepoint(word)) {
    pushToken(std::move(word), effectiveAttachToPrevious, effectiveNoSpaceBefore, 0, visibleTextOffset);
    if (wordStartsRtl) {
      hasRtlWord = true;
    }
    return;
  }

  // Already-bold text should stay fully bold; focus splitting would make its suffix regular later.
  if (!this->focusReadingEnabled || (baseStyle & EpdFontFamily::BOLD) != 0) {
    pushToken(std::move(word), effectiveAttachToPrevious, effectiveNoSpaceBefore, 0, visibleTextOffset);
    if (wordStartsRtl) {
      hasRtlWord = true;
    }
    return;
  }

  // --- FOCUS READING LOGIC BELOW ---

  // Worst case: a segment boundary on each byte (highly punctuated UTF-8 text).
  // words/rubyTexts are deques (no reserve); ensureTokenCapacity covers the vector arrays.
  ensureTokenCapacity(word.length());

  // Lambda helper to process and push individual sub-segments of the string
  // Use std::string_view to avoid heap allocations when slicing
  // segmentOffset: visible-codepoint offset of the segment's first character, tracked
  // incrementally by the tokenization loop below (a per-segment prefix rescan would be
  // O(n^2) on highly punctuated words -- Copilot review, PR #24).
  auto processSegment = [&](std::string_view segment, bool isWord, bool attach, bool noSpaceBefore,
                            const uint32_t segmentOffset) {
    const auto pushSegmentFont = pushTokenFont;
    if (!isWord) {
      // Punctuation and Numbers stay regular
      words.emplace_back(segment);
      wordStyles.push_back(baseStyle);
      wordContinues.push_back(attach);
      wordNoSpaceBefore.push_back(noSpaceBefore);
      wordFocusBoundary.push_back(0);
      pushSegmentFont();
      wordLinkIds.push_back(linkId);
      pushVisibleOffset(segmentOffset);
    } else {
      size_t charCount = 0;
      const unsigned char* countPtr = reinterpret_cast<const unsigned char*>(segment.data());
      const unsigned char* countEnd = countPtr + segment.length();

      while (countPtr < countEnd) {
        utf8NextCodepoint(&countPtr);
        charCount++;
      }

      // Target 45% for 1-bold at 4 chars and 3-bold at 7 chars with floor truncation
      constexpr size_t FOCUS_READING_PERCENT = 45;
      size_t targetBoldChars = (charCount * FOCUS_READING_PERCENT) / 100;
      targetBoldChars = std::clamp<size_t>(targetBoldChars, 1, 9);

      if (targetBoldChars >= charCount) {
        // Whole segment is bold - no suffix split needed
        words.emplace_back(segment);
        wordStyles.push_back(static_cast<EpdFontFamily::Style>(baseStyle | EpdFontFamily::BOLD));
        wordContinues.push_back(attach);
        wordNoSpaceBefore.push_back(noSpaceBefore);
        wordFocusBoundary.push_back(0);
        pushSegmentFont();
        wordLinkIds.push_back(linkId);
        pushVisibleOffset(segmentOffset);
      } else {
        countPtr = reinterpret_cast<const unsigned char*>(segment.data());
        for (size_t i = 0; i < targetBoldChars; ++i) {
          utf8NextCodepoint(&countPtr);
        }
        size_t splitByteOffset = countPtr - reinterpret_cast<const unsigned char*>(segment.data());

        // Keep the original word as one token so hyphenation sees every legal break.
        words.emplace_back(segment);
        wordStyles.push_back(baseStyle);
        wordContinues.push_back(attach);
        wordNoSpaceBefore.push_back(noSpaceBefore);
        wordFocusBoundary.push_back(static_cast<uint8_t>(std::min<size_t>(splitByteOffset, 255)));
        pushSegmentFont();
        wordLinkIds.push_back(linkId);
        pushVisibleOffset(segmentOffset);
      }
    }
  };

  // Tokenize the string by alternating states (Word vs. Non-Word)
  const unsigned char* ptr = reinterpret_cast<const unsigned char*>(word.c_str());
  const unsigned char* end = ptr + word.length();

  const unsigned char* segmentStart = ptr;
  uint32_t firstCp = utf8NextCodepoint(&ptr);  // Consume the first char to determine initial state
  bool inWordSegment = isWordCharacter(firstCp);

  bool isFirstSegment = true;
  uint32_t cpIndex = 1;              // codepoints consumed from the word so far
  uint32_t segmentStartCpIndex = 0;  // codepoint index where the current segment starts

  while (ptr < end) {
    const unsigned char* currentCpStart = ptr;
    uint32_t cp = utf8NextCodepoint(&ptr);
    bool isWordChar = isWordCharacter(cp);

    // Whenever the character type flips, slice off the segment we just completed and process it
    if (isWordChar != inWordSegment) {
      size_t segmentLen = currentCpStart - segmentStart;
      std::string_view segment(reinterpret_cast<const char*>(segmentStart), segmentLen);

      // Subsequent segments stay attached unless the previous token ends in a visible
      // breakable hyphen; true,true records a gap-less break opportunity there.
      const bool breakAfterPrev = !isFirstSegment && !words.empty() && endsWithBreakableHyphen(words.back());
      processSegment(segment, inWordSegment, isFirstSegment ? effectiveAttachToPrevious : true,
                     isFirstSegment ? effectiveNoSpaceBefore : breakAfterPrev, visibleTextOffset + segmentStartCpIndex);

      // Setup for the next segment
      segmentStart = currentCpStart;
      segmentStartCpIndex = cpIndex;
      inWordSegment = isWordChar;
      isFirstSegment = false;
    }
    cpIndex++;
  }

  // Process the final remaining segment
  size_t segmentLen = end - segmentStart;
  std::string_view segment(reinterpret_cast<const char*>(segmentStart), segmentLen);
  const bool breakAfterPrev = !isFirstSegment && !words.empty() && endsWithBreakableHyphen(words.back());
  processSegment(segment, inWordSegment, isFirstSegment ? effectiveAttachToPrevious : true,
                 isFirstSegment ? effectiveNoSpaceBefore : breakAfterPrev, visibleTextOffset + segmentStartCpIndex);
  if (wordStartsRtl) {
    hasRtlWord = true;
  }
}

uint8_t ParsedText::addLinkTarget(const char* href) {
  if (!href || href[0] == '\0' || strnlen(href, FOOTNOTE_HREF_LEN) >= FOOTNOTE_HREF_LEN ||
      linkTargets.size() >= UINT8_MAX) {
    return 0;
  }
  linkTargets.emplace_back(href);
  return static_cast<uint8_t>(linkTargets.size());
}

bool ParsedText::linkTargetMatches(const uint8_t linkId, const char* href) const {
  return linkId > 0 && linkId <= linkTargets.size() && href && linkTargets[linkId - 1] == href;
}

void ParsedText::setRubyForWordAt(size_t index, const std::string& ruby) {
  if (index >= words.size()) return;
  if (rubyTexts.size() <= index) {
    rubyTexts.resize(words.size());
  }
  rubyTexts[index] = ruby;
}

void ParsedText::setRubyGroupAt(size_t startIndex, size_t count, const std::string& ruby) {
  if (startIndex >= words.size()) return;
  if (rubyTexts.size() <= startIndex) {
    rubyTexts.resize(words.size());
  }
  rubyTexts[startIndex] = ruby;
  for (size_t i = 1; i < count; i++) {
    size_t idx = startIndex + i;
    if (idx >= words.size()) break;
    if (rubyTexts.size() <= idx) {
      rubyTexts.resize(words.size());
    }
    rubyTexts[idx] = "";
    wordStyles[idx] =
        static_cast<EpdFontFamily::Style>(static_cast<uint8_t>(wordStyles[idx]) | EpdFontFamily::RUBY_CONTINUE);
    wordContinues[idx] = true;       // Prevent page breaker from splitting the Group Ruby!
    wordNoSpaceBefore[idx] = false;  // Ensure allowsBreak returns false!
  }
}

void ParsedText::ensureRubyCapacity() {
  // No-op: rubyTexts is a std::deque (chunked growth, no capacity to pre-reserve
  // and no large contiguous reallocation to avoid). Kept for call-site stability.
}

int ParsedText::resolveFirstLineIndent(const bool isFirstLine, const GfxRenderer& renderer, const int fontId) const {
  if (!isFirstLine || !isNaturalAlign) {
    return 0;
  }
  if (blockStyle.textIndentDefined) {
    if (blockStyle.textIndent < 0 || !extraParagraphSpacing) {
      return blockStyle.textIndent;
    }
    return 0;
  }
  if (!extraParagraphSpacing) {
    return renderer.getSpaceWidth(fontId, EpdFontFamily::REGULAR) * 3;
  }
  return 0;
}

int ParsedText::resolveLineIndent(const size_t lineIndex, const GfxRenderer& renderer, const int fontId) const {
  if (blockStyle.hasDropCap()) {
    // The enlarged letter takes the place of the first-line indent -- applying both would push
    // the opening words a further three spaces off a column that is already inset.
    return lineIndex < blockStyle.dropCapLines ? blockStyle.dropCapIndent : 0;
  }
  return resolveFirstLineIndent(lineIndex == 0 && !dropCapLinesEmitted, renderer, fontId);
}

namespace {

// A drop cap is only meaningful on a letter. Digits and punctuation are excluded because a
// stylesheet applies `::first-letter` to whole classes of paragraph, and one that happens to
// open with a quote mark or a numeral would otherwise blow that character up to four lines
// tall. Non-ASCII is assumed to be a letter (accented capitals, Cyrillic, Greek).
bool isDropCapLetter(const uint32_t cp) {
  if (cp >= 0x80) return true;
  return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z');
}

}  // namespace

bool ParsedText::prepareDropCap(const GfxRenderer& renderer, const int fontId, const int pageWidth) {
  if (words.empty() || words[0].empty()) return false;

  const auto* ptr = reinterpret_cast<const unsigned char*>(words[0].c_str());
  const auto* const start = ptr;
  const uint32_t cp = utf8NextCodepoint(&ptr);
  if (cp == 0 || !isDropCapLetter(cp)) return false;
  const size_t letterBytes = static_cast<size_t>(ptr - start);

  // The face of the word the letter comes from -- an italic chapter opening keeps an italic
  // initial. Read before the peel below, which can drop the token entirely.
  const auto style = static_cast<EpdFontFamily::Style>(
      wordStyles.empty() ? 0 : static_cast<uint8_t>(wordStyles[0]) & TextBlock::DROP_CAP_STYLE_MASK);

  int glyphLeft = 0;
  int glyphWidth = 0;
  int glyphTop = 0;
  int glyphHeight = 0;
  if (!renderer.getGlyphMetrics(fontId, cp, style, &glyphLeft, &glyphWidth, &glyphTop, &glyphHeight)) {
    return false;
  }
  if (glyphWidth <= 0 || glyphHeight <= 0) return false;

  // Magnify by whole pixels only (see GfxRenderer::drawCharUpscaled). Floor rather than round,
  // so the letter never grows past the lines it is meant to sit beside; the small shortfall
  // reads as the optical gap a drop cap normally keeps above the baseline it lands on.
  const int lineHeight = renderer.getLineHeight(fontId);
  const int targetHeight = lineHeight * blockStyle.dropCapLines;
  int scale = targetHeight / glyphHeight;
  if (scale > TextBlock::MAX_DROP_CAP_SCALE) scale = TextBlock::MAX_DROP_CAP_SCALE;
  // Below 2x this is not a wrap-around drop cap, just a slightly bigger letter, and the
  // narrowed lines would cost more than the effect is worth. Leave the letter in the text.
  if (scale < 2) return false;

  const int gap = renderer.getSpaceWidth(fontId, EpdFontFamily::REGULAR);
  const int indent = glyphWidth * scale + gap;
  // A column this wide leaves too little for the text beside it to break sensibly; the greedy
  // fill would put one word per line and any long word would overhang the margin.
  if (indent > pageWidth / 3) return false;

  dropCap.cp = cp;
  dropCap.scale = static_cast<uint8_t>(scale);
  dropCap.style = static_cast<uint8_t>(style);
  // Ink box origin relative to the block's, on the side the reserved column sits.
  dropCap.inkLeft = blockStyle.isRtl ? static_cast<int16_t>(pageWidth - glyphWidth * scale) : 0;
  // Align the enlarged letter's ink top with where the first line's own capitals start, so the
  // two share a top edge instead of the drop cap floating above or sinking into the line.
  dropCap.inkTop = static_cast<int16_t>(renderer.getFontAscenderSize(fontId) - glyphTop);
  blockStyle.dropCapIndent = static_cast<int16_t>(indent);

  // The letter now belongs to the drop cap, not to the text flow -- CSS ::first-letter styles
  // it in place, and drawing it here as well would double it. Its visible-codepoint offset
  // stays with the word so selection and progress positions do not shift.
  words[0].erase(0, letterBytes);
  if (words[0].empty()) {
    // The whole first token was the letter (a one-character opening word). Drop the now-empty
    // token, keeping every parallel array in lockstep exactly as layoutAndExtractLines does.
    words.erase(words.begin());
    wordStyles.erase(wordStyles.begin());
    wordContinues.erase(wordContinues.begin());
    wordNoSpaceBefore.erase(wordNoSpaceBefore.begin());
    wordFocusBoundary.erase(wordFocusBoundary.begin());
    if (!wordFonts.empty()) wordFonts.erase(wordFonts.begin());
    wordLinkIds.erase(wordLinkIds.begin());
    eraseVisibleOffsetPrefix(1);
    if (!rubyTexts.empty()) rubyTexts.erase(rubyTexts.begin());
  } else if (!wordFocusBoundary.empty() && wordFocusBoundary[0] != 0) {
    // The focus-reading bold prefix is a BYTE count into the word that just got shorter.
    wordFocusBoundary[0] =
        static_cast<uint8_t>(wordFocusBoundary[0] > letterBytes ? wordFocusBoundary[0] - letterBytes : 0);
  }
  return true;
}

std::vector<size_t> ParsedText::computeDropCapLineBreaks(const GfxRenderer& renderer, const int fontId,
                                                         const int pageWidth, const std::vector<uint16_t>& wordWidths,
                                                         const std::vector<bool>& continuesVec,
                                                         const std::vector<bool>& noSpaceBeforeVec) const {
  // Greedy, not the optimal DP in computeLineBreaks: dp[i] is keyed by the word that STARTS a
  // line, and of the lines beside a drop cap only line 0's start is known before the solution
  // exists -- lines 1..N-1 begin wherever line 0 happened to break. Threading a line count
  // through that DP would change the cost function every paragraph in every book goes through,
  // to gain optimality on at most four lines of one paragraph per chapter whose width is FIXED
  // (unlike the paragraph tail the DP exists to balance). The hyphenated path is greedy for the
  // whole paragraph already, so this is the behaviour books ship with either way.
  std::vector<size_t> breaks;
  breaks.reserve(blockStyle.dropCapLines);

  const size_t total = words.size();
  size_t start = 0;
  for (uint8_t line = 0; line < blockStyle.dropCapLines && start < total; ++line) {
    const int avail = pageWidth - resolveLineIndent(line, renderer, fontId);
    int used = 0;
    size_t best = start;  // words committed at the last break the boundary flags allow

    for (size_t j = start; j < total; ++j) {
      int gap = 0;
      if (j > start) {
        if (continuesVec[j]) {
          gap = renderer.getKerning(fontId, lastCodepoint(words[j - 1]), firstCodepoint(words[j]), wordStyles[j - 1]);
        } else if (!noSpaceBeforeVec[j]) {
          gap = renderer.getSpaceAdvance(fontId, lastCodepoint(words[j - 1]), firstCodepoint(words[j]),
                                         wordStyles[j - 1], blockStyle.letterSpacing);
        }
      }
      const int candidate = used + gap + wordWidths[j];
      const bool overflows = candidate > avail;
      if (overflows && best > start) break;  // this word does not fit; keep the last legal break
      used = candidate;
      if (j + 1 >= total || TokenBoundary::allowsBreak(continuesVec[j + 1], noSpaceBeforeVec[j + 1])) {
        best = j + 1;
        if (overflows) break;  // an unbreakable run had to overflow to reach a legal break
      }
    }

    // No legal break anywhere ahead: give the line one word, the same concession the DP makes
    // for a word wider than the column (dp[i] == MAX_COST).
    if (best <= start) best = start + 1;
    breaks.push_back(best);
    start = best;
  }
  return breaks;
}
// Consumes data to minimize memory usage
void ParsedText::layoutAndExtractLines(const GfxRenderer& renderer, const int baseFontId, const uint16_t viewportWidth,
                                       const std::function<void(std::shared_ptr<TextBlock>, uint32_t)>& processLine,
                                       const bool includeLastLine) {
  if (words.empty()) {
    return;
  }

  // Every measurement below is made with the BLOCK's font (CSS font-size), and the emitted
  // TextBlocks carry the same blockStyle, so TextBlock::render() resolves the identical id.
  // Measuring here with the reader's font while drawing with a larger one is exactly the
  // layout/draw disagreement that mis-positions a whole line.
  const int fontId = blockStyle.resolveFontId(baseFontId);

  // Per-paragraph RTL auto-detection: only when CSS/HTML didn't explicitly set direction.
  // Explicit dir="ltr" must be respected and not overridden by content heuristic.
  if (!blockStyle.directionDefined && hasRtlWord) {
    // Check the first few words for RTL letter codepoints (no heap allocation).
    const size_t wordsToScan = std::min(words.size(), RTL_PARAGRAPH_PROBE_WORDS);
    for (size_t i = 0; i < wordsToScan; ++i) {
      if (BidiUtils::startsWithRtl(words[i].c_str(), BidiUtils::RTL_PARAGRAPH_PROBE_DEPTH)) {
        blockStyle.isRtl = true;
        break;
      }
    }
  }

  isNaturalAlign =
      blockStyle.alignment == CssTextAlign::Justify ||
      (blockStyle.isRtl ? blockStyle.alignment == CssTextAlign::Right : blockStyle.alignment == CssTextAlign::Left);

  // Ensure SD card font glyph metrics are loaded before measuring word widths.
  // For flash-based fonts isSdCardFont() returns false and this block is skipped
  // entirely — no heap allocation. For SD card fonts this reads glyph metadata
  // (advanceX only, no bitmaps) for all unique codepoints in this paragraph so
  // that calculateWordWidths() can measure text without on-demand SD I/O.
  const bool hyphenationActive = hyphenationEnabled && !blockStyle.suppressHyphens;
  if (renderer.isSdCardFont(fontId)) {
    // Style mask: only ask the SD font to load advances for styles actually
    // used in this paragraph. Style index is the low two bits (regular/bold/
    // italic/bold-italic); the underline bit is irrelevant to advance metrics.
    uint8_t styleMask = 0;
    for (size_t i = 0; i < wordStyles.size(); ++i) {
      const auto s = wordStyles[i];
      styleMask |= static_cast<uint8_t>(1u << (static_cast<uint8_t>(s) & 0x03));
      if (i < wordFocusBoundary.size() && wordFocusBoundary[i] != 0) {
        const auto bold = static_cast<uint8_t>(s | EpdFontFamily::BOLD);
        styleMask |= static_cast<uint8_t>(1u << (bold & 0x03));
      }
    }
    if (styleMask == 0) styleMask = 0x01;  // defensive: regular only
    renderer.ensureSdCardFontReady(fontId, words, hyphenationActive, styleMask);
  }

  const int pageWidth = viewportWidth;

  // Drop cap: claim the first letter before the widths are measured, so the shortened opening
  // word is measured as it will be drawn.
  if (blockStyle.dropCapLines > 0 && !dropCapResolved) {
    dropCapResolved = true;
    if (!prepareDropCap(renderer, fontId, pageWidth)) {
      blockStyle.dropCapLines = 0;  // letter stays in the text and renders inline
    } else if (words.empty()) {
      return;  // the paragraph was the single letter
    }
  }

  auto wordWidths = calculateWordWidths(renderer, fontId);

  // The lines beside the enlarged letter, laid out against the reserved column, then removed
  // from the pass so the rest of the paragraph flows at full width through the usual DP.
  if (blockStyle.hasDropCap()) {
    const std::vector<size_t> dropCapBreaks =
        computeDropCapLineBreaks(renderer, fontId, pageWidth, wordWidths, wordContinues, wordNoSpaceBefore);
    for (size_t i = 0; i < dropCapBreaks.size(); ++i) {
      extractLine(i, pageWidth, wordWidths, wordContinues, wordNoSpaceBefore, dropCapBreaks, processLine, renderer,
                  fontId);
    }
    if (!dropCapBreaks.empty()) {
      consumeWords(dropCapBreaks.back());
      wordWidths.erase(wordWidths.begin(), wordWidths.begin() + std::min(dropCapBreaks.back(), wordWidths.size()));
    }
    // Cleared together: the column is behind us, so the remaining lines take the full width,
    // and the paragraph tail's own first line -- which reaches extractLine with breakIndex 0
    // like any other -- cannot pick the enlarged letter up and draw it a second time.
    blockStyle.dropCapLines = 0;
    blockStyle.dropCapIndent = 0;
    dropCap = TextBlock::DropCap{};
    dropCapLinesEmitted = true;
    if (words.empty()) return;
  }

  std::vector<size_t> lineBreakIndices;
  if (hyphenationActive) {
    // Use greedy layout that can split words mid-loop when a hyphenated prefix fits.
    lineBreakIndices =
        computeHyphenatedLineBreaks(renderer, fontId, pageWidth, wordWidths, wordContinues, wordNoSpaceBefore);
  } else {
    lineBreakIndices = computeLineBreaks(renderer, fontId, pageWidth, wordWidths, wordContinues, wordNoSpaceBefore);
  }
  const size_t lineCount = includeLastLine ? lineBreakIndices.size() : lineBreakIndices.size() - 1;

  for (size_t i = 0; i < lineCount; ++i) {
    extractLine(i, pageWidth, wordWidths, wordContinues, wordNoSpaceBefore, lineBreakIndices, processLine, renderer,
                fontId);
  }

  // Remove consumed words so size() reflects only remaining words
  if (lineCount > 0) {
    consumeWords(lineBreakIndices[lineCount - 1]);
  }
}

void ParsedText::consumeWords(const size_t consumed) {
  if (consumed == 0) return;
  words.erase(words.begin(), words.begin() + consumed);
  wordStyles.erase(wordStyles.begin(), wordStyles.begin() + consumed);
  wordContinues.erase(wordContinues.begin(), wordContinues.begin() + consumed);
  wordNoSpaceBefore.erase(wordNoSpaceBefore.begin(), wordNoSpaceBefore.begin() + consumed);
  wordFocusBoundary.erase(wordFocusBoundary.begin(), wordFocusBoundary.begin() + consumed);
  if (!wordFonts.empty()) {
    const size_t wfConsumed = std::min(consumed, wordFonts.size());
    wordFonts.erase(wordFonts.begin(), wordFonts.begin() + wfConsumed);
  }
  wordLinkIds.erase(wordLinkIds.begin(), wordLinkIds.begin() + consumed);
  eraseVisibleOffsetPrefix(consumed);
  if (!rubyTexts.empty()) {
    const size_t rtConsumed = std::min(consumed, rubyTexts.size());
    rubyTexts.erase(rubyTexts.begin(), rubyTexts.begin() + rtConsumed);
  }
}

static inline bool isCjkIdeograph(uint32_t cp) {
  return (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
         (cp >= 0x20000 && cp <= 0x3FFFF);
}

// The first word of a line may have its ruby characters wider than the word (the base text). In that case, we need to
// move the base text to the right a bit so that ruby text doesn't overflow the left border, and it is still centered
// over the base text. This function calculates how much we need to move the base text to the right.
int ParsedText::calculateRubyExtraStartOffset(const size_t wordIdx, const size_t maxWordIdx,
                                              const GfxRenderer& renderer, const int fontId) const {
  if (rubyTexts.empty() || wordIdx >= rubyTexts.size() || rubyTexts[wordIdx].empty() ||
      (wordStyles[wordIdx] & EpdFontFamily::RUBY_CONTINUE) != 0) {
    return 0;
  }

  size_t groupWordCount = 1;
  while (wordIdx + groupWordCount < maxWordIdx &&
         (wordStyles[wordIdx + groupWordCount] & EpdFontFamily::RUBY_CONTINUE) != 0) {
    groupWordCount++;
  }
  int groupActualWidth = 0;
  for (size_t k = 0; k < groupWordCount; ++k) {
    groupActualWidth +=
        measureWordWidth(renderer, fontId, words[wordIdx + k], wordStyles[wordIdx + k], blockStyle.letterSpacing);
  }
  const int rubyWidth =
      renderer.getTextAdvanceX(fontId, rubyTexts[wordIdx].c_str(), EpdFontFamily::SUP, blockStyle.letterSpacing);
  if (rubyWidth <= groupActualWidth) {
    return 0;
  }

  const int leftOverlap = (rubyWidth - groupActualWidth) / 2;

  // This function is only ever called for the first word of a line.
  // words[wordIdx - 1], if it exists, is always the last word of the *prior* line
  // and cannot absorb any left overhang on the current line.
  // The full leftOverlap must therefore be reserved as a visual indent so the
  // ruby text does not overflow the left margin.
  return leftOverlap;
}

// The last ruby group on a line may have its ruby characters wider than the group's base text.
// The right half of that overhang protrudes past the last base character. This function returns
// the amount of right-margin space that must be reserved so the ruby does not overflow the right
// border. It mirrors calculateRubyExtraStartOffset: words[lineBreak] is on the *next* line and
// cannot absorb any of the right overhang on the current line, so the full rightOverlap is returned.
int ParsedText::calculateRubyExtraEndOffset(const size_t lineStartIdx, const size_t lineBreakIdx,
                                            const GfxRenderer& renderer, const int fontId) const {
  if (rubyTexts.empty() || lineBreakIdx == 0 || lineStartIdx >= lineBreakIdx) {
    return 0;
  }

  // Walk backwards from the last word to find the leader of the last ruby group on the line.
  size_t leaderIdx = lineBreakIdx - 1;
  while (leaderIdx > lineStartIdx && (wordStyles[leaderIdx] & EpdFontFamily::RUBY_CONTINUE) != 0) {
    leaderIdx--;
  }

  // leaderIdx must be a ruby group leader (non-empty ruby, no RUBY_CONTINUE flag).
  if (leaderIdx >= rubyTexts.size() || rubyTexts[leaderIdx].empty() ||
      (wordStyles[leaderIdx] & EpdFontFamily::RUBY_CONTINUE) != 0) {
    return 0;
  }

  // Measure the group.
  int groupActualWidth = 0;
  for (size_t k = leaderIdx; k < lineBreakIdx; ++k) {
    groupActualWidth += measureWordWidth(renderer, fontId, words[k], wordStyles[k], blockStyle.letterSpacing);
  }
  const int rubyWidth =
      renderer.getTextAdvanceX(fontId, rubyTexts[leaderIdx].c_str(), EpdFontFamily::SUP, blockStyle.letterSpacing);
  if (rubyWidth <= groupActualWidth) {
    return 0;
  }

  return (rubyWidth - groupActualWidth) / 2;
}

std::vector<uint16_t> ParsedText::calculateWordWidths(const GfxRenderer& renderer, const int fontId) {
  std::vector<uint16_t> wordWidths;
  wordWidths.reserve(words.size());

  for (size_t i = 0; i < words.size(); ++i) {
    // A word with an inline font-size measures with its own font -- the widths feed the line
    // breaker and the x positions, so measuring here with the block font while drawing with
    // the override is exactly the layout/draw disagreement resolveFontId() exists to prevent.
    wordWidths.push_back(measureFocusWordWidth(renderer, effectiveWordFont(i, fontId), words[i], wordStyles[i],
                                               wordFocusBoundary[i], blockStyle.letterSpacing));
  }

  // Adjust widths for ruby groups to comply with JLReq standards
  if (!rubyTexts.empty()) {
    struct RubyGroupInfo {
      size_t start;
      size_t count;
      int baseWidth;
      int rubyWidth;
      int leftOverlap;
      int rightOverlap;
    };

    std::vector<RubyGroupInfo> groups;
    for (size_t i = 0; i < words.size(); ++i) {
      if (i < rubyTexts.size() && !rubyTexts[i].empty() && (wordStyles[i] & EpdFontFamily::RUBY_CONTINUE) == 0) {
        RubyGroupInfo g;
        g.start = i;
        g.baseWidth = wordWidths[i];
        g.count = 1;
        while (i + g.count < words.size() && (wordStyles[i + g.count] & EpdFontFamily::RUBY_CONTINUE) != 0) {
          g.baseWidth += wordWidths[i + g.count];
          g.count++;
        }
        g.rubyWidth =
            renderer.getTextAdvanceX(fontId, rubyTexts[i].c_str(), EpdFontFamily::SUP, blockStyle.letterSpacing);
        g.leftOverlap = std::max(0, (g.rubyWidth - g.baseWidth) / 2);
        g.rightOverlap = std::max(0, (g.rubyWidth - g.baseWidth) / 2);
        groups.push_back(g);
        i += g.count - 1;
      }
    }

    // Adjust widths based on adjacent characters and group-to-group spacing
    for (size_t gIdx = 0; gIdx < groups.size(); ++gIdx) {
      const auto& g = groups[gIdx];

      // 1. Preceding character (left overhang)
      if (g.start > 0) {
        const uint32_t cpPrev = lastCodepoint(words[g.start - 1]);
        if (isCjkIdeograph(cpPrev)) {
          wordWidths[g.start - 1] += g.leftOverlap;
        } else {
          const int maxLeftOverhang = wordWidths[g.start - 1] / 2;
          wordWidths[g.start - 1] += std::max(0, g.leftOverlap - maxLeftOverhang);
        }
      }

      // 2. Succeeding character (right overhang / group collision)
      const size_t nextIdx = g.start + g.count;
      if (nextIdx < words.size()) {
        if (gIdx + 1 < groups.size() && groups[gIdx + 1].start == nextIdx) {
          // Adjacent ruby groups: compute collision
          const auto& nextG = groups[gIdx + 1];
          const int collision = g.rightOverlap + nextG.leftOverlap;
          if (collision > 0) {
            wordWidths[g.start + g.count - 1] += collision;
          }
        } else {
          // Regular character following: check if it's Kanji
          const uint32_t cpNext = firstCodepoint(words[nextIdx]);
          if (isCjkIdeograph(cpNext)) {
            wordWidths[g.start + g.count - 1] += g.rightOverlap;
          } else {
            const int maxRightOverhang = wordWidths[nextIdx] / 2;
            wordWidths[g.start + g.count - 1] += std::max(0, g.rightOverlap - maxRightOverhang);
          }

          // Check if there is another ruby group further ahead separated only by non-ideographs
          if (gIdx + 1 < groups.size()) {
            const auto& nextG = groups[gIdx + 1];
            bool onlyNonIdeographsInBetween = true;
            int gapWidth = 0;
            for (size_t k = nextIdx; k < nextG.start; ++k) {
              const uint32_t cp = firstCodepoint(words[k]);
              if (isCjkIdeograph(cp)) {
                onlyNonIdeographsInBetween = false;
                break;
              }
              gapWidth += wordWidths[k];
            }
            if (onlyNonIdeographsInBetween) {
              const int maxRightOverhang = wordWidths[g.start + g.count - 1] / 2;
              const int maxLeftOverhang = wordWidths[nextG.start - 1] / 2;
              const int allowedRight = std::min(g.rightOverlap, maxRightOverhang);
              const int allowedLeft = std::min(nextG.leftOverlap, maxLeftOverhang);
              const int touchOverlap = allowedRight + allowedLeft - gapWidth;
              if (touchOverlap > 0) {
                wordWidths[g.start + g.count - 1] += touchOverlap;
              }
            }
          }
        }
      }
    }
  }

  return wordWidths;
}

std::vector<size_t> ParsedText::computeLineBreaks(const GfxRenderer& renderer, const int fontId, const int pageWidth,
                                                  std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                                                  std::vector<bool>& noSpaceBeforeVec) {
  if (words.empty()) {
    return {};
  }

  // Line 0 of THIS pass, through the same resolver extractLine uses: after a drop cap's
  // lines the paragraph tail re-enters here as line 0 with no first-line indent, and a
  // width measured against one the drawing would not apply breaks the line short.
  const int firstLineIndent = resolveLineIndent(0, renderer, fontId);

  // Ensure any word that would overflow even as the first entry on a line is split using fallback hyphenation.
  for (size_t i = 0; i < wordWidths.size(); ++i) {
    // First word needs to fit in reduced width if there's an indent
    const int effectiveWidth = i == 0 ? pageWidth - firstLineIndent : pageWidth;
    while (wordWidths[i] > effectiveWidth) {
      if (!hyphenateWordAtIndex(i, effectiveWidth, renderer, fontId, wordWidths, /*allowFallbackBreaks=*/true)) {
        break;
      }
    }
  }

  const size_t totalWordCount = words.size();

  // DP table to store the minimum badness (cost) of lines starting at index i
  std::vector<int> dp(totalWordCount);
  // 'ans[i]' stores the index 'j' of the *last word* in the optimal line starting at 'i'
  std::vector<size_t> ans(totalWordCount);

  // Base Case
  dp[totalWordCount - 1] = 0;
  ans[totalWordCount - 1] = totalWordCount - 1;

  for (int i = totalWordCount - 2; i >= 0; --i) {
    int currlen = 0;
    dp[i] = MAX_COST;

    // First line has reduced width due to text-indent
    const int effectivePageWidth = i == 0 ? pageWidth - firstLineIndent : pageWidth;

    for (size_t j = i; j < totalWordCount; ++j) {
      // Add space before word j, unless it's the first word on the line or a continuation
      int gap = 0;
      if (j > static_cast<size_t>(i) && continuesVec[j]) {
        gap = renderer.getKerning(fontId, lastCodepoint(words[j - 1]), firstCodepoint(words[j]), wordStyles[j - 1]);
      } else if (j > static_cast<size_t>(i) && noSpaceBeforeVec[j]) {
        gap = 0;
      } else if (j > static_cast<size_t>(i)) {
        gap = renderer.getSpaceAdvance(fontId, lastCodepoint(words[j - 1]), firstCodepoint(words[j]), wordStyles[j - 1],
                                       blockStyle.letterSpacing);
      }

      // Calculate extraStartOffset for the first word on the line (i) (protect left margin)
      const int extraStartOffset = (j == i) ? calculateRubyExtraStartOffset(i, totalWordCount, renderer, fontId) : 0;

      currlen += wordWidths[j] + gap + (j == i ? extraStartOffset : 0);

      if (currlen > effectivePageWidth) {
        break;
      }

      if (j + 1 < totalWordCount && !TokenBoundary::allowsBreak(continuesVec[j + 1], noSpaceBeforeVec[j + 1])) {
        continue;
      }

      const int extraEndOffset = calculateRubyExtraEndOffset(i, j + 1, renderer, fontId);

      if (currlen + extraEndOffset > effectivePageWidth) {
        continue;  // Cannot split here as it would overflow the right margin
      }

      int cost;
      if (j == totalWordCount - 1) {
        cost = 0;  // Last line
      } else {
        const int remainingSpace = effectivePageWidth - currlen;
        // Use long long for the square to prevent overflow
        const long long cost_ll = static_cast<long long>(remainingSpace) * remainingSpace + dp[j + 1];

        if (cost_ll > MAX_COST) {
          cost = MAX_COST;
        } else {
          cost = static_cast<int>(cost_ll);
        }
      }

      // Favor longer lines when line-breaking costs are equal, to avoid unnecessary short lines in Chinese and Japanese
      // text.
      if (cost <= dp[i]) {
        dp[i] = cost;
        ans[i] = j;  // j is the index of the last word in this optimal line
      }
    }

    // Handle oversized word: if no valid configuration found, force single-word line
    // This prevents cascade failure where one oversized word breaks all preceding words
    if (dp[i] == MAX_COST) {
      ans[i] = i;  // Just this word on its own line
      // Inherit cost from next word to allow subsequent words to find valid configurations
      if (i + 1 < static_cast<int>(totalWordCount)) {
        dp[i] = dp[i + 1];
      } else {
        dp[i] = 0;
      }
    }
  }

  // Stores the index of the word that starts the next line (last_word_index + 1)
  std::vector<size_t> lineBreakIndices;
  size_t currentWordIndex = 0;

  while (currentWordIndex < totalWordCount) {
    size_t nextBreakIndex = ans[currentWordIndex] + 1;

    // Safety check: prevent infinite loop if nextBreakIndex doesn't advance
    if (nextBreakIndex <= currentWordIndex) {
      // Force advance by at least one word to avoid infinite loop
      nextBreakIndex = currentWordIndex + 1;
    }

    lineBreakIndices.push_back(nextBreakIndex);
    currentWordIndex = nextBreakIndex;
  }

  return lineBreakIndices;
}

// Builds break indices while opportunistically splitting the word that would overflow the current line.
std::vector<size_t> ParsedText::computeHyphenatedLineBreaks(const GfxRenderer& renderer, const int fontId,
                                                            const int pageWidth, std::vector<uint16_t>& wordWidths,
                                                            std::vector<bool>& continuesVec,
                                                            std::vector<bool>& noSpaceBeforeVec) {
  // Line 0 of THIS pass, through the same resolver extractLine uses: after a drop cap's
  // lines the paragraph tail re-enters here as line 0 with no first-line indent, and a
  // width measured against one the drawing would not apply breaks the line short.
  const int firstLineIndent = resolveLineIndent(0, renderer, fontId);

  std::vector<size_t> lineBreakIndices;
  size_t currentIndex = 0;
  bool isFirstLine = true;

  while (currentIndex < wordWidths.size()) {
    const size_t lineStart = currentIndex;
    int lineWidth = 0;

    // First line has reduced width due to text-indent
    const int effectivePageWidth = isFirstLine ? pageWidth - firstLineIndent : pageWidth;

    // Consume as many words as possible for current line, splitting when prefixes fit
    while (currentIndex < wordWidths.size()) {
      const bool isFirstWord = currentIndex == lineStart;
      int spacing = 0;
      if (!isFirstWord && continuesVec[currentIndex]) {
        spacing = renderer.getKerning(fontId, lastCodepoint(words[currentIndex - 1]),
                                      firstCodepoint(words[currentIndex]), wordStyles[currentIndex - 1]);
      } else if (!isFirstWord && noSpaceBeforeVec[currentIndex]) {
        spacing = 0;
      } else if (!isFirstWord) {
        spacing = renderer.getSpaceAdvance(fontId, lastCodepoint(words[currentIndex - 1]),
                                           firstCodepoint(words[currentIndex]), wordStyles[currentIndex - 1],
                                           blockStyle.letterSpacing);
      }
      const int candidateWidth = spacing + wordWidths[currentIndex];

      // Word fits on current line
      if (lineWidth + candidateWidth <= effectivePageWidth) {
        lineWidth += candidateWidth;
        ++currentIndex;
        continue;
      }

      // Word would overflow — try to split based on hyphenation points
      const int availableWidth = effectivePageWidth - lineWidth - spacing;
      const bool allowFallbackBreaks = isFirstWord;  // Only for first word on line

      if (availableWidth > 0 &&
          hyphenateWordAtIndex(currentIndex, availableWidth, renderer, fontId, wordWidths, allowFallbackBreaks)) {
        // Prefix now fits; append it to this line and move to next line
        lineWidth += spacing + wordWidths[currentIndex];
        ++currentIndex;
        break;
      }

      // Could not split: force at least one word per line to avoid infinite loop
      if (currentIndex == lineStart) {
        lineWidth += candidateWidth;
        ++currentIndex;
      }
      break;
    }

    // Don't break before a continuation word (e.g., orphaned "?" after "question").
    // Backtrack to the start of the continuation group so the whole group moves to the next line.
    while (currentIndex > lineStart + 1 && currentIndex < wordWidths.size() &&
           !TokenBoundary::allowsBreak(continuesVec[currentIndex], noSpaceBeforeVec[currentIndex])) {
      --currentIndex;
    }

    lineBreakIndices.push_back(currentIndex);
    isFirstLine = false;
  }

  return lineBreakIndices;
}

// Splits words[wordIndex] into prefix (adding a hyphen only when needed) and remainder when a legal breakpoint fits the
// available width.
bool ParsedText::hyphenateWordAtIndex(const size_t wordIndex, const int availableWidth, const GfxRenderer& renderer,
                                      const int fontId, std::vector<uint16_t>& wordWidths,
                                      const bool allowFallbackBreaks) {
  // Guard against invalid indices or zero available width before attempting to split.
  if (availableWidth <= 0 || wordIndex >= words.size()) {
    return false;
  }

  const std::string& word = words[wordIndex];
  const auto style = wordStyles[wordIndex];
  const uint8_t focusBoundary = wordFocusBoundary[wordIndex];
  // Prefix/remainder widths must use the same font the whole word was measured with.
  const int wordFont = effectiveWordFont(wordIndex, fontId);

  // Collect candidate breakpoints (byte offsets and hyphen requirements).
  auto breakInfos = Hyphenator::breakOffsets(word, allowFallbackBreaks);
  if (breakInfos.empty()) {
    return false;
  }

  size_t chosenOffset = 0;
  int chosenWidth = -1;
  bool chosenNeedsHyphen = true;

  // Iterate over each legal breakpoint and retain the widest prefix that still fits.
  for (const auto& info : breakInfos) {
    const size_t offset = info.byteOffset;
    if (offset == 0 || offset >= word.size()) {
      continue;
    }

    const bool needsHyphen = info.requiresInsertedHyphen;
    const int prefixWidth =
        measureFocusWordWidth(renderer, wordFont, word.substr(0, offset), style,
                              focusBoundaryBefore(focusBoundary, offset), blockStyle.letterSpacing, needsHyphen);
    if (prefixWidth > availableWidth || prefixWidth <= chosenWidth) {
      continue;  // Skip if too wide or not an improvement
    }

    chosenWidth = prefixWidth;
    chosenOffset = offset;
    chosenNeedsHyphen = needsHyphen;
  }

  if (chosenWidth < 0) {
    // No hyphenation point produced a prefix that fits in the remaining space.
    return false;
  }

  uint32_t remainderOffset = visibleOffsetAt(wordIndex);
  const unsigned char* offsetPtr = reinterpret_cast<const unsigned char*>(word.data());
  const unsigned char* splitPtr = offsetPtr + chosenOffset;
  while (offsetPtr < splitPtr) {
    utf8NextCodepoint(&offsetPtr);
    remainderOffset++;
  }

  // Split the word at the selected breakpoint and append a hyphen if required.
  std::string remainder = word.substr(chosenOffset);
  words[wordIndex].resize(chosenOffset);
  if (chosenNeedsHyphen) {
    words[wordIndex].push_back('-');
  }

  // Insert the remainder word (with matching style and continuation flag) directly after the prefix.
  words.insert(words.begin() + wordIndex + 1, remainder);
  wordStyles.insert(wordStyles.begin() + wordIndex + 1, style);
  if (!wordFonts.empty()) {
    wordFonts.insert(wordFonts.begin() + wordIndex + 1, wordFonts[wordIndex]);
  }
  insertVisibleOffset(wordIndex + 1, remainderOffset);
  wordFocusBoundary.insert(wordFocusBoundary.begin() + wordIndex + 1, focusBoundaryAfter(focusBoundary, chosenOffset));
  wordLinkIds.insert(wordLinkIds.begin() + wordIndex + 1, wordLinkIds[wordIndex]);
  wordFocusBoundary[wordIndex] = focusBoundaryBefore(focusBoundary, chosenOffset);
  if (wordFocusBoundary[wordIndex] >= words[wordIndex].size()) {
    wordStyles[wordIndex] = static_cast<EpdFontFamily::Style>(wordStyles[wordIndex] | EpdFontFamily::BOLD);
    wordFocusBoundary[wordIndex] = 0;
  }
  if (wordIndex + 1 <= rubyTexts.size()) {
    rubyTexts.insert(rubyTexts.begin() + wordIndex + 1, "");
  }

  // Continuation flag handling after splitting a word into prefix + remainder.
  //
  // The prefix keeps the original word's continuation flag so that no-break-space groups
  // stay linked. The remainder always gets continues=false because it starts on the next
  // line and is not attached to the prefix.
  //
  // Example: "200&#xA0;Quadratkilometer" produces tokens:
  //   [0] "200"               continues=false
  //   [1] " "                 continues=true
  //   [2] "Quadratkilometer"  continues=true   <-- the word being split
  //
  // After splitting "Quadratkilometer" at "Quadrat-" / "kilometer":
  //   [0] "200"         continues=false
  //   [1] " "           continues=true
  //   [2] "Quadrat-"    continues=true   (KEPT — still attached to the no-break group)
  //   [3] "kilometer"   continues=false  (NEW — starts fresh on the next line)
  //
  // This lets the backtracking loop keep the entire prefix group ("200 Quadrat-") on one
  // line, while "kilometer" moves to the next line.
  // wordContinues[wordIndex] is intentionally left unchanged — the prefix keeps its original attachment.
  wordContinues.insert(wordContinues.begin() + wordIndex + 1, false);
  wordNoSpaceBefore.insert(wordNoSpaceBefore.begin() + wordIndex + 1, false);

  // Update cached widths to reflect the new prefix/remainder pairing.
  wordWidths[wordIndex] = static_cast<uint16_t>(chosenWidth);
  const uint16_t remainderWidth = measureFocusWordWidth(renderer, wordFont, remainder, style,
                                                        wordFocusBoundary[wordIndex + 1], blockStyle.letterSpacing);
  wordWidths.insert(wordWidths.begin() + wordIndex + 1, remainderWidth);
  return true;
}

void ParsedText::extractLine(const size_t breakIndex, const int pageWidth, const std::vector<uint16_t>& wordWidths,
                             const std::vector<bool>& continuesVec, const std::vector<bool>& noSpaceBeforeVec,
                             const std::vector<size_t>& lineBreakIndices,
                             const std::function<void(std::shared_ptr<TextBlock>, uint32_t)>& processLine,
                             const GfxRenderer& renderer, const int fontId) {
  const size_t lineBreak = lineBreakIndices[breakIndex];
  const size_t lastBreakAt = breakIndex > 0 ? lineBreakIndices[breakIndex - 1] : 0;
  const size_t lineWordCount = lineBreak - lastBreakAt;
  const uint32_t lineVisibleOffset = visibleOffsetAt(lastBreakAt);

  const int firstLineIndent = resolveLineIndent(breakIndex, renderer, fontId);

  std::vector<std::string> lineRubyTexts(lineWordCount);
  if (!rubyTexts.empty() && lastBreakAt < rubyTexts.size()) {
    const size_t copyCount = std::min(lineBreak, rubyTexts.size()) - lastBreakAt;
    std::copy(rubyTexts.begin() + lastBreakAt, rubyTexts.begin() + lastBreakAt + copyCount, lineRubyTexts.begin());
  }

  const int extraStartOffset = calculateRubyExtraStartOffset(lastBreakAt, lineBreak, renderer, fontId);
  const int extraEndOffset = calculateRubyExtraEndOffset(lastBreakAt, lineBreak, renderer, fontId);

  std::vector<std::string> lineWords;
  lineWords.reserve(lineWordCount);
  std::vector<EpdFontFamily::Style> lineWordStyles;
  lineWordStyles.reserve(lineWordCount);
  // Sliced only when the block carries per-word fonts; stays empty (no allocation) otherwise.
  std::vector<int32_t> lineWordFonts;
  if (!wordFonts.empty()) {
    lineWordFonts.reserve(lineWordCount);
  }

  bool lineHasFontOverride = false;
  for (size_t i = 0; i < lineWordCount; ++i) {
    std::string word = std::move(words[lastBreakAt + i]);
    if (containsSoftHyphen(word)) {
      stripSoftHyphensInPlace(word);
    }
    lineWords.push_back(std::move(word));
    lineWordStyles.push_back(wordStyles[lastBreakAt + i]);
    if (!wordFonts.empty()) {
      const int32_t wf = lastBreakAt + i < wordFonts.size() ? wordFonts[lastBreakAt + i] : 0;
      lineWordFonts.push_back(wf);
      if (wf != 0) lineHasFontOverride = true;
    }
  }
  // A block-level wordFonts only means SOME word in the block is sized. THIS line's slice may
  // still be all zero (the sized span sat on an earlier/later line); drop it so TextBlock keeps
  // the no-fonts arena layout and addLineToPage skips its per-word scan for this line.
  if (!lineHasFontOverride) {
    lineWordFonts.clear();
  }

  // Calculate total word width for this line, count actual word gaps,
  // and accumulate total natural gap widths (including space kerning adjustments).
  int lineWordWidthSum = 0;
  size_t actualGapCount = 0;
  int totalNaturalGaps = 0;

  for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
    lineWordWidthSum += wordWidths[lastBreakAt + wordIdx];
    if (wordIdx == 0) continue;
    const size_t boundaryIdx = lastBreakAt + wordIdx;
    if (TokenBoundary::isJustifiableGap(continuesVec[boundaryIdx], noSpaceBeforeVec[boundaryIdx],
                                        lineWords[wordIdx] == " ")) {
      actualGapCount++;
    }
    if (continuesVec[boundaryIdx]) {
      totalNaturalGaps += renderer.getKerning(fontId, lastCodepoint(lineWords[wordIdx - 1]),
                                              firstCodepoint(lineWords[wordIdx]), lineWordStyles[wordIdx - 1]);
    } else if (!noSpaceBeforeVec[boundaryIdx]) {
      totalNaturalGaps +=
          renderer.getSpaceAdvance(fontId, lastCodepoint(lineWords[wordIdx - 1]), firstCodepoint(lineWords[wordIdx]),
                                   lineWordStyles[wordIdx - 1], blockStyle.letterSpacing);
    }
  }

  // Calculate spacing (account for indent reducing effective page width on first line)
  const int effectivePageWidth = pageWidth - firstLineIndent;
  const bool isLastLine = breakIndex == lineBreakIndices.size() - 1;

  // For RTL, implicit/default Left alignment becomes Right alignment.
  // Explicit text-align:left must remain left for CSS correctness.
  const CssTextAlign effectiveAlignment =
      (blockStyle.isRtl && !blockStyle.textAlignDefined && blockStyle.alignment == CssTextAlign::Left)
          ? CssTextAlign::Right
          : blockStyle.alignment;

  // For justified text, compute per-gap extra to distribute remaining space evenly.
  // extraEndOffset reserves space for any ruby group at the right edge of the line.
  const int spareSpace = effectivePageWidth - extraStartOffset - extraEndOffset - lineWordWidthSum - totalNaturalGaps;
  const int justifyExtra = (effectiveAlignment == CssTextAlign::Justify && !isLastLine)
                               ? computeJustifyExtra(spareSpace, actualGapCount)
                               : 0;

  // BiDi processing: reorder words with UAX#9 in full-line context.
  visualOrderScratch.clear();
  visualOrderScratch.reserve(lineWordCount);
  // Skip expensive visual-order resolution for pure LTR paragraphs that have no RTL words.
  const bool shouldResolveVisualOrder = blockStyle.isRtl || hasRtlWord;
  const bool willReorder =
      shouldResolveVisualOrder && BidiUtils::computeVisualWordOrder(lineWords, blockStyle.isRtl, visualOrderScratch);

  std::vector<int16_t> lineXPos;
  lineXPos.reserve(lineWordCount);

  if (willReorder) {
    reorderedWordsScratch.clear();
    reorderedStylesScratch.clear();
    reorderedFontsScratch.clear();
    reorderedWidthsScratch.clear();
    reorderedContinuesScratch.clear();
    reorderedNoSpaceBeforeScratch.clear();
    reorderedFocusBoundaryScratch.clear();
    reorderedWordsScratch.reserve(visualOrderScratch.size());
    reorderedStylesScratch.reserve(visualOrderScratch.size());
    reorderedWidthsScratch.reserve(visualOrderScratch.size());
    reorderedContinuesScratch.reserve(visualOrderScratch.size());
    reorderedNoSpaceBeforeScratch.reserve(visualOrderScratch.size());
    reorderedFocusBoundaryScratch.reserve(visualOrderScratch.size());
    if (!lineWordFonts.empty()) {
      reorderedFontsScratch.reserve(visualOrderScratch.size());
    }

    std::vector<std::string> reorderedRubyScratch;
    reorderedRubyScratch.reserve(visualOrderScratch.size());

    for (size_t i = 0; i < visualOrderScratch.size(); ++i) {
      const uint16_t src = visualOrderScratch[i];
      reorderedWordsScratch.push_back(std::move(lineWords[src]));
      reorderedStylesScratch.push_back(lineWordStyles[src]);
      if (!lineWordFonts.empty()) {
        reorderedFontsScratch.push_back(lineWordFonts[src]);
      }
      reorderedRubyScratch.push_back(src < lineRubyTexts.size() ? std::move(lineRubyTexts[src]) : std::string{});
      reorderedWidthsScratch.push_back(wordWidths[lastBreakAt + src]);
      reorderedFocusBoundaryScratch.push_back(wordFocusBoundary[lastBreakAt + src]);

      // Continuation means "no break/gap between two adjacent logical tokens".
      // After visual reordering (common in RTL), an adjacent logical pair can appear
      // as either (prev -> curr) or (curr -> prev) in visual order; preserve both.
      bool continues = false;
      if (i > 0) {
        const size_t prevSrc = visualOrderScratch[i - 1];
        const size_t currSrc = src;
        const bool forwardAdjacent = currSrc == prevSrc + 1;
        const bool reverseAdjacent = prevSrc == currSrc + 1;

        if (forwardAdjacent && continuesVec[lastBreakAt + currSrc]) {
          continues = true;
        } else if (reverseAdjacent && continuesVec[lastBreakAt + prevSrc]) {
          continues = true;
        }
      }
      reorderedContinuesScratch.push_back(continues);
      reorderedNoSpaceBeforeScratch.push_back(!continues && noSpaceBeforeVec[lastBreakAt + src]);
    }

    int reorderedWordWidthSum = 0;
    size_t reorderedGapCount = 0;
    int reorderedNaturalGaps = 0;
    for (size_t wordIdx = 0; wordIdx < reorderedWidthsScratch.size(); wordIdx++) {
      reorderedWordWidthSum += reorderedWidthsScratch[wordIdx];
      if (wordIdx > 0 && reorderedNoSpaceBeforeScratch[wordIdx]) {
        // Unicode break opportunity with no inserted Latin-style space. It is still
        // a stretchable gap for justified CJK/Korean text.
        reorderedGapCount++;
      } else if (wordIdx > 0 && !reorderedContinuesScratch[wordIdx]) {
        reorderedGapCount++;
        reorderedNaturalGaps += renderer.getSpaceAdvance(fontId, lastCodepoint(reorderedWordsScratch[wordIdx - 1]),
                                                         firstCodepoint(reorderedWordsScratch[wordIdx]),
                                                         reorderedStylesScratch[wordIdx - 1], blockStyle.letterSpacing);
      } else if (wordIdx > 0 && reorderedContinuesScratch[wordIdx]) {
        if (reorderedWordsScratch[wordIdx] == " ") {
          reorderedGapCount++;
        }
        reorderedNaturalGaps +=
            renderer.getKerning(fontId, lastCodepoint(reorderedWordsScratch[wordIdx - 1]),
                                firstCodepoint(reorderedWordsScratch[wordIdx]), reorderedStylesScratch[wordIdx - 1]);
      }
    }

    const int reorderedSpare =
        effectivePageWidth - extraStartOffset - extraEndOffset - reorderedWordWidthSum - reorderedNaturalGaps;
    const int reorderedJustifyExtra = (effectiveAlignment == CssTextAlign::Justify && !isLastLine)
                                          ? computeJustifyExtra(reorderedSpare, reorderedGapCount)
                                          : 0;

    const int justifyContribution = (effectiveAlignment == CssTextAlign::Justify && !isLastLine)
                                        ? reorderedJustifyExtra * static_cast<int>(reorderedGapCount)
                                        : 0;
    const int contentWidth = reorderedWordWidthSum + reorderedNaturalGaps + justifyContribution;

    int xpos = 0;
    if (blockStyle.isRtl) {
      if (effectiveAlignment == CssTextAlign::Right || effectiveAlignment == CssTextAlign::Justify) {
        xpos = effectivePageWidth - contentWidth;
      } else if (effectiveAlignment == CssTextAlign::Center) {
        xpos = (effectivePageWidth - contentWidth) / 2;
      }
    } else {
      xpos = firstLineIndent;
      if (effectiveAlignment == CssTextAlign::Right) {
        xpos = effectivePageWidth - contentWidth;
      } else if (effectiveAlignment == CssTextAlign::Center) {
        xpos = (effectivePageWidth - contentWidth) / 2;
      }
    }

    for (size_t wordIdx = 0; wordIdx < reorderedWidthsScratch.size(); wordIdx++) {
      lineXPos.push_back(static_cast<int16_t>(xpos));
      xpos += reorderedWidthsScratch[wordIdx];

      const bool nextIsContinuation =
          wordIdx + 1 < reorderedWidthsScratch.size() && reorderedContinuesScratch[wordIdx + 1];
      if (nextIsContinuation) {
        int advance =
            renderer.getKerning(fontId, lastCodepoint(reorderedWordsScratch[wordIdx]),
                                firstCodepoint(reorderedWordsScratch[wordIdx + 1]), reorderedStylesScratch[wordIdx]);
        // wordIdx > 0 mirrors the gap accounting above (which skips index 0): a leading
        // no-break space must not receive justifyExtra, or the line over-stretches by one
        // gap and the last word is pushed past the right margin (issue #2185).
        if (wordIdx > 0 && reorderedWordsScratch[wordIdx] == " " && reorderedContinuesScratch[wordIdx] &&
            effectiveAlignment == CssTextAlign::Justify && !isLastLine) {
          advance += reorderedJustifyExtra;
        }
        xpos += advance;
      } else if (wordIdx + 1 < reorderedWidthsScratch.size()) {
        const bool nextNoSpace = reorderedNoSpaceBeforeScratch[wordIdx + 1];
        int gap = nextNoSpace ? 0
                              : renderer.getSpaceAdvance(fontId, lastCodepoint(reorderedWordsScratch[wordIdx]),
                                                         firstCodepoint(reorderedWordsScratch[wordIdx + 1]),
                                                         reorderedStylesScratch[wordIdx], blockStyle.letterSpacing);
        if (effectiveAlignment == CssTextAlign::Justify && !isLastLine) {
          gap += reorderedJustifyExtra;
        }
        xpos += gap;
      }
    }

    lineWords.swap(reorderedWordsScratch);
    lineWordStyles.swap(reorderedStylesScratch);
    lineRubyTexts.swap(reorderedRubyScratch);
    if (!lineWordFonts.empty()) {
      lineWordFonts.swap(reorderedFontsScratch);
    }
  } else {
    // Standard LTR/RTL positioning loop when no visual reordering is needed
    if (blockStyle.isRtl) {
      // RTL: position words from right to left
      int xpos = effectivePageWidth;
      if (effectiveAlignment == CssTextAlign::Left) {
        // Explicit left alignment in RTL context
        xpos = lineWordWidthSum + totalNaturalGaps;
      } else if (effectiveAlignment == CssTextAlign::Center) {
        xpos = (effectivePageWidth + lineWordWidthSum + totalNaturalGaps) / 2;
      }
      // For Right and Justify, start from right edge (xpos = effectivePageWidth)

      for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
        xpos -= wordWidths[lastBreakAt + wordIdx];
        lineXPos.push_back(static_cast<int16_t>(xpos));

        const bool nextIsContinuation = wordIdx + 1 < lineWordCount && continuesVec[lastBreakAt + wordIdx + 1];
        if (nextIsContinuation) {
          // Cross-boundary kerning for continuation words
          int advance = renderer.getKerning(fontId, lastCodepoint(lineWords[wordIdx]),
                                            firstCodepoint(lineWords[wordIdx + 1]), lineWordStyles[wordIdx]);
          // wordIdx > 0: see the LTR branch — a leading no-break space is not a justifiable gap.
          if (wordIdx > 0 && lineWords[wordIdx] == " " && continuesVec[lastBreakAt + wordIdx] &&
              effectiveAlignment == CssTextAlign::Justify && !isLastLine) {
            advance += justifyExtra;
          }
          xpos -= advance;
        } else {
          int gap = 0;
          bool nextNoSpace = false;
          if (wordIdx + 1 < lineWordCount) {
            nextNoSpace = noSpaceBeforeVec[lastBreakAt + wordIdx + 1];
            gap = nextNoSpace ? 0
                              : renderer.getSpaceAdvance(fontId, lastCodepoint(lineWords[wordIdx]),
                                                         firstCodepoint(lineWords[wordIdx + 1]),
                                                         lineWordStyles[wordIdx], blockStyle.letterSpacing);
          }
          if (wordIdx + 1 < lineWordCount && effectiveAlignment == CssTextAlign::Justify && !isLastLine) {
            gap += justifyExtra;
          }
          xpos -= gap;
        }
      }
    } else {
      // LTR: position words from left to right
      int xpos = firstLineIndent + extraStartOffset;
      if (effectiveAlignment == CssTextAlign::Right) {
        xpos = effectivePageWidth - lineWordWidthSum - totalNaturalGaps;
      } else if (effectiveAlignment == CssTextAlign::Center) {
        xpos = (effectivePageWidth - lineWordWidthSum - totalNaturalGaps) / 2;
      }

      for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
        lineXPos.push_back(static_cast<int16_t>(xpos));

        const bool nextIsContinuation = wordIdx + 1 < lineWordCount && continuesVec[lastBreakAt + wordIdx + 1];
        if (nextIsContinuation) {
          int advance = wordWidths[lastBreakAt + wordIdx];
          advance += renderer.getKerning(fontId, lastCodepoint(lineWords[wordIdx]),
                                         firstCodepoint(lineWords[wordIdx + 1]), lineWordStyles[wordIdx]);
          // wordIdx > 0 mirrors the gap accounting above (which skips index 0): a leading
          // no-break space must not receive justifyExtra, or the line over-stretches by one
          // gap and the last word is pushed past the right margin (issue #2185).
          if (wordIdx > 0 && lineWords[wordIdx] == " " && continuesVec[lastBreakAt + wordIdx] &&
              effectiveAlignment == CssTextAlign::Justify && !isLastLine) {
            advance += justifyExtra;
          }
          xpos += advance;
        } else {
          int gap = 0;
          bool nextNoSpace = false;
          if (wordIdx + 1 < lineWordCount) {
            nextNoSpace = noSpaceBeforeVec[lastBreakAt + wordIdx + 1];
            gap = nextNoSpace ? 0
                              : renderer.getSpaceAdvance(fontId, lastCodepoint(lineWords[wordIdx]),
                                                         firstCodepoint(lineWords[wordIdx + 1]),
                                                         lineWordStyles[wordIdx], blockStyle.letterSpacing);
          }
          if (wordIdx + 1 < lineWordCount && effectiveAlignment == CssTextAlign::Justify && !isLastLine) {
            gap += justifyExtra;
          }
          xpos += wordWidths[lastBreakAt + wordIdx] + gap;
        }
      }
    }
  }

  const auto focusBoundaryAt = [&](const size_t idx) {
    return willReorder ? reorderedFocusBoundaryScratch[idx] : wordFocusBoundary[lastBreakAt + idx];
  };

  std::vector<TextBlock::LinkSpan> lineLinks;
  std::vector<uint8_t> lineLinkIdsSeen;
  for (size_t i = 0; i < lineWordCount; i++) {
    const uint8_t linkId = wordLinkIds[lastBreakAt + (willReorder ? visualOrderScratch[i] : i)];
    if (linkId == 0 || linkId > linkTargets.size()) continue;

    size_t spanIndex = 0;
    while (spanIndex < lineLinkIdsSeen.size() && lineLinkIdsSeen[spanIndex] != linkId) spanIndex++;
    int width = willReorder ? reorderedWidthsScratch[i] : wordWidths[lastBreakAt + i];
    const int right = lineXPos[i] + width;
    const int topLift =
        (lineWordStyles[i] & EpdFontFamily::SUP) != 0 ? renderer.getFontAscenderSize(fontId) * 2 / 5 : 0;

    if (spanIndex == lineLinkIdsSeen.size()) {
      lineLinks.emplace_back();
      auto& span = lineLinks.back();
      strncpy(span.href, linkTargets[linkId - 1].c_str(), sizeof(span.href) - 1);
      span.href[sizeof(span.href) - 1] = '\0';
      span.x = lineXPos[i];
      span.width = static_cast<int16_t>(width);
      span.topLift = static_cast<int16_t>(topLift);
      lineLinkIdsSeen.push_back(linkId);
    } else {
      auto& span = lineLinks[spanIndex];
      const int left = std::min<int>(span.x, lineXPos[i]);
      const int mergedRight = std::max<int>(span.x + span.width, right);
      span.x = static_cast<int16_t>(left);
      span.width = static_cast<int16_t>(mergedRight - left);
      span.topLift = std::max<int16_t>(span.topLift, static_cast<int16_t>(topLift));
    }
  }

  // Fast path: no word on this line carries focus emphasis, so pass empty boundary/suffixX
  // vectors. TextBlock pays zero per-word RAM cost for these annotations when they are empty.
  bool lineHasFocusSplit = false;
  for (size_t i = 0; i < lineWordCount; i++) {
    if (focusBoundaryAt(i) != 0) {
      lineHasFocusSplit = true;
      break;
    }
  }

  if (!lineHasFocusSplit) {
    // TextBlock flattens the vectors into its arena; they stay owned here and die at return.
    auto block = std::make_shared<TextBlock>(lineWords, lineXPos, lineWordStyles, std::vector<uint8_t>{},
                                             std::vector<uint16_t>{}, blockStyle, std::move(lineRubyTexts),
                                             lineWordFonts, std::move(lineLinks));
    if (!block->valid()) {
      LOG_ERR("PTX", "Dropping line: TextBlock arena allocation failed");
      return;
    }
    if (breakIndex == 0 && dropCap.present()) block->setDropCap(dropCap);
    processLine(std::move(block), lineVisibleOffset);
    return;
  }

  // Each word already occupies one slot; add the render-time bold boundary and suffix x offset.
  std::vector<uint8_t> outBoundaries;
  std::vector<uint16_t> outSuffixX;
  outBoundaries.reserve(lineWordCount);
  outSuffixX.reserve(lineWordCount);

  for (size_t i = 0; i < lineWordCount; i++) {
    const uint8_t boundary = focusBoundaryAt(i);
    const int wordFont = !lineWordFonts.empty() && lineWordFonts[i] != 0 ? lineWordFonts[i] : fontId;
    outBoundaries.push_back(boundary);
    outSuffixX.push_back(boundary == 0 ? 0
                                       : measureFocusPrefixAdvance(renderer, wordFont, lineWords[i], lineWordStyles[i],
                                                                   boundary, blockStyle.letterSpacing));
  }

  auto block = std::make_shared<TextBlock>(lineWords, lineXPos, lineWordStyles, outBoundaries, outSuffixX, blockStyle,
                                           std::move(lineRubyTexts), lineWordFonts, std::move(lineLinks));
  if (!block->valid()) {
    LOG_ERR("PTX", "Dropping line: TextBlock arena allocation failed");
    return;
  }
  if (breakIndex == 0 && dropCap.present()) block->setDropCap(dropCap);
  processLine(std::move(block), lineVisibleOffset);
}
