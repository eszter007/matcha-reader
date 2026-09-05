#include "ChapterHtmlSlimParser.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>
#include <XmlParserUtils.h>
#include <expat.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <iterator>
#include <new>

#include "../../../../src/fontIds.h"
#include "../../../../src/util/InflectionRules.h"
#include "Epub.h"
#include "Epub/AsciiTextTransform.h"
#include "Epub/Page.h"
#include "Epub/ReaderFontScale.h"
#include "Epub/VisibleTextUtils.h"
#include "Epub/converters/ImageDecoderFactory.h"
#include "Epub/converters/ImageDimsProbe.h"
#include "Epub/converters/ImageToFramebufferDecoder.h"
#include "Epub/htmlEntities.h"

// Minimum file size (in bytes) to show indexing popup - smaller chapters don't benefit from it
constexpr size_t MIN_SIZE_FOR_POPUP = 10 * 1024;  // 10KB
constexpr size_t PARSE_BUFFER_SIZE = 4096;        // 4KB: SD reads are latency-bound (see VerticalSection)

// This number comes from PR #73
// If we have > 750 words buffered up, perform the layout and consume out all but the last line
// There should be enough here to build out 1-2 full pages and doing this will free up a lot of
// memory.
// Spotted when reading Intermezzo, there are some really long text blocks in there.
constexpr size_t TEXT_BLOCK_SOFT_FLUSH_WORDS = 750;

// When CSS is enabled, flush earlier to save RAM. 320 is still more than enough to build a CJK
// page at font size 14
constexpr size_t TEXT_BLOCK_SOFT_FLUSH_WORDS_WITH_CSS = 320;

// Hard cap on the number of anchor IDs recorded per chapter. Legitimate navigation
// anchors (TOC entries, footnotes, cross-references) rarely exceed a few hundred per
// chapter. A runaway count usually means a converter injected machine-generated IDs on
// every text fragment (e.g. Kobo KePub spans). The cap prevents unbounded heap growth
// on resource-constrained devices (~380KB heap). TOC anchors bypass this cap.
constexpr size_t MAX_ANCHORS_PER_CHAPTER = 1024;

// Reuse serializable PageLine/PageHorizontalRule elements for a small grid.
constexpr int16_t TABLE_CELL_HORIZONTAL_PADDING = 4;
constexpr int16_t TABLE_ROW_SEPARATOR_GAP = 4;
constexpr uint8_t TABLE_ROW_SEPARATOR_THICKNESS = 1;
constexpr int16_t TABLE_MIN_CELL_WIDTH_LINE_HEIGHTS = 3;

constexpr const char* HEADER_TAGS[] = {"h1", "h2", "h3", "h4", "h5", "h6"};
// HTML5 sectioning/grouping tags (aside, section, ...) are block-level in every browser and
// publishers hang block CSS on them (e.g. <aside class="box"> with border+padding for call-out
// boxes). Treating them as inline dropped their margins, borders and text-align entirely.
constexpr const char* BLOCK_TAGS[] = {"p",  "li",    "div",     "br",      "blockquote", "ol",
                                      "ul", "aside", "section", "article", "figure",     "figcaption"};

// UTF-8 mark glyph for a text-emphasis style (JP bouten), nullptr for none.
const char* emphasisMarkUtf8(const CssTextEmphasis e) {
  switch (e) {
    case CssTextEmphasis::FilledDot:
      return "\xE2\x80\xA2";  // •
    case CssTextEmphasis::OpenDot:
      return "\xE2\x97\xA6";  // ◦
    case CssTextEmphasis::FilledCircle:
      return "\xE2\x97\x8F";  // ●
    case CssTextEmphasis::OpenCircle:
      return "\xE2\x97\x8B";  // ○
    case CssTextEmphasis::FilledSesame:
      return "\xEF\xB9\x85";  // ﹅
    case CssTextEmphasis::OpenSesame:
      return "\xEF\xB9\x86";  // ﹆
    case CssTextEmphasis::FilledTriangle:
      return "\xE2\x96\xB2";  // ▲
    case CssTextEmphasis::OpenTriangle:
      return "\xE2\x96\xB3";  // △
    case CssTextEmphasis::FilledDoubleCircle:
      return "\xE2\x97\x89";  // ◉
    case CssTextEmphasis::OpenDoubleCircle:
      return "\xE2\x97\x8E";  // ◎
    default:
      return nullptr;
  }
}

// In-place uppercase for the small-caps approximation: ASCII a-z plus the
// two-byte Latin-1 range (à..þ -> À..Þ, skipping ÷ and ß). We cannot render
// true small capitals (no per-word font size), so full caps conveys the style.
void smallCapsTransform(char* s) {
  for (unsigned char* p = reinterpret_cast<unsigned char*>(s); *p; p++) {
    if (*p >= 'a' && *p <= 'z') {
      *p -= 0x20;
    } else if (*p == 0xC3 && p[1] >= 0xA0 && p[1] <= 0xBE && p[1] != 0xB7) {
      p[1] -= 0x20;
      p++;
    }
  }
}
constexpr const char* BOLD_TAGS[] = {"b", "strong"};
constexpr const char* ITALIC_TAGS[] = {"i", "em"};
constexpr const char* UNDERLINE_TAGS[] = {"u", "ins"};
constexpr const char* LINETHROUGH_TAGS[] = {"del", "s", "strike"};
constexpr const char* IMAGE_TAGS[] = {"img", "image"};
// style/script are skipped as well: EPUBs in the wild put <style> in the body,
// and without this its CSS text is emitted as paragraph text.
constexpr const char* SKIP_TAGS[] = {"head", "style", "script", "rp"};

bool isWhitespace(const char c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; }

std::string trimAndNormalize(const std::string& str) {
  if (str.empty()) return "";
  size_t start = 0;
  while (start < str.size() && isWhitespace(str[start])) {
    start++;
  }
  if (start == str.size()) return "";
  size_t end = str.size() - 1;
  while (end > start && isWhitespace(str[end])) {
    end--;
  }
  std::string result;
  result.reserve(end - start + 1);
  bool inSpace = false;
  for (size_t i = start; i <= end; i++) {
    if (isWhitespace(str[i])) {
      if (!inSpace) {
        result.push_back(' ');
        inSpace = true;
      }
    } else {
      result.push_back(str[i]);
      inSpace = false;
    }
  }
  return result;
}

bool matches(const char* tag_name, const char* const* possible_tags, size_t count) {
  for (size_t i = 0; i < count; i++) {
    if (strcmp(tag_name, possible_tags[i]) == 0) {
      return true;
    }
  }
  return false;
}

bool isNonVisibleTextTag(const char* name) { return VisibleTextUtils::isNonVisibleElement(name); }

const char* getAttribute(const XML_Char** atts, const char* attrName) {
  if (!atts) return nullptr;
  for (int i = 0; atts[i]; i += 2) {
    if (strcmp(atts[i], attrName) == 0) return atts[i + 1];
  }
  return nullptr;
}

uint16_t parseTableSpan(const char* value) {
  if (!value || value[0] == '\0') return 1;

  uint32_t span = 0;
  for (const char* current = value; *current != '\0'; ++current) {
    if (*current < '0' || *current > '9') return 1;
    const uint32_t digit = static_cast<uint32_t>(*current - '0');
    if (span > (UINT16_MAX - digit) / 10) return UINT16_MAX;
    span = span * 10 + digit;
  }
  return span == 0 ? UINT16_MAX : static_cast<uint16_t>(span);
}

// Returns true if the HTML element is a purely inline, non-navigable wrapper.
// IDs on these elements are never meaningful navigation targets in epub content.
// Reading-system converters (Kobo KePub, Calibre, etc.) frequently inject thousands
// of such IDs for progress tracking or internal bookkeeping, and recording each one
// as a navigation anchor exhausts the heap on memory-constrained devices.
// Block-level, sectioning, and structural elements are always considered navigable.
bool isNonNavigableInlineElement(const char* name) { return strcmp(name, "span") == 0; }

bool isInternalEpubLink(const char* href) {
  if (!href || href[0] == '\0') return false;
  if (strncmp(href, "http://", 7) == 0 || strncmp(href, "https://", 8) == 0) return false;
  if (strncmp(href, "mailto:", 7) == 0) return false;
  if (strncmp(href, "ftp://", 6) == 0) return false;
  if (strncmp(href, "tel:", 4) == 0) return false;
  if (strncmp(href, "javascript:", 11) == 0) return false;
  return true;
}

bool isHeaderOrBlock(const char* name) {
  return matches(name, HEADER_TAGS, std::size(HEADER_TAGS)) || matches(name, BLOCK_TAGS, std::size(BLOCK_TAGS));
}

bool isTableStructuralTag(const char* name) {
  return strcmp(name, "table") == 0 || strcmp(name, "tr") == 0 || strcmp(name, "td") == 0 || strcmp(name, "th") == 0;
}

void ChapterHtmlSlimParser::applyDirectionToEntry(StyleStackEntry& entry, const CssStyle& css) {
  if (css.hasDirection()) {
    entry.hasDirection = true;
    entry.direction = css.direction;
  }
}

EpdFontFamily::Style ChapterHtmlSlimParser::fontStyleForTextDecoration(const CssTextDecoration decoration) {
  EpdFontFamily::Style style = EpdFontFamily::REGULAR;
  if ((decoration & CssTextDecoration::Underline) != CssTextDecoration::None) {
    style = static_cast<EpdFontFamily::Style>(style | EpdFontFamily::UNDERLINE);
  }
  if ((decoration & CssTextDecoration::LineThrough) != CssTextDecoration::None) {
    style = static_cast<EpdFontFamily::Style>(style | EpdFontFamily::STRIKETHROUGH);
  }
  return style;
}

void ChapterHtmlSlimParser::applyTextDecorationToEntry(StyleStackEntry& entry, const CssStyle& css) {
  if (css.hasTextDecoration()) {
    entry.hasTextDecoration = true;
    entry.textDecoration = css.textDecoration;
  }
}

void ChapterHtmlSlimParser::applyTextTransformToEntry(StyleStackEntry& entry, const CssStyle& css) {
  if (css.hasTextTransform()) {
    entry.hasTextTransform = true;
    entry.textTransform = css.textTransform();
  }
}

void ChapterHtmlSlimParser::applyVerticalAlignToEntry(StyleStackEntry& entry, const CssStyle& css) {
  if (!css.hasVerticalAlign()) return;
  if (css.verticalAlign == CssVerticalAlign::Super) {
    entry.hasSup = true;
    entry.sup = true;
  } else if (css.verticalAlign == CssVerticalAlign::Sub) {
    entry.hasSub = true;
    entry.sub = true;
  }
}

void ChapterHtmlSlimParser::pushTableTextStyleEntry(const CssStyle& cssStyle) {
  if (!cssStyle.hasFontWeight() && !cssStyle.hasFontStyle() && !cssStyle.hasTextDecoration() &&
      !cssStyle.hasDirection() && !cssStyle.hasTextAlign()) {
    return;
  }

  StyleStackEntry entry;
  entry.depth = depth;
  if (cssStyle.hasFontWeight()) {
    entry.hasBold = true;
    entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
  }
  if (cssStyle.hasFontStyle()) {
    entry.hasItalic = true;
    entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
  }
  applyTextDecorationToEntry(entry, cssStyle);
  applyDirectionToEntry(entry, cssStyle);
  entry.setsParagraphDirection = true;
  if (cssStyle.hasTextAlign()) {
    entry.hasTextAlign = true;
    entry.textAlign = cssStyle.textAlign;
  }
  inlineStyleStack.push_back(entry);
  updateEffectiveInlineStyle();
}

void ChapterHtmlSlimParser::pushDecorationStyleEntry(const CssTextDecoration defaultDecoration,
                                                     const CssStyle& cssStyle) {
  StyleStackEntry entry;
  entry.depth = depth;
  entry.hasTextDecoration = true;
  entry.textDecoration = cssStyle.hasTextDecoration() ? cssStyle.textDecoration : defaultDecoration;
  if (cssStyle.hasFontWeight()) {
    entry.hasBold = true;
    entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
  }
  if (cssStyle.hasFontStyle()) {
    entry.hasItalic = true;
    entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
  }
  applyDirectionToEntry(entry, cssStyle);
  applyTextTransformToEntry(entry, cssStyle);
  inlineStyleStack.push_back(entry);
  updateEffectiveInlineStyle();
}

// Update effective bold/italic/decorations based on block style and inline style stack
void ChapterHtmlSlimParser::updateEffectiveInlineStyle() {
  // Start with block-level styles
  effectiveBold = currentCssStyle.hasFontWeight() && currentCssStyle.fontWeight == CssFontWeight::Bold;
  effectiveItalic = currentCssStyle.hasFontStyle() && currentCssStyle.fontStyle == CssFontStyle::Italic;
  effectiveTextDecoration =
      currentCssStyle.hasTextDecoration() ? currentCssStyle.textDecoration : CssTextDecoration::None;
  bool paragraphDirectionDefined = false;
  bool paragraphIsRtl = false;
  if (!blockStyleStack.empty()) {
    const auto& blockStyle = blockStyleStack.back();
    paragraphDirectionDefined = blockStyle.directionDefined;
    paragraphIsRtl = blockStyle.isRtl;
  }
  effectiveDirectionDefined = paragraphDirectionDefined;
  effectiveDirection = paragraphIsRtl ? CssTextDirection::Rtl : CssTextDirection::Ltr;
  effectiveTextAlignDefined = currentCssStyle.hasTextAlign();
  effectiveTextAlign = currentCssStyle.textAlign;
  effectiveSup = false;
  effectiveSub = false;
  effectiveEmphasis = currentCssStyle.hasTextEmphasis() ? currentCssStyle.textEmphasis : CssTextEmphasis::None;
  effectiveSmallCaps = currentCssStyle.hasFontVariant() && currentCssStyle.fontVariant == CssFontVariant::SmallCaps;
  effectiveTextTransform = currentTextBlock ? currentTextBlock->getBlockStyle().textTransform : CssTextTransform::None;
  effectiveWordFontId = 0;

  // Apply inline style stack in order
  for (const auto& entry : inlineStyleStack) {
    if (entry.hasBold) {
      effectiveBold = entry.bold;
    }
    if (entry.hasItalic) {
      effectiveItalic = entry.italic;
    }
    // CSS line decorations propagate through descendants; child entries add
    // their own lines but cannot cancel an ancestor's already active line.
    if (entry.hasTextDecoration) {
      effectiveTextDecoration = effectiveTextDecoration | entry.textDecoration;
    }
    if (entry.hasDirection) {
      effectiveDirectionDefined = true;
      effectiveDirection = entry.direction;
      if (entry.setsParagraphDirection) {
        paragraphDirectionDefined = true;
        paragraphIsRtl = entry.direction == CssTextDirection::Rtl;
      }
    }
    if (entry.hasTextAlign) {
      effectiveTextAlignDefined = true;
      effectiveTextAlign = entry.textAlign;
    }
    if (entry.hasSup) {
      effectiveSup = entry.sup;
      if (entry.sup) effectiveSub = false;
    }
    if (entry.hasSub) {
      effectiveSub = entry.sub;
      if (entry.sub) effectiveSup = false;
    }
    if (entry.hasEmphasis) {
      effectiveEmphasis = entry.emphasis;
    }
    if (entry.hasSmallCaps) {
      effectiveSmallCaps = entry.smallCaps;
    }
    if (entry.hasTextTransform) {
      effectiveTextTransform = entry.textTransform;
    }
    if (entry.hasFontId) {
      effectiveWordFontId = entry.fontIdOverride;
    }
  }

  // Keep flow direction in the active empty text block. Inline direction remains
  // available for CSS inheritance without replacing the paragraph's base direction.
  if (currentTextBlock && currentTextBlock->isEmpty()) {
    auto& style = currentTextBlock->getBlockStyle();
    style.directionDefined = paragraphDirectionDefined;
    style.isRtl = paragraphIsRtl;
  }
}

void ChapterHtmlSlimParser::flushPendingAnchor() {
  if (pendingAnchorId.empty()) return;

  // If the pending anchor is a TOC chapter boundary, force a page break after the previous
  // block is flushed so the chapter starts on a fresh page.
  if (std::find(tocAnchors.begin(), tocAnchors.end(), pendingAnchorId) != tocAnchors.end()) {
    if (currentPage && !currentPage->elements.empty()) {
      maybeEmitOpenBoxForPageBreak();
      completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex, currentPageVisibleOffset);
      completedPageCount++;
      currentPage.reset(new Page());
      currentPageNextY = 0;
      currentPageVisibleOffsetSet = false;
    }
  }

  // Record deferred anchor after previous block is flushed (and any TOC page break)
  anchorData.push_back({std::move(pendingAnchorId), static_cast<uint16_t>(completedPageCount)});
  pendingAnchorId.clear();
}

void ChapterHtmlSlimParser::setCurrentPageVisibleOffset(const uint32_t offset) {
  if (currentPageVisibleOffsetSet) return;
  // The first page always begins at the start of the body, even when the XHTML
  // contains leading formatting whitespace before its first rendered word.
  currentPageVisibleOffset = completedPageCount == 0 ? 0 : offset;
  currentPageVisibleOffsetSet = true;
}

// French verb-subject inversion (literary narration register): "songeai-je", "pense-t-il",
// "dit-elle". The verb and the clitic subject pronoun are each meaningful dictionary headwords
// on their own, unlike a genuine hyphenated compound (kFrenchInversionExceptions below), so
// flushPartWordBuffer() below splits the buffered word into extra addWord() tokens glued
// together with attachToPrevious -- the same "extra token, no gap" technique the U+00A0 handling
// above uses to keep a non-breaking pair visually joined while giving each half its own token.
constexpr const char* kFrenchInversionPronouns[] = {"je", "tu", "il", "elle", "on", "nous", "vous", "ils", "elles"};

// Nouns that are historically verb-pronoun inversions but are lexicalized as a single headword
// today ("un rendez-vous", "le qu'en-dira-t-on"), so the whole word must stay one token.
constexpr const char* kFrenchInversionExceptions[] = {"rendez-vous", "qu'en-dira-t-on"};

static bool asciiEqualsCi(const char* a, const int aLen, const char* b) {
  if (static_cast<size_t>(aLen) != strlen(b)) return false;
  for (int i = 0; i < aLen; i++) {
    if ((a[i] | 0x20) != (b[i] | 0x20)) return false;
  }
  return true;
}

static bool asciiEndsWithCi(const char* word, const int wordLen, const char* suffix) {
  const int suffixLen = static_cast<int>(strlen(suffix));
  if (wordLen < suffixLen) return false;
  return asciiEqualsCi(word + (wordLen - suffixLen), suffixLen, suffix);
}

// Mirrors Dictionary::cleanWord()'s trailing trim (a byte is "word" if it's ASCII alnum or any
// non-ASCII byte, with the 3-byte E2 80/81 xx General Punctuation sequences -- curly quotes,
// dashes -- stripped as a unit) so "songeai-je," and "pense-t-il?" match the same as the bare
// forms. Only shrinks the length used for matching: the buffer itself is untouched, so the
// trimmed punctuation still renders as part of the pronoun's token in flushPartWordBuffer().
static int trimmedLengthForFrenchInversionMatch(const char* word, int wordLen) {
  const auto* bytes = reinterpret_cast<const unsigned char*>(word);
  while (wordLen > 0) {
    if (std::isalnum(bytes[wordLen - 1]) || bytes[wordLen - 1] >= 0x80) {
      if (wordLen >= 3 && bytes[wordLen - 3] == 0xE2 && (bytes[wordLen - 2] == 0x80 || bytes[wordLen - 2] == 0x81)) {
        wordLen -= 3;
        continue;
      }
      break;
    }
    wordLen--;
  }
  return wordLen;
}

// On a match, word[0, verbLen) is the verb and word[verbLen, verbLen + connectorLen) is the
// literal "-" or "-t-" connector; the pronoun runs from there to the end of `word` (including
// any trailing punctuation ignored for matching). Returns false when `word`, ignoring trailing
// punctuation, does not end in a hyphenated subject pronoun, or is one of
// kFrenchInversionExceptions.
static bool findFrenchInversionSplit(const char* word, const int wordLen, int& verbLen, int& connectorLen) {
  if (!memchr(word, '-', static_cast<size_t>(wordLen))) return false;
  const int matchLen = trimmedLengthForFrenchInversionMatch(word, wordLen);
  for (const char* exception : kFrenchInversionExceptions) {
    if (asciiEqualsCi(word, matchLen, exception)) return false;
  }
  for (const char* pronoun : kFrenchInversionPronouns) {
    if (!asciiEndsWithCi(word, matchLen, pronoun)) continue;
    const int pronounLen = static_cast<int>(strlen(pronoun));
    const int hyphenIndex = matchLen - pronounLen - 1;
    if (hyphenIndex <= 0 || word[hyphenIndex] != '-') continue;
    // Euphonic "-t-", inserted only before il/elle/on to avoid a vowel hiatus: "pense-t-il".
    const bool takesEuphonicT =
        strcmp(pronoun, "il") == 0 || strcmp(pronoun, "elle") == 0 || strcmp(pronoun, "on") == 0;
    if (takesEuphonicT && hyphenIndex >= 2 && (word[hyphenIndex - 1] | 0x20) == 't' && word[hyphenIndex - 2] == '-' &&
        hyphenIndex - 2 > 0) {
      verbLen = hyphenIndex - 2;
      connectorLen = 3;
      return true;
    }
    verbLen = hyphenIndex;
    connectorLen = 1;
    return true;
  }
  return false;
}

// flush the contents of partWordBuffer to currentTextBlock
void ChapterHtmlSlimParser::flushPartWordBuffer() {
  if (!currentTextBlock) {
    partWordBufferIndex = 0;
    nextWordContinues = false;
    return;
  }

  // Determine font style from depth-based tracking and CSS effective style
  const bool isBold = boldUntilDepth < depth || effectiveBold;
  const bool isItalic = italicUntilDepth < depth || effectiveItalic;

  // Combine style flags using bitwise OR
  EpdFontFamily::Style fontStyle = EpdFontFamily::REGULAR;
  if (isBold) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::BOLD);
  }
  if (isItalic) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::ITALIC);
  }
  fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | fontStyleForTextDecoration(effectiveTextDecoration));
  if (effectiveSup) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::SUP);
  } else if (effectiveSub) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::SUB);
  }

  // flush the buffer
  partWordBuffer[partWordBufferIndex] = '\0';
  applyAsciiTextTransform(partWordBuffer, effectiveTextTransform, !nextWordContinues);
  if (effectiveSmallCaps) {
    smallCapsTransform(partWordBuffer);
  }
  // Per-word font from an inline font-size. Sup/sub already draw at 50% scale, so a size on
  // the same run would shrink twice -- the vertical shift wins. An override equal to the
  // line's own font is stored as 0 to keep the block on the no-override fast path.
  int32_t wordFontId = (effectiveSup || effectiveSub) ? 0 : effectiveWordFontId;
  if (wordFontId != 0 && wordFontId == currentTextBlock->getBlockStyle().resolveFontId(fontId)) {
    wordFontId = 0;
  }
  // Grid-table cell budgets, checked around the same addWord this fork passes wordFontId to.
  const size_t wordBytes = static_cast<size_t>(partWordBufferIndex);
  if (insideTableCell && !tableRowStacked && tableCellTextBytes + wordBytes > MAX_GRID_TABLE_CELL_BYTES) {
    fallbackTableRowToStacked();
  }
  uint8_t linkId = 0;
  if (insideFootnoteLink) {
    if (!currentTextBlock->linkTargetMatches(currentFootnoteLinkId, currentFootnote.href.c_str())) {
      currentFootnoteLinkId = currentTextBlock->addLinkTarget(currentFootnote.href.c_str());
    }
    linkId = currentFootnoteLinkId;
  }
  if (frenchBookCache < 0) {
    frenchBookCache =
        (epub && InflectionRules::languageFromCode(epub->getLanguage().c_str()) == InflectionRules::Language::French)
            ? 1
            : 0;
  }
  int frenchVerbLen = 0;
  int frenchConnectorLen = 0;
  if (frenchBookCache &&
      findFrenchInversionSplit(partWordBuffer, partWordBufferIndex, frenchVerbLen, frenchConnectorLen)) {
    const int pronounStart = frenchVerbLen + frenchConnectorLen;

    // The connector and pronoun are always plain ASCII, so only the verb needs counting.
    uint32_t verbVisibleLen = 0;
    const auto* verbPtr = reinterpret_cast<const unsigned char*>(partWordBuffer);
    const unsigned char* const verbEnd = verbPtr + frenchVerbLen;
    while (verbPtr < verbEnd) {
      utf8NextCodepoint(&verbPtr);
      verbVisibleLen++;
    }

    const char savedAtVerbEnd = partWordBuffer[frenchVerbLen];
    partWordBuffer[frenchVerbLen] = '\0';
    currentTextBlock->addWord(partWordBuffer, fontStyle, false, nextWordContinues, wordFontId, partWordVisibleOffset,
                              linkId);
    partWordBuffer[frenchVerbLen] = savedAtVerbEnd;

    const char savedAtPronounStart = partWordBuffer[pronounStart];
    partWordBuffer[pronounStart] = '\0';
    currentTextBlock->addWord(partWordBuffer + frenchVerbLen, fontStyle, false, true, wordFontId,
                              partWordVisibleOffset + verbVisibleLen, linkId);
    partWordBuffer[pronounStart] = savedAtPronounStart;

    currentTextBlock->addWord(partWordBuffer + pronounStart, fontStyle, false, true, wordFontId,
                              partWordVisibleOffset + verbVisibleLen + static_cast<uint32_t>(frenchConnectorLen),
                              linkId);
  } else {
    currentTextBlock->addWord(partWordBuffer, fontStyle, false, nextWordContinues, wordFontId, partWordVisibleOffset,
                              linkId);
  }
  if (insideTableCell && !tableRowStacked) {
    tableCellTextBytes += wordBytes;
    if (currentTextBlock->size() > MAX_GRID_TABLE_CELL_WORDS) {
      fallbackTableRowToStacked();
    }
  }
  if (effectiveEmphasis != CssTextEmphasis::None) {
    if (const char* mark = emphasisMarkUtf8(effectiveEmphasis)) {
      // Synthetic per-glyph ruby: one mark per codepoint, each followed by an ideographic space.
      //
      // The spacer is what makes the marks line up. Ruby draws in SUP style, which drawText
      // renders at 50% scale, and TextBlock::render centres the whole ruby string over the whole
      // word. Bare marks therefore advance only N/2 character widths and bunch into the middle
      // third of the run they are meant to mark -- observed on device as a dense row of marks
      // roughly a third the width of the emphasised text, aligned with none of it. U+3000 is
      // full-width, so at SUP it is another 0.5 character: mark + space = exactly one character
      // cell, making the run the same width as the text and putting one mark per character.
      //
      // U+3000 shares its block (CJK Symbols and Punctuation) with the brackets and kutouten used
      // on every page, so it is present wherever the marks themselves are.
      //
      // A real <rt> annotation following this word simply overwrites the marks - furigana wins
      // over bouten. Marks render through the ruby path, so they follow the furigana toggle.
      int cps = 0;
      for (int i = 0; partWordBuffer[i] != '\0'; i++) {
        if ((static_cast<unsigned char>(partWordBuffer[i]) & 0xC0) != 0x80) cps++;
      }
      if (cps > 0 && cps <= 24) {
        static constexpr char IDEOGRAPHIC_SPACE[] = "\xE3\x80\x80";  // U+3000
        std::string marks;
        const size_t markLen = strlen(mark);
        marks.reserve((markLen + sizeof(IDEOGRAPHIC_SPACE) - 1) * cps);
        for (int i = 0; i < cps; i++) {
          marks.append(mark, markLen);
          marks.append(IDEOGRAPHIC_SPACE, sizeof(IDEOGRAPHIC_SPACE) - 1);
        }
        currentTextBlock->setLastWordRuby(marks);
      }
    }
  }
  partWordBufferIndex = 0;
  nextWordContinues = false;
  listItemBulletOnly = false;
}

// start a new text block if needed
// Lay out the pending text block NOW (block layout is normally deferred until the next block
// starts) so currentPageNextY reflects everything before/inside a box boundary.
void ChapterHtmlSlimParser::flushPendingBlockLayout() {
  if (!currentTextBlock || currentTextBlock->isEmpty()) return;
  makePages();
  const auto style = currentTextBlock->getBlockStyle();
  currentTextBlock.reset(new (std::nothrow)
                             ParsedText(extraParagraphSpacing, hyphenationEnabled, focusReadingEnabled, style));
  wordsExtractedInBlock = 0;
}

// Black panel behind ONE line of a CssInkMode::Inverted block, pushed before the line itself so
// the white glyphs land on top of it.
//
// The rect is the block's PADDING box, not the glyph run: a heading that asks for
// `background-color` + `padding` wants a bar spanning the column, and the padding is already
// reserved in the layout whether or not it is painted. Horizontally that is
// [marginLeft, viewportWidth - marginRight) -- margins stay outside the panel, padding inside it.
// Vertically each line covers exactly its own line height, so consecutive lines abut seamlessly
// and a block split across a page break simply stops and resumes.
void ChapterHtmlSlimParser::emitInvertedPanel(const BlockStyle& blockStyle, const int16_t lineHeight) {
  if (!currentPage) return;

  const int left = std::max(0, static_cast<int>(blockStyle.marginLeft));
  const int right = std::max(left + 1, viewportWidth - std::max(0, static_cast<int>(blockStyle.marginRight)));

  // Stitch the block's top padding onto its first panel line. Clamped at the page top: after a
  // page break the padding was consumed on the previous page and there is nothing to cover.
  const int top = std::max(0, currentPageNextY - pendingPanelTopPad);
  pendingPanelTopPad = 0;
  const int height = currentPageNextY - top + lineHeight;
  if (height <= 0) return;

  auto panel = std::shared_ptr<PageBox>(
      new (std::nothrow) PageBox(static_cast<int16_t>(right - left), static_cast<int16_t>(height),
                                 /*edges=*/0, static_cast<int16_t>(left), static_cast<int16_t>(top), /*filled=*/true));
  if (!panel) {
    LOG_ERR("EHP", "OOM: inverted panel box");
    return;
  }
  currentPage->elements.push_back(panel);
  lastPanelBox = std::move(panel);
}

// The ONE place a page boundary is made. Everything about CSS page breaks that could go wrong --
// a blank page, a block that never lands anywhere -- is prevented here: a page with no elements
// on it is never flushed, it is simply reused. A break therefore always leaves content behind and
// always starts the next block at y = 0, so pagination cannot fail to move forward.
void ChapterHtmlSlimParser::breakPage() {
  if (currentPage && !currentPage->elements.empty()) {
    maybeEmitOpenBoxForPageBreak();
    completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex, currentPageVisibleOffset);
    completedPageCount++;
    currentPage.reset(new (std::nothrow) Page());
    if (!currentPage) LOG_ERR("EHP", "OOM: page after break");
    currentPageVisibleOffsetSet = false;
    boxFirstElementIndex = 0;
  }
  // Pending top spacing dies with the boundary: a block that starts a page starts it at the top.
  currentPageNextY = 0;
}

// Start buffering this block's lines if it asks to be kept together (page-break-inside: avoid) or
// to keep the following block with it (page-break-after: avoid).
//
// Nothing is gained by buffering a block that already starts a page: moving it would move it to
// another empty page and it would be split at exactly the same line, so the (bounded) buffering
// work is skipped and the block is laid out normally.
void ChapterHtmlSlimParser::beginKeepTogether(const BlockStyle& blockStyle) {
  keepingBlockTogether = false;
  keepBufferHeight = 0;
  keepWithNextReserve = 0;
  keepBuffer.clear();

  if (!blockStyle.keepTogether() && !blockStyle.keepWithNext()) return;
  if (!currentPage || currentPage->elements.empty()) return;

  // One allocation for the parser's lifetime (64 pointers, ~512 bytes) instead of a realloc per
  // heading; the cap is enforced in addLineToPage, so it never grows past this.
  if (keepBuffer.capacity() < KEEP_MAX_LINES) keepBuffer.reserve(KEEP_MAX_LINES);
  keepingBlockTogether = true;
  if (blockStyle.keepWithNext()) {
    // Room for ONE line of the reader's own font, not the block's: the block asking to be kept
    // with the next one is a heading, and what has to fit after it is body text.
    keepWithNextReserve = static_cast<int16_t>(renderer.getLineHeight(fontId, lineCompression));
  }
}

// Place the buffered block, breaking the page first if it does not fit where it stands.
void ChapterHtmlSlimParser::finishKeepTogether() {
  if (!keepingBlockTogether) return;

  // The buffer is bounded by one viewport height, so the block always fits on a page BY ITSELF;
  // the only question is whether it fits on this one. `need` includes the keep-with-next reserve
  // when that still leaves the block placeable -- an ambition, dropped rather than obeyed if
  // honouring it would push the block off every page.
  int16_t need = static_cast<int16_t>(keepBufferHeight + keepWithNextReserve);
  if (need > viewportHeight) need = keepBufferHeight;
  if (currentPage && !currentPage->elements.empty() && currentPageNextY + need > viewportHeight) {
    breakPage();
  }
  flushKeepBuffer();
}

// Hand the buffered lines to the normal placement path. Called both when the decision has been
// made (finishKeepTogether) and when the block turns out to be too tall to keep together at all
// (addLineToPage) -- in the second case the lines placed so far must still reach the page.
void ChapterHtmlSlimParser::flushKeepBuffer() {
  keepingBlockTogether = false;  // cleared FIRST: addLineToPage must not re-buffer the replay
  keepWithNextReserve = 0;
  keepBufferHeight = 0;
  for (auto& buffered : keepBuffer) {
    addLineToPage(std::move(buffered.first), buffered.second);
  }
  keepBuffer.clear();
}

void ChapterHtmlSlimParser::emitBoxRect(const bool openBottom) {
  if (!currentPage || boxDepth < 0 || boxAwaitingFirstLine) return;  // no box content on this page yet
  const int lineHeight = static_cast<int>(renderer.getLineHeight(fontId) * lineCompression);
  const auto pad = static_cast<int16_t>(std::max(2, lineHeight / 3));
  const auto yTop = boxContinued ? static_cast<int16_t>(0) : static_cast<int16_t>(std::max(0, boxStartY - pad));
  // Closing edge: currentPageNextY at close already includes the last block's bottom spacing
  // (margin/padding + extra paragraph spacing), so the line must be pulled back UP toward the
  // text ink rather than padded further down (device feedback, three rounds).
  const int lineHeight2 = static_cast<int>(renderer.getLineHeight(fontId) * lineCompression);
  const auto yBottom = openBottom
                           ? static_cast<int16_t>(viewportHeight - 1)
                           : static_cast<int16_t>(std::min<int>(
                                 viewportHeight - 1, std::max<int>(yTop + 1, currentPageNextY - lineHeight2 / 3)));
  if (yBottom <= yTop) return;
  uint8_t edges = CssStyle::edgeMaskOf(boxBorderSpec);
  if (boxContinued) edges &= static_cast<uint8_t>(~CssStyle::BORDER_TOP);
  if (openBottom) edges &= static_cast<uint8_t>(~CssStyle::BORDER_BOTTOM);

  int16_t x = 2;
  int16_t width = static_cast<int16_t>(viewportWidth - 5);
  if (boxShrinkToContent) {
    int contentLeft = viewportWidth;
    int contentRight = 0;
    for (size_t i = boxFirstElementIndex; i < currentPage->elements.size(); ++i) {
      const auto& element = currentPage->elements[i];
      if (element->getTag() != TAG_PageLine) continue;
      const auto line = std::static_pointer_cast<PageLine>(element);
      const auto& text = line->getBlock();
      const int lineFontId = text->getBlockStyle().resolveFontId(fontId);
      int rightmostX = 0;
      uint16_t rightmostWord = 0;
      for (uint16_t word = 0; word < text->wordCount(); ++word) {
        const int wordX = line->xPos + text->wordXpos(word);
        contentLeft = std::min(contentLeft, wordX);
        if (wordX >= rightmostX) {
          rightmostX = wordX;
          rightmostWord = word;
        }
      }
      if (text->wordCount() > 0) {
        // x positions already encode every earlier word's advance. Measure only the final
        // visual word instead of walking the whole heading through the small SD-font cache.
        const int32_t wf = text->wordFont(rightmostWord);
        const int wordWidth =
            renderer.getTextAdvanceX(wf != 0 ? wf : lineFontId, text->wordText(rightmostWord),
                                     text->wordStyle(rightmostWord), text->getBlockStyle().letterSpacing);
        contentRight = std::max(contentRight, rightmostX + wordWidth);
      }
    }
    if (contentRight > contentLeft) {
      x = static_cast<int16_t>(std::max(2, contentLeft));
      width = static_cast<int16_t>(std::min<int>(viewportWidth - 3, contentRight + 2) - x);
    }
  }

  const uint8_t borderSpec =
      CssStyle::makeBorderSpec(edges, CssStyle::lineStyleOf(boxBorderSpec), CssStyle::lineWidthOf(boxBorderSpec));
  auto box = std::shared_ptr<PageBox>(new (std::nothrow)
                                          PageBox(width, static_cast<int16_t>(yBottom - yTop), borderSpec, x, yTop));
  if (box) currentPage->elements.push_back(box);
}

void ChapterHtmlSlimParser::maybeEmitOpenBoxForPageBreak() {
  if (boxDepth < 0) return;
  emitBoxRect(/*openBottom=*/true);
  boxContinued = true;
}

void ChapterHtmlSlimParser::closeBoxBlock() {
  flushPendingBlockLayout();
  if (boxAwaitingFirstLine) {
    if (!currentPage) currentPage.reset(new (std::nothrow) Page());
    if (currentPage) {
      boxStartY = currentPageNextY;
      currentPageNextY =
          static_cast<int16_t>(std::min<int>(viewportHeight, currentPageNextY + std::max<int16_t>(1, boxEmptyAdvance)));
      boxAwaitingFirstLine = false;
    }
  }
  emitBoxRect(/*openBottom=*/false);
  // Full boxes need a little clearance after their closing edge. Partial publisher rules already
  // carry their own margin/padding; adding the old box clearance here doubled the space after
  // headings, quotes and worksheet lines.
  if (CssStyle::edgeMaskOf(boxBorderSpec) == CssStyle::BORDER_ALL) {
    const int lineHeight = static_cast<int>(renderer.getLineHeight(fontId) * lineCompression);
    currentPageNextY =
        static_cast<int16_t>(currentPageNextY + std::max(2, lineHeight / 12) + std::max(4, lineHeight / 2));
  }
  boxDepth = -1;
  boxBorderSpec = 0;
  boxEmptyAdvance = 0;
  boxFirstElementIndex = 0;
  boxShrinkToContent = false;
  boxContinued = false;
  boxAwaitingFirstLine = false;
}

void ChapterHtmlSlimParser::startNewTextBlock(const BlockStyle& blockStyle) {
  nextWordContinues = false;  // New block = new paragraph, no continuation
  if (currentTextBlock) {
    // already have a text block running and it is empty - just reuse it
    if (currentTextBlock->isEmpty()) {
      // The stack accumulates horizontal margins and text properties from ancestors.
      // Vertical margins are per-element and not inherited through the stack, but
      // container elements deposit their vertical margins on the empty block when they
      // open. Merge those into the new style so the first child in a container inherits
      // the container's vertical spacing.
      const auto style = currentTextBlock->getBlockStyle();
      BlockStyle incoming = blockStyle;
      if (style.fromBrElement) {
        // The empty block was created by a <br> section separator. Inject a full line of
        // blank space before the following paragraph so the scene/section break is visible.
        // This only fires when the <br> block stayed empty (i.e. no inline text was added).
        const int16_t lineHeight = static_cast<int16_t>(renderer.getLineHeight(fontId, lineCompression));
        incoming.marginTop = static_cast<int16_t>(incoming.marginTop + lineHeight);
      }

      currentTextBlock->setBlockStyle(style.getCombinedBlockStyle(incoming, BlockStyle::CombineAxis::Vertical));

      flushPendingAnchor();
      updateEffectiveInlineStyle();
      return;
    }

    // <li> added a bullet as the first word, making the block non-empty. When a nested
    // block-level child (<p>, <div>, etc.) opens, reuse the block instead of flushing
    // the bullet to its own line. The bullet stays inline with the child's text.
    if (listItemBulletOnly) {
      const auto style = currentTextBlock->getBlockStyle();
      currentTextBlock->setBlockStyle(style.getCombinedBlockStyle(blockStyle, BlockStyle::CombineAxis::Vertical));
      listItemBulletOnly = false;
      flushPendingAnchor();
      updateEffectiveInlineStyle();
      return;
    }

    makePages();
  }
  // If the pending anchor is a TOC chapter boundary, force a page break after the previous
  // block is flushed so the chapter starts on a fresh page.
  flushPendingAnchor();
  currentTextBlock.reset(new ParsedText(extraParagraphSpacing, hyphenationEnabled, focusReadingEnabled, blockStyle));
  wordsExtractedInBlock = 0;
  listItemBulletOnly = false;
  updateEffectiveInlineStyle();
}

void ChapterHtmlSlimParser::emitHorizontalRule(const BlockStyle& blockStyle) {
  if (partWordBufferIndex > 0) {
    flushPartWordBuffer();
  }

  if (currentTextBlock) {
    const BlockStyle parentBlockStyle = currentTextBlock->getBlockStyle();
    startNewTextBlock(parentBlockStyle);
  }

  if (!currentPage) {
    currentPage.reset(new (std::nothrow) Page());
    if (!currentPage) {
      LOG_ERR("EHP", "Failed to create page for horizontal rule");
      return;
    }
    currentPageNextY = 0;
  }

  const int16_t lineHeight = static_cast<int16_t>(renderer.getLineHeight(fontId, lineCompression));
  const int16_t defaultVerticalSpacing = static_cast<int16_t>(lineHeight / 2);
  const int16_t topSpacing =
      static_cast<int16_t>((blockStyle.marginTop > 0 ? blockStyle.marginTop : defaultVerticalSpacing) +
                           (blockStyle.paddingTop > 0 ? blockStyle.paddingTop : 0));
  const int16_t bottomSpacing =
      static_cast<int16_t>((blockStyle.marginBottom > 0 ? blockStyle.marginBottom : defaultVerticalSpacing) +
                           (blockStyle.paddingBottom > 0 ? blockStyle.paddingBottom : 0));
  constexpr uint8_t ruleThickness = 2;
  const int16_t availableWidth =
      std::max<int16_t>(1, static_cast<int16_t>(viewportWidth - blockStyle.totalHorizontalInset()));
  const int16_t width = std::max<int16_t>(1, static_cast<int16_t>(availableWidth / 4));
  const int16_t xPos = static_cast<int16_t>(blockStyle.leftInset() + ((availableWidth - width) / 2));
  const int16_t totalHeight = static_cast<int16_t>(topSpacing + ruleThickness + bottomSpacing);

  if (!currentPage->elements.empty() && currentPageNextY + totalHeight > viewportHeight) {
    maybeEmitOpenBoxForPageBreak();
    setCurrentPageVisibleOffset(visibleTextOffset);
    completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex, currentPageVisibleOffset);
    completedPageCount++;
    currentPage.reset(new (std::nothrow) Page());
    if (!currentPage) {
      LOG_ERR("EHP", "Failed to create page after horizontal-rule page break");
      return;
    }
    currentPageNextY = 0;
    currentPageVisibleOffsetSet = false;
  }

  currentPageNextY += topSpacing;

  auto pageRule = std::shared_ptr<PageHorizontalRule>(
      new (std::nothrow) PageHorizontalRule(width, ruleThickness, xPos, currentPageNextY));
  if (!pageRule) {
    LOG_ERR("EHP", "Failed to create PageHorizontalRule");
    return;
  }
  currentPage->elements.push_back(pageRule);
  setCurrentPageVisibleOffset(visibleTextOffset);
  currentPageNextY = static_cast<int16_t>(currentPageNextY + ruleThickness + bottomSpacing);

  if (!pendingAnchorId.empty()) {
    anchorData.push_back({std::move(pendingAnchorId), static_cast<uint16_t>(completedPageCount)});
    pendingAnchorId.clear();
  }
}

void ChapterHtmlSlimParser::fallbackTableRowToStacked() {
  if (tableRowStacked) {
    return;
  }

  auto activeCell = std::move(currentTextBlock);
  tableRowStacked = true;

  for (auto& cell : tableRowCells) {
    currentTextBlock = std::move(cell);
    wordsExtractedInBlock = 0;
    if (currentTextBlock && !currentTextBlock->isEmpty()) {
      makePages();
    }
  }
  tableRowCells.clear();
  currentTextBlock = std::move(activeCell);
  wordsExtractedInBlock = 0;
}

void ChapterHtmlSlimParser::closeTableCell() {
  if (!insideTableCell) {
    return;
  }
  insideTableCell = false;

  if (!currentTextBlock) {
    return;
  }

  if (!tableRowStacked &&
      (tableRowCells.size() >= MAX_GRID_TABLE_COLUMNS || currentTextBlock->size() > MAX_GRID_TABLE_CELL_WORDS)) {
    fallbackTableRowToStacked();
  }

  if (tableRowStacked) {
    wordsExtractedInBlock = 0;
    if (!currentTextBlock->isEmpty()) {
      makePages();
    }
    currentTextBlock.reset();
    return;
  }

  tableRowCells.push_back(std::move(currentTextBlock));
}

void ChapterHtmlSlimParser::addTableRowSeparator() {
  if (!currentPage || currentPage->elements.empty() || viewportWidth == 0 ||
      currentPageNextY + TABLE_ROW_SEPARATOR_GAP > viewportHeight) {
    return;
  }

  auto separator = std::shared_ptr<PageHorizontalRule>(
      new (std::nothrow) PageHorizontalRule(viewportWidth, TABLE_ROW_SEPARATOR_THICKNESS, 0, currentPageNextY + 1));
  if (!separator) {
    LOG_ERR("EHP", "OOM: table row separator");
    return;
  }
  if (currentPage->elements.capacity() == currentPage->elements.size()) {
    currentPage->elements.reserve(currentPage->elements.size() + 1);
  }
  currentPage->elements.push_back(std::move(separator));
  currentPageNextY += TABLE_ROW_SEPARATOR_GAP;
}

void ChapterHtmlSlimParser::finishTableRow() {
  closeTableCell();

  if (tableRowCells.empty()) {
    if (tableRowStacked) {
      addTableRowSeparator();
    }
    tableRowStacked = false;
    return;
  }

  const int16_t lineHeight =
      std::max<int16_t>(1, static_cast<int16_t>(renderer.getLineHeight(fontId) * lineCompression));
  const size_t columnCount = tableRowCells.size();
  const uint16_t cellWidth = static_cast<uint16_t>(viewportWidth / columnCount);

  // Keep enough width for a few glyphs while allowing ordinary three-column
  // tables to remain tabular at the default font size in portrait.
  if (columnCount < 2 || cellWidth <= TABLE_CELL_HORIZONTAL_PADDING * 2 ||
      cellWidth < lineHeight * TABLE_MIN_CELL_WIDTH_LINE_HEIGHTS) {
    fallbackTableRowToStacked();
    addTableRowSeparator();
    tableRowStacked = false;
    return;
  }

  const uint16_t textWidth = static_cast<uint16_t>(cellWidth - TABLE_CELL_HORIZONTAL_PADDING * 2);
  for (auto& lines : tableCellLines) {
    lines.clear();
  }
  tableLineVisibleOffsets.clear();
  if (tableLineVisibleOffsets.capacity() < MAX_GRID_TABLE_CELL_WORDS * 2) {
    tableLineVisibleOffsets.reserve(MAX_GRID_TABLE_CELL_WORDS * 2);
  }
  size_t maxLineCount = 0;
  const bool rowRtl = tableRowRtl;

  for (size_t column = 0; column < columnCount; ++column) {
    auto& lines = tableCellLines[column];
    // Two wrapped lines per buffered word avoids normal vector growth (max 64).
    if (lines.capacity() < MAX_GRID_TABLE_CELL_WORDS * 2) {
      lines.reserve(MAX_GRID_TABLE_CELL_WORDS * 2);
    }
    tableRowCells[column]->layoutAndExtractLines(
        renderer, fontId, textWidth, [this, &lines](const std::shared_ptr<TextBlock>& line, const uint32_t offset) {
          const size_t lineIndex = lines.size();
          lines.push_back(line);
          if (tableLineVisibleOffsets.size() <= lineIndex) {
            tableLineVisibleOffsets.resize(lineIndex + 1, UINT32_MAX);
          }
          tableLineVisibleOffsets[lineIndex] = std::min(tableLineVisibleOffsets[lineIndex], offset);
        });
    maxLineCount = std::max(maxLineCount, lines.size());
  }
  tableRowCells.clear();
  const auto clearLayoutLines = [this]() {
    for (auto& lines : tableCellLines) {
      lines.clear();
    }
    tableLineVisibleOffsets.clear();
  };

  for (size_t lineIndex = 0; lineIndex < maxLineCount; ++lineIndex) {
    const uint32_t lineVisibleOffset =
        lineIndex < tableLineVisibleOffsets.size() ? tableLineVisibleOffsets[lineIndex] : visibleTextOffset;
    int16_t rowLineHeight = lineHeight;
    for (size_t column = 0; column < columnCount; ++column) {
      if (lineIndex < tableCellLines[column].size()) {
        rowLineHeight = std::max<int16_t>(
            rowLineHeight, static_cast<int16_t>(lineHeight + tableCellLines[column][lineIndex]->getRubyShift(
                                                                 renderer.getFontAscenderSize(fontId))));
      }
    }

    const bool pageFull =
        currentPage && !currentPage->elements.empty() && currentPageNextY + rowLineHeight > viewportHeight;
    if (!currentPage || pageFull) {
      if (pageFull) {
        setCurrentPageVisibleOffset(lineVisibleOffset);
        completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex, currentPageVisibleOffset);
        completedPageCount++;
      }
      currentPage = makeUniqueNoThrow<Page>();
      if (!currentPage) {
        LOG_ERR("EHP", "OOM: page for table row");
        clearLayoutLines();
        return;
      }
      currentPageNextY = 0;
      currentPageVisibleOffsetSet = false;
    }

    const int16_t rowY = currentPageNextY;
    const size_t requiredCapacity = currentPage->elements.size() + columnCount;
    if (currentPage->elements.capacity() < requiredCapacity) {
      const size_t linesThatFit =
          std::max<size_t>(1, static_cast<size_t>((viewportHeight - currentPageNextY) / rowLineHeight));
      const size_t linesToReserve = std::min(maxLineCount - lineIndex, linesThatFit);
      currentPage->elements.reserve(currentPage->elements.size() + linesToReserve * columnCount + 1);
    }
    for (size_t column = 0; column < columnCount; ++column) {
      if (lineIndex >= tableCellLines[column].size()) {
        continue;
      }

      auto& line = tableCellLines[column][lineIndex];
      auto style = line->getBlockStyle();
      const size_t physicalColumn = rowRtl ? columnCount - column - 1 : column;
      style.marginLeft = static_cast<int16_t>(physicalColumn * cellWidth + TABLE_CELL_HORIZONTAL_PADDING);
      style.paddingLeft = 0;
      line->setBlockStyle(style);

      // Reset Y so every cell in this slice shares one baseline.
      currentPageNextY = rowY;
      addLineToPage(line, lineVisibleOffset);
    }
    currentPageNextY = static_cast<int16_t>(rowY + rowLineHeight);
  }

  addTableRowSeparator();
  tableRowStacked = false;
  clearLayoutLines();
}

void XMLCALL ChapterHtmlSlimParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);
  if (strcasecmp(name, "body") == 0) {
    // Case-insensitive to match ParagraphStreamer's tag matching (ProgressMapper). A case
    // mismatch here would leave visibleTextOffset at 0 for the whole section, so every page
    // would record offset 0 while the sync resolver still counts a non-zero offset.
    self->insideBody = true;
  }
  if (self->insideBody && (self->nonVisibleTextDepth > 0 || isNonVisibleTextTag(name))) {
    self->nonVisibleTextDepth++;
  }

  // Open this element on the CSS ancestor chain FIRST, before any of the early returns below:
  // every start tag must push exactly once so endElement's single pop stays paired with it.
  // Elements inside a skipped subtree are pushed too -- they style nothing, but they are real
  // ancestors of the elements that follow.
  self->cssPath.push(name);

  // Middle of skip
  if (self->skipUntilDepth < self->depth) {
    self->depth += 1;
    return;
  }

  if (strcmp(name, "p") == 0) {
    self->xpathParagraphIndex++;
  }
  if (strcmp(name, "li") == 0) {
    self->xpathListItemIndex++;
  }

  // Extract class, style, id, and dir attributes for CSS/RTL processing
  std::string classAttr;
  std::string styleAttr;
  std::string dirAttr;
  if (atts != nullptr) {
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "class") == 0) {
        classAttr = atts[i + 1];
      } else if (strcmp(atts[i], "style") == 0) {
        styleAttr = atts[i + 1];
      } else if (strcmp(atts[i], "id") == 0) {
        // Defer both anchor recording and TOC page breaks until startNewTextBlock,
        // after the previous block is flushed to pages via makePages().
        //
        // Skip IDs on non-navigable inline elements (e.g. <span>): these are never
        // link targets in epub content, but reading-system converters can inject tens
        // of thousands of them per chapter, exhausting the heap. TOC anchors are
        // always recorded regardless of element type, since they drive page breaks.
        const char* idValue = atts[i + 1];
        const bool isTocAnchor =
            std::find(self->tocAnchors.begin(), self->tocAnchors.end(), idValue) != self->tocAnchors.end();
        if (isTocAnchor || (!isNonNavigableInlineElement(name) && self->anchorData.size() < MAX_ANCHORS_PER_CHAPTER)) {
          // Flush a displaced anchor before overwriting. Consecutive non-block elements
          // (e.g. <aside id="fn1">text</aside><aside id="fn2">) with no intervening block
          // never trigger startNewTextBlock, so fn1 gets silently overwritten. That leaves
          // fn1 missing from the anchor map -> getPageForAnchor returns nullopt -> reader
          // lands at page 0 (section start) instead of the footnote.
          if (!self->pendingAnchorId.empty()) {
            self->flushPendingAnchor();
          }
          self->pendingAnchorId = idValue;
        }
      } else if (strcmp(atts[i], "dir") == 0) {
        dirAttr = atts[i + 1];
      }
    }
  }

  // The chain entry for this element was pushed with its tag only; its classes are known
  // now, and a descendant selector needs them for the elements nested inside it.
  self->cssPath.setTopClasses(classAttr);

  auto centeredBlockStyle = BlockStyle();
  centeredBlockStyle.textAlignDefined = true;
  centeredBlockStyle.alignment = CssTextAlign::Center;

  // Compute CSS style for this element early so display:none can short-circuit
  // before tag-specific branches emit any content or metadata.
  CssStyle cssStyle;
  if (self->cssParser) {
    cssStyle = self->cssParser->resolveStyle(name, classAttr, &self->cssPath);
    if (!styleAttr.empty()) {
      CssStyle inlineStyle = CssParser::parseInlineStyle(styleAttr);
      cssStyle.applyOver(inlineStyle);
    }
  }

  // A font-size on <html>/<body> is the book restating the size its body text should have --
  // and that base is the READER's font here, because every font-size resolves against it
  // (cssBlockFontId, ReaderFontScale.cpp) rather than against a fixed 16px. Applying a root
  // declaration on top of a base it is itself describing multiplies the user's setting by the
  // publisher's default: `body { font-size: 0.8em }` drags an entire book a ladder step (or two,
  // since ties snap down) below the size they chose, and no declaration further in ever brings
  // it back -- <p> inherits the root font through the block/inline style stack. Dropping it here
  // costs nothing the book can express elsewhere: every nested font-size still applies, so
  // headings, captions and small print keep their intended relationship to the body text.
  if (strcasecmp(name, "body") == 0 || strcasecmp(name, "html") == 0) {
    cssStyle.defined.fontSize = 0;
  }

  // HTML dir attribute overrides CSS direction (case-insensitive per HTML spec)
  if (!dirAttr.empty()) {
    if (strcasecmp(dirAttr.c_str(), "rtl") == 0) {
      cssStyle.direction = CssTextDirection::Rtl;
      cssStyle.defined.direction = 1;
    } else if (strcasecmp(dirAttr.c_str(), "ltr") == 0) {
      cssStyle.direction = CssTextDirection::Ltr;
      cssStyle.defined.direction = 1;
    }
  }

  // Direction is inherited in HTML/CSS. If this element does not define one, carry
  // the currently active inherited direction into its computed style.
  if (!cssStyle.hasDirection() && self->effectiveDirectionDefined) {
    cssStyle.direction = self->effectiveDirection;
    cssStyle.defined.direction = 1;
  }

  // Skip elements with display:none before all fast paths (tables, links, etc.).
  if (cssStyle.hasDisplay() && cssStyle.display == CssDisplay::None) {
    self->skipUntilDepth = self->depth;
    self->depth += 1;
    return;
  }

  // Buffer one simple row; oversized rows fall back to full-width flow.
  if (strcmp(name, "table") == 0) {
    // Flatten nested content without allocating a recursive row buffer.
    if (self->tableDepth > 0) {
      if (self->tableDepth == 1 && self->insideTableCell && self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }
      self->nextWordContinues = false;
      self->tableDepth += 1;
      self->depth += 1;
      return;
    }

    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
    }
    if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
      self->makePages();
      self->currentTextBlock.reset();
    }
    self->flushPendingAnchor();
    self->pushTableTextStyleEntry(cssStyle);
    self->tableDepth = 1;
    self->insideTableCell = false;
    self->tableRowStacked = false;
    self->tableRowRtl = cssStyle.hasDirection() && cssStyle.direction == CssTextDirection::Rtl;
    self->tableRowsSpannedRemaining = 0;
    self->tableCellTextBytes = 0;
    self->tableRowCells.clear();
    self->tableRowCells.reserve(MAX_GRID_TABLE_COLUMNS);
    self->depth += 1;
    return;
  }

  if (self->tableDepth == 1 && strcmp(name, "tr") == 0) {
    self->finishTableRow();
    if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
      // Text before the first row is typically a <caption>.
      self->makePages();
    }
    self->currentTextBlock.reset();
    self->tableRowStacked = self->tableRowsSpannedRemaining > 0;
    self->tableRowRtl = cssStyle.hasDirection() && cssStyle.direction == CssTextDirection::Rtl;
    if (self->tableRowsSpannedRemaining != UINT16_MAX && self->tableRowsSpannedRemaining > 0) {
      self->tableRowsSpannedRemaining--;
    }
    self->pushTableTextStyleEntry(cssStyle);
    self->depth += 1;
    return;
  }

  if (self->tableDepth == 1 && (strcmp(name, "td") == 0 || strcmp(name, "th") == 0)) {
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
    }
    self->closeTableCell();
    if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
      self->makePages();
    }
    self->currentTextBlock.reset();

    const uint16_t columnSpan = parseTableSpan(getAttribute(atts, "colspan"));
    const uint16_t rowSpan = parseTableSpan(getAttribute(atts, "rowspan"));
    if (columnSpan > 1 || rowSpan > 1) {
      self->fallbackTableRowToStacked();
    }
    if (rowSpan > 1) {
      const uint16_t remaining = rowSpan == UINT16_MAX ? UINT16_MAX : static_cast<uint16_t>(rowSpan - 1);
      self->tableRowsSpannedRemaining = std::max(self->tableRowsSpannedRemaining, remaining);
    }

    auto tableCellBlockStyle = BlockStyle();
    tableCellBlockStyle.textAlignDefined = true;
    tableCellBlockStyle.alignment =
        cssStyle.hasTextAlign()
            ? cssStyle.textAlign
            : (self->effectiveTextAlignDefined
                   ? self->effectiveTextAlign
                   : (cssStyle.hasDirection() && cssStyle.direction == CssTextDirection::Rtl ? CssTextAlign::Right
                                                                                             : CssTextAlign::Left));
    if (cssStyle.hasDirection()) {
      tableCellBlockStyle.directionDefined = true;
      tableCellBlockStyle.isRtl = cssStyle.direction == CssTextDirection::Rtl;
    }

    self->currentTextBlock = makeUniqueNoThrow<ParsedText>(self->extraParagraphSpacing, self->hyphenationEnabled,
                                                           self->focusReadingEnabled, tableCellBlockStyle);
    if (!self->currentTextBlock) {
      LOG_ERR("EHP", "OOM: table cell");
      self->skipUntilDepth = self->depth;
      self->depth += 1;
      return;
    }
    self->insideTableCell = true;
    self->tableCellTextBytes = 0;
    self->wordsExtractedInBlock = 0;
    self->flushPendingAnchor();
    self->pushTableTextStyleEntry(cssStyle);

    if (strcmp(name, "th") == 0 && (!cssStyle.hasFontWeight() || cssStyle.fontWeight == CssFontWeight::Bold)) {
      self->boldUntilDepth = std::min(self->boldUntilDepth, self->depth);
    }

    self->depth += 1;
    return;
  }

  if (self->tableDepth >= 1 && strcmp(name, "hr") == 0) {
    self->depth += 1;
    return;
  }

  if (self->tableDepth >= 1 && self->insideTableCell && isHeaderOrBlock(name)) {
    // Collapse block markup inside a cell to a word boundary.
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
    }
    self->nextWordContinues = false;
    self->depth += 1;
    return;
  }

  if (self->tableDepth >= 1 && self->insideTableCell && matches(name, IMAGE_TAGS, std::size(IMAGE_TAGS))) {
    // Preserve alt text without allocating an image framebuffer in the row.
    const char* alt = getAttribute(atts, "alt");
    if (alt && alt[0] != '\0') {
      self->syntheticCharacterData = true;
      self->characterData(userData, alt, strlen(alt));
      self->syntheticCharacterData = false;
    }
    self->skipUntilDepth = self->depth;
    self->depth += 1;
    return;
  }

  if (matches(name, IMAGE_TAGS, std::size(IMAGE_TAGS))) {
    std::string src;
    std::string alt;
    if (atts != nullptr) {
      for (int i = 0; atts[i]; i += 2) {
        if (strcmp(atts[i], "src") == 0) {
          src = atts[i + 1];
        } else if (src.empty() && (strcmp(atts[i], "href") == 0 || strcmp(atts[i], "xlink:href") == 0)) {
          src = atts[i + 1];
        } else if (strcmp(atts[i], "alt") == 0) {
          alt = atts[i + 1];
        }
      }

      const size_t fragmentPos = src.find('#');
      if (fragmentPos != std::string::npos) {
        src.resize(fragmentPos);
      }

      // imageRendering: 0=display, 1=placeholder (alt text only), 2=suppress entirely
      if (self->imageRendering == 2) {
        self->skipUntilDepth = self->depth;
        self->depth += 1;
        return;
      }

      // Skip image if CSS display:none
      if (self->cssParser) {
        CssStyle imgDisplayStyle = self->cssParser->resolveStyle("img", classAttr, &self->cssPath);
        if (!styleAttr.empty()) {
          imgDisplayStyle.applyOver(CssParser::parseInlineStyle(styleAttr));
        }
        if (imgDisplayStyle.hasDisplay() && imgDisplayStyle.display == CssDisplay::None) {
          self->skipUntilDepth = self->depth;
          self->depth += 1;
          return;
        }
      }

      // Gaiji: a tiny inline image standing in for a rare glyph mid-sentence.
      // Emitting it through the image path would end the paragraph around it;
      // append replacement text to the current word instead (see
      // gaijiReplacementText for the alt/filename/geta-mark order).
      if (classAttr.find("gaiji") != std::string::npos) {
        const std::string repl = gaijiReplacementText(src, alt);
        for (const char c : repl) {
          if (self->partWordBufferIndex < MAX_WORD_SIZE) {
            self->partWordBuffer[self->partWordBufferIndex++] = c;
          }
        }
        self->depth += 1;
        return;
      }

      if (!src.empty() && self->imageRendering != 1) {
        LOG_DBG("EHP", "Found image: src=%s", src.c_str());

        {
          // Resolve the image path relative to the HTML file
          std::string resolvedPath = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(self->contentBase + src));

          if (ImageDecoderFactory::isFormatSupported(resolvedPath)) {
            // Create a unique filename for the cached image
            std::string ext;
            size_t extPos = resolvedPath.rfind('.');
            if (extPos != std::string::npos) {
              ext = resolvedPath.substr(extPos);
            }
            std::string cachedImagePath = self->imageBasePath + std::to_string(self->imageCounter++) + ext;

            {
              // Probe the dimensions from the entry's first bytes (early-aborted
              // inflate, a few KB) instead of extracting the whole image now —
              // extraction is deferred to the first render of the page (see
              // ImageBlock's lazy extractor). This is what keeps first-open of an
              // image-heavy chapter from stalling for seconds per image.
              ImageDimensions dims = {0, 0};
              ImageDimsProbe headerProbe;
              self->epub->readItemContentsToStream(resolvedPath, headerProbe, 1024, /*allowEarlyStop=*/true);
              bool gotDimensions = headerProbe.getDimensions(dims);

              if (!gotDimensions) {
                // No header within the stream (rare) — fall back to extracting the
                // whole image and probing the file. That can take seconds, so
                // surface the indexing popup first (single-shot per parser).
                if (self->popupFn && !self->imagePopupFired) {
                  self->imagePopupFired = true;
                  self->popupFn();
                }
                HalFile cachedImageFile;
                bool extractSuccess = false;
                if (Storage.openFileForWrite("EHP", cachedImagePath, cachedImageFile)) {
                  {
                    // The zip inflate window is one contiguous 32KB block and the heap here is
                    // fragmented, not full: measured 89992 free with a largest block of 30708, so
                    // the malloc fallback fails and the image is lost. Freeing more cannot fix a
                    // fragmentation problem -- font reclaim and suspending the build each moved
                    // maxAlloc by exactly 0.
                    //
                    // InflateStream::init() prefers the lent framebuffer (buildscratch) over the
                    // heap, and 48KB comfortably holds its ~11KB state + 32KB window. The blocking
                    // full build holds a loan throughout and its extractions succeed; the
                    // incremental chunk loop deliberately runs without one (so the popup can draw)
                    // and its extractions fail. Take a loan for the extraction itself, which draws
                    // nothing. Nesting-safe: under an outer loan this is inert and behaviour is
                    // unchanged. The chapter HTML is parsed from a temp file, so no outer inflate
                    // is holding the scratch at this point.
                    GfxRenderer::FrameBufferLoan loan(self->renderer);
                    extractSuccess = self->epub->readItemContentsToStream(resolvedPath, cachedImageFile, 4096);
                  }
                  cachedImageFile.flush();
                  cachedImageFile.close();
                }
                if (extractSuccess) {
                  // Retry to absorb SD-card sync latency on slow cards, and to close
                  // the silent-drop bug where a single getDimensions failure was fatal.
                  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(cachedImagePath);
                  for (int attempt = 0; attempt < 3 && !gotDimensions; attempt++) {
                    if (attempt > 0) {
                      delay(50);  // Give a slow SD card time to finish syncing before retrying
                    }
                    gotDimensions = decoder && decoder->getDimensions(cachedImagePath, dims);
                  }
                } else {
                  LOG_ERR("EHP", "Failed to extract image");
                }
              }

              // Neither the header probe nor the full extraction produced dimensions -- on this
              // device that is almost always a transient OOM, since both need a zip inflate stream
              // and images arrive mid-parse when the build's heap is at its worst (measured: 11
              // images in a row failing at "Failed to init inflate stream").
              //
              // The image used to be dropped from the layout here and replaced by alt text. Because
              // no ImageBlock was created, nothing retained resolvedPath, so ImageBlock's lazy
              // extractor had nothing to attach to and the image stayed gone for the life of the
              // section cache -- a transient failure made permanent, the same shape as the vertical
              // bug that VerticalPage::imageSrcPath fixed.
              //
              // Reserve a viewport-sized box and let the block carry its source href instead:
              // render() re-extracts once the heap has recovered, and fits the real image inside
              // the reserved box. Costs a too-generous reservation on the failure path, which is
              // strictly better than losing the image. Falls through to the old behaviour when
              // there is no href to recover from, which is the only genuinely unrecoverable case.
              if (!gotDimensions && !resolvedPath.empty()) {
                // Never leave a partial behind: render() skips lazy extraction when the file
                // already exists, and would then decode a truncated image.
                Storage.remove(cachedImagePath.c_str());
                dims.width = static_cast<int>(self->viewportWidth);
                dims.height = static_cast<int>(self->viewportHeight);
                gotDimensions = true;
                LOG_INF("EHP", "Image %s unavailable during build; reserving box for lazy extraction",
                        resolvedPath.c_str());
              }

              if (gotDimensions) {
                LOG_DBG("EHP", "Image dimensions: %dx%d", dims.width, dims.height);

                int displayWidth = 0;
                int displayHeight = 0;
                const float emSize = static_cast<float>(self->renderer.getFontAscenderSize(self->fontId));
                const CssStyle& imgStyle = cssStyle;
                const bool hasCssHeight = imgStyle.hasImageHeight();
                const bool hasCssWidth = imgStyle.hasImageWidth();

                // Compute effective container width for percentage-based image sizes.
                // If the image is inside a block with horizontal margins/padding (e.g.
                // <div style="margin: 1em 40%">), percentage widths like width:100%
                // should resolve against the container width, not the full viewport.
                int containerWidth = self->viewportWidth;
                if (self->currentTextBlock) {
                  const int inset = self->currentTextBlock->getBlockStyle().totalHorizontalInset();
                  if (inset > 0 && inset < self->viewportWidth) {
                    containerWidth = self->viewportWidth - inset;
                  }
                }

                if (hasCssHeight && hasCssWidth && dims.width > 0 && dims.height > 0) {
                  // Both CSS height and width set: treat them as a bounding box and fit the
                  // image within it preserving its own aspect ratio, the same as the
                  // width-only/height-only branches below, rather than stretching to the
                  // requested box. A stylesheet rule reused across images with different
                  // intrinsic ratios (e.g. a template sized for a wide banner applied to a
                  // roughly-square chapter-heading sketch) would otherwise visibly distort
                  // the artwork.
                  int boxHeight = static_cast<int>(
                      imgStyle.imageHeight.toPixels(emSize, static_cast<float>(self->viewportHeight)) + 0.5f);
                  int boxWidth =
                      static_cast<int>(imgStyle.imageWidth.toPixels(emSize, static_cast<float>(containerWidth)) + 0.5f);
                  if (boxHeight < 1) boxHeight = 1;
                  if (boxWidth < 1) boxWidth = 1;
                  if (boxWidth > containerWidth) boxWidth = containerWidth;
                  if (boxHeight > self->viewportHeight) boxHeight = self->viewportHeight;

                  const float scaleX = static_cast<float>(boxWidth) / dims.width;
                  const float scaleY = static_cast<float>(boxHeight) / dims.height;
                  const float scale = (scaleX < scaleY) ? scaleX : scaleY;
                  displayWidth = static_cast<int>(dims.width * scale + 0.5f);
                  displayHeight = static_cast<int>(dims.height * scale + 0.5f);
                  if (displayWidth < 1) displayWidth = 1;
                  if (displayHeight < 1) displayHeight = 1;
                  LOG_DBG("EHP", "Display size from CSS height+width (aspect preserved): %dx%d", displayWidth,
                          displayHeight);
                } else if (hasCssHeight && !hasCssWidth && dims.width > 0 && dims.height > 0) {
                  // Use CSS height (resolve % against viewport height) and derive width from aspect ratio
                  displayHeight = static_cast<int>(
                      imgStyle.imageHeight.toPixels(emSize, static_cast<float>(self->viewportHeight)) + 0.5f);
                  if (displayHeight < 1) displayHeight = 1;
                  displayWidth =
                      static_cast<int>(displayHeight * (static_cast<float>(dims.width) / dims.height) + 0.5f);
                  if (displayHeight > self->viewportHeight) {
                    displayHeight = self->viewportHeight;
                    // Rescale width to preserve aspect ratio when height is clamped
                    displayWidth =
                        static_cast<int>(displayHeight * (static_cast<float>(dims.width) / dims.height) + 0.5f);
                    if (displayWidth < 1) displayWidth = 1;
                  }
                  if (displayWidth > containerWidth) {
                    displayWidth = containerWidth;
                    // Rescale height to preserve aspect ratio when width is clamped
                    displayHeight =
                        static_cast<int>(displayWidth * (static_cast<float>(dims.height) / dims.width) + 0.5f);
                    if (displayHeight < 1) displayHeight = 1;
                  }
                  if (displayWidth < 1) displayWidth = 1;
                  LOG_DBG("EHP", "Display size from CSS height: %dx%d", displayWidth, displayHeight);
                } else if (hasCssWidth && !hasCssHeight && dims.width > 0 && dims.height > 0) {
                  // Use CSS width (resolve % against container width) and derive height from aspect ratio
                  displayWidth =
                      static_cast<int>(imgStyle.imageWidth.toPixels(emSize, static_cast<float>(containerWidth)) + 0.5f);
                  if (displayWidth > containerWidth) displayWidth = containerWidth;
                  if (displayWidth < 1) displayWidth = 1;
                  displayHeight =
                      static_cast<int>(displayWidth * (static_cast<float>(dims.height) / dims.width) + 0.5f);
                  if (displayHeight > self->viewportHeight) {
                    displayHeight = self->viewportHeight;
                    // Rescale width to preserve aspect ratio when height is clamped
                    displayWidth =
                        static_cast<int>(displayHeight * (static_cast<float>(dims.width) / dims.height) + 0.5f);
                    if (displayWidth < 1) displayWidth = 1;
                  }
                  if (displayHeight < 1) displayHeight = 1;
                  LOG_DBG("EHP", "Display size from CSS width: %dx%d", displayWidth, displayHeight);
                } else {
                  // Scale to fit container while maintaining aspect ratio
                  int maxWidth = containerWidth;
                  int maxHeight = self->viewportHeight;
                  float scaleX = (dims.width > maxWidth) ? (float)maxWidth / dims.width : 1.0f;
                  float scaleY = (dims.height > maxHeight) ? (float)maxHeight / dims.height : 1.0f;
                  float scale = (scaleX < scaleY) ? scaleX : scaleY;
                  if (scale > 1.0f) scale = 1.0f;

                  displayWidth = (int)(dims.width * scale);
                  displayHeight = (int)(dims.height * scale);
                  LOG_DBG("EHP", "Display size: %dx%d (scale %.2f)", displayWidth, displayHeight, scale);
                }

                // Flush any pending text block so it appears before the image
                if (self->partWordBufferIndex > 0) {
                  self->flushPartWordBuffer();
                }
                if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
                  const BlockStyle parentBlockStyle = self->currentTextBlock->getBlockStyle();
                  self->startNewTextBlock(parentBlockStyle);
                }

                // Apply vertical margins from the container to the image.
                // Top margin lives on the empty text block (deposited via vertical merge
                // in startNewTextBlock). Bottom margin was stripped by withoutBottom() for
                // deferred application at element close, so read it from the stack.
                int16_t imageMarginTop = 0;
                int16_t imageMarginBottom = 0;
                if (self->currentTextBlock && self->currentTextBlock->isEmpty()) {
                  const auto& bs = self->currentTextBlock->getBlockStyle();
                  imageMarginTop = bs.topInset();
                  if (self->blockStyleStack.size() > 1) {
                    imageMarginBottom = self->blockStyleStack.back().bottomInset();
                  }
                }

                (void)imageMarginBottom;

                // Images get their own dedicated page. Complete the current page
                // if it already has content, then start a fresh page for the image.
                if (self->currentPage && !self->currentPage->elements.empty()) {
                  self->maybeEmitOpenBoxForPageBreak();
                  self->completePageFn(std::move(self->currentPage), self->xpathParagraphIndex,
                                       self->xpathListItemIndex, self->currentPageVisibleOffset);
                  self->completedPageCount++;
                }
                self->currentPage.reset(new Page());
                if (!self->currentPage) {
                  LOG_ERR("EHP", "Failed to create image page");
                  return;
                }
                self->currentPageNextY = 0;
                self->currentPageVisibleOffsetSet = false;

                // Rotate when the image aspect doesn't match the viewport, so it fills the screen (the
                // user tilts the device to view it). Natural dims are stored deliberately: only
                // ImageBlock::render() knows the rotated frame, and it fits + centres there.
                //
                // This was disabled for one commit (0d4d1aa0) because render() did NOT implement
                // rotation -- it bounds-checked the natural dims against the upright screen and drew
                // nothing at all. Re-enabled now that it does. If a wide image ever goes blank
                // again, check render()'s rotated branch before touching this one.
                const bool viewportIsPortrait = self->viewportHeight > self->viewportWidth;
                const bool imageIsLandscape = dims.width > dims.height;
                const bool rotateImage = dims.width > 0 && dims.height > 0 && (viewportIsPortrait == imageIsLandscape);

                // Apply top margin from container block. Clamp it so the image never
                // overflows the page bottom: a full-viewport-height image leaves no room
                // for the margin, and the break above only fires on non-empty pages, so a
                // fresh page would otherwise place the image at y=marginTop and run
                // marginTop pixels past viewportHeight. A large bottom reserve (status
                // bar / big screen margin) absorbs that overflow silently, but with a
                // thin reserve it crosses the physical screen edge and fails
                // ImageBlock::render's bounds check, dropping the image entirely.
                if (self->currentPageNextY + imageMarginTop + displayHeight > self->viewportHeight) {
                  const int room = self->viewportHeight - displayHeight - self->currentPageNextY;
                  imageMarginTop = static_cast<int16_t>(room > 0 ? room : 0);
                }
                self->currentPageNextY += imageMarginTop;

                // nothrow: make_shared uses bare new, which aborts on OOM under
                // -fno-exceptions; images arrive mid-parse when the heap is at its
                // most loaded, so this must fail soft into the null-check below.
                std::shared_ptr<ImageBlock> imageBlock;
                int xPos = 0;
                int yPos = 0;
                if (rotateImage) {
                  // Store natural dims; render() fits and centres them in the rotated frame.
                  imageBlock = std::shared_ptr<ImageBlock>(
                      new (std::nothrow) ImageBlock(cachedImagePath, resolvedPath, static_cast<int16_t>(dims.width),
                                                    static_cast<int16_t>(dims.height)));
                  if (imageBlock) {
                    const int reserve = std::max(self->renderer.getScreenWidth() - self->viewportWidth,
                                                 self->renderer.getScreenHeight() - self->viewportHeight);
                    imageBlock->setRotated(true, static_cast<int16_t>(reserve));
                  }
                } else {
                  // Scale to fit the full viewport, preserving aspect ratio.
                  // Don't upscale small images beyond their natural size.
                  const float sx = static_cast<float>(self->viewportWidth) / dims.width;
                  const float sy = static_cast<float>(self->viewportHeight) / dims.height;
                  float s = std::min(sx, sy);
                  if (s > 1.0f) s = 1.0f;
                  int fitW = static_cast<int>(dims.width * s + 0.5f);
                  int fitH = static_cast<int>(dims.height * s + 0.5f);
                  if (fitW < 1) fitW = 1;
                  if (fitH < 1) fitH = 1;
                  imageBlock = std::shared_ptr<ImageBlock>(new (std::nothrow) ImageBlock(
                      cachedImagePath, resolvedPath, static_cast<int16_t>(fitW), static_cast<int16_t>(fitH)));
                  xPos = (self->viewportWidth - fitW) / 2;
                  yPos = (self->viewportHeight - fitH) / 2;
                  if (yPos < 0) yPos = 0;
                }
                if (!imageBlock) {
                  LOG_ERR("EHP", "Failed to create ImageBlock");
                  return;
                }
                auto pageImage = std::shared_ptr<PageImage>(
                    new (std::nothrow) PageImage(imageBlock, static_cast<int16_t>(xPos), static_cast<int16_t>(yPos)));
                if (!pageImage) {
                  LOG_ERR("EHP", "Failed to create PageImage");
                  return;
                }
                self->currentPage->elements.push_back(pageImage);
                self->setCurrentPageVisibleOffset(self->visibleTextOffset);

                // Complete the image's dedicated page; start fresh for following text.
                self->maybeEmitOpenBoxForPageBreak();
                self->completePageFn(std::move(self->currentPage), self->xpathParagraphIndex, self->xpathListItemIndex,
                                     self->currentPageVisibleOffset);
                self->completedPageCount++;
                self->currentPage.reset(new Page());
                if (!self->currentPage) {
                  LOG_ERR("EHP", "Failed to create post-image page");
                  return;
                }
                self->currentPageNextY = 0;
                self->currentPageVisibleOffsetSet = false;

                // Reset any empty text block's accumulated vertical spacing.
                if (self->currentTextBlock && self->currentTextBlock->isEmpty()) {
                  BlockStyle resetStyle;
                  resetStyle.alignment = (self->paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None))
                                             ? CssTextAlign::Justify
                                             : static_cast<CssTextAlign>(self->paragraphAlignment);
                  self->currentTextBlock->setBlockStyle(resetStyle);
                }

                self->depth += 1;
                return;
              } else {
                LOG_ERR("EHP", "Failed to get image dimensions");
                Storage.remove(cachedImagePath.c_str());
              }
            }
          }  // isFormatSupported
        }
      }

      // Fallback to alt text if image processing fails
      if (!alt.empty()) {
        alt = "[Image: " + alt + "]";
        self->startNewTextBlock(self->blockStyleStack.back()
                                    .getCombinedBlockStyle(centeredBlockStyle, BlockStyle::CombineAxis::Horizontal)
                                    .withoutBottom());
        self->italicUntilDepth = std::min(self->italicUntilDepth, self->depth);
        self->depth += 1;
        self->syntheticCharacterData = true;
        self->characterData(userData, alt.c_str(), alt.length());
        self->syntheticCharacterData = false;
        // Skip any child content (skip until parent as we pre-advanced depth above)
        self->skipUntilDepth = self->depth - 1;
        return;
      }

      // No alt text, skip
      self->skipUntilDepth = self->depth;
      self->depth += 1;
      return;
    }
  }

  // Ruby tag handling
  if (strcmp(name, "ruby") == 0) {
    // <ruby> is an inline element: a base that follows text with no whitespace between them
    // continues the same visual word, exactly like <b>/<i> handling in endElement().
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->inRuby = true;
    self->rubyStartWordIndex = self->currentTextBlock ? static_cast<int>(self->currentTextBlock->size()) : 0;
    self->rubyElemBase.clear();
    self->rubyElemRuby.clear();
    self->rubyElemRunCount = 0;
    if (self->currentTextBlock) {
      self->currentTextBlock->ensureRubyCapacity();
    }
    self->rubyTextBuffer.clear();
    self->depth += 1;
    return;
  }
  if (strcmp(name, "rt") == 0) {
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
    }
    self->collectingRubyText = true;
    self->depth += 1;
    return;
  }

  if (VisibleTextUtils::isNonVisibleElement(name)) {
    // start skip
    self->skipUntilDepth = self->depth;
    self->depth += 1;
    return;
  }

  // Skip blocks with role="doc-pagebreak" and epub:type="pagebreak"
  if (atts != nullptr) {
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "role") == 0 && strcmp(atts[i + 1], "doc-pagebreak") == 0 ||
          strcmp(atts[i], "epub:type") == 0 && strcmp(atts[i + 1], "pagebreak") == 0) {
        self->skipUntilDepth = self->depth;
        self->depth += 1;
        return;
      }
    }
  }

  // Detect internal <a href="..."> links (footnotes, cross-references)
  // Note: <aside epub:type="footnote"> elements are rendered as normal content
  // without special handling. Links pointing to them are collected as footnotes.
  if (strcmp(name, "a") == 0) {
    const char* href = getAttribute(atts, "href");

    bool isInternalLink = isInternalEpubLink(href);

    // Special case: javascript:void(0) links with data attributes
    // Example: <a href="javascript:void(0)"
    // data-xyz="{&quot;name&quot;:&quot;OPS/ch2.xhtml&quot;,&quot;frag&quot;:&quot;id46&quot;}">
    if (href && strncmp(href, "javascript:", 11) == 0) {
      isInternalLink = false;
      // TODO: Parse data-* attributes to extract actual href
    }

    if (isInternalLink) {
      // Footnote indices are block-relative, so linked rows use ordinary flow.
      if (self->tableDepth >= 1 && self->insideTableCell && !self->tableRowStacked) {
        self->fallbackTableRowToStacked();
      }

      // Flush buffer before style change
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
        self->nextWordContinues = true;
      }
      self->insideFootnoteLink = true;
      self->footnoteLinkDepth = self->depth;
      self->currentFootnoteLinkId = self->currentTextBlock ? self->currentTextBlock->addLinkTarget(href) : 0;
      self->currentFootnote.href.assign(href, strnlen(href, FOOTNOTE_HREF_LEN - 1));
      self->currentFootnote.number[0] = '\0';
      self->currentFootnoteLinkTextLen = 0;

      // Apply underline style to visually indicate the link.
      StyleStackEntry entry;
      entry.depth = self->depth;
      entry.hasTextDecoration = true;
      entry.textDecoration = CssTextDecoration::Underline;
      // Carry the link's own resolved CSS bits too: footnote references are typically made
      // superscript via a class on the <a> itself (.apnb { vertical-align: 70% }) -- the early
      // return below otherwise skips the generic inline-CSS path entirely and the reference
      // rendered as a full-size digit on the baseline.
      if (cssStyle.hasVerticalAlign()) {
        if (cssStyle.verticalAlign == CssVerticalAlign::Super) {
          entry.hasSup = true;
          entry.sup = true;
        } else if (cssStyle.verticalAlign == CssVerticalAlign::Sub) {
          entry.hasSub = true;
          entry.sub = true;
        }
      }
      if (cssStyle.hasFontWeight()) {
        entry.hasBold = true;
        entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
      }
      if (cssStyle.hasFontStyle()) {
        entry.hasItalic = true;
        entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
      }
      applyDirectionToEntry(entry, cssStyle);
      applyTextTransformToEntry(entry, cssStyle);
      self->inlineStyleStack.push_back(entry);
      self->updateEffectiveInlineStyle();

      // Skip CSS resolution — we already handled styling for this <a> tag
      self->depth += 1;
      return;
    }
  }

  // CSS em inside a block resolves against that block's OWN font size, so an element with a
  // font-size gets its margins/padding/indent measured in the larger em too.
  const int blockFontId = cssBlockFontId(cssStyle, self->fontId);
  const float emSize =
      static_cast<float>(self->renderer.getFontAscenderSize(blockFontId != 0 ? blockFontId : self->fontId));
  const auto userAlignmentBlockStyle =
      BlockStyle::fromCssStyle(cssStyle, emSize, static_cast<CssTextAlign>(self->paragraphAlignment),
                               self->viewportWidth, self->honorBookInsets, self->fontId);

  // Defer every block border until its content is placed. This keeps top/bottom pairs together
  // across page breaks and lets bottom-only heading/worksheet rules render at all.
  if (self->boxDepth < 0 && cssStyle.hasBorder() && cssStyle.borderEdgeMask() != 0 && isHeaderOrBlock(name) &&
      self->tableDepth == 0) {
    self->flushPendingBlockLayout();
    self->boxDepth = self->depth;
    self->boxBorderSpec = cssStyle.borderEdges;
    self->boxContinued = false;
    self->boxAwaitingFirstLine = true;
    self->boxShrinkToContent = cssStyle.display == CssDisplay::InlineBlock;
    self->boxFirstElementIndex = self->currentPage ? self->currentPage->elements.size() : 0;
    // Adjacent CSS vertical margins collapse. For an empty worksheet paragraph, advancing by the
    // sum makes each ruled row roughly twice as tall as the publisher intended.
    const int emptySpacing = std::max(userAlignmentBlockStyle.topInset(), userAlignmentBlockStyle.bottomInset());
    self->boxEmptyAdvance =
        static_cast<int16_t>(std::clamp(emptySpacing, std::max(1, self->renderer.getLineHeight(self->fontId) / 2),
                                        static_cast<int>(self->viewportHeight)));
    LOG_DBG("EHP", "border open: <%s class=%s> edges=0x%02x style=%u width=%u", name, classAttr.c_str(),
            cssStyle.borderEdgeMask(), static_cast<unsigned>(cssStyle.borderLineStyle()), cssStyle.borderLineWidth());
  }

  if (strcmp(name, "hr") == 0) {
    auto hrBlockStyle = BlockStyle::fromCssStyle(cssStyle, emSize, CssTextAlign::Left, self->viewportWidth,
                                                 self->honorBookInsets, self->fontId);
    if (!self->embeddedStyle) {
      hrBlockStyle.marginLeft = 0;
      hrBlockStyle.marginRight = 0;
      hrBlockStyle.marginTop = 0;
      hrBlockStyle.marginBottom = 0;
      hrBlockStyle.paddingLeft = 0;
      hrBlockStyle.paddingRight = 0;
      hrBlockStyle.paddingTop = 0;
      hrBlockStyle.paddingBottom = 0;
      hrBlockStyle.textIndentDefined = false;
      hrBlockStyle.textIndent = 0;
    }
    self->emitHorizontalRule(hrBlockStyle);
    self->depth += 1;
    return;
  }

  if (matches(name, HEADER_TAGS, std::size(HEADER_TAGS))) {
    self->currentCssStyle = cssStyle;
    // HTML headings default to left alignment. The former hard-coded Center made every h2/h3
    // ignore the publisher unless it repeated `text-align:left`, which is why the comparison
    // book's inline-block headings floated in the middle of the page.
    const auto headingAlignment = self->paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None)
                                      ? CssTextAlign::Left
                                      : static_cast<CssTextAlign>(self->paragraphAlignment);
    auto headerBlockStyle = BlockStyle::fromCssStyle(cssStyle, emSize, headingAlignment, self->viewportWidth,
                                                     self->honorBookInsets, self->fontId);
    headerBlockStyle.textAlignDefined = true;
    if (self->embeddedStyle && cssStyle.hasTextAlign()) {
      headerBlockStyle.alignment = cssStyle.textAlign;
    }
    const auto accumulated =
        self->blockStyleStack.back().getCombinedBlockStyle(headerBlockStyle, BlockStyle::CombineAxis::Horizontal);
    self->blockStyleStack.push_back(accumulated);
    self->startNewTextBlock(accumulated.withoutBottom());
    // AFTER startNewTextBlock: that call flushes the PREVIOUS block, which must stay on the page
    // it was already on. The break is spent by the first block laid out from here on.
    if (cssStyle.pageBreakBefore() == CssPageBreak::Always) self->pendingForcedBreak = true;
    self->boldUntilDepth = std::min(self->boldUntilDepth, self->depth);
    self->updateEffectiveInlineStyle();
  } else if (matches(name, BLOCK_TAGS, std::size(BLOCK_TAGS))) {
    if (strcmp(name, "br") == 0) {
      if (self->partWordBufferIndex > 0) {
        // flush word preceding <br/> to currentTextBlock before calling startNewTextBlock
        self->flushPartWordBuffer();
      }
      // A <br> after text is a line break: start the next block with the container's
      // vertical margins stripped, matching browsers, which never apply paragraph
      // margins at a <br>. This is what keeps <br>-per-paragraph books (common CJK
      // web-novel formatting) from re-adding container spacing at every paragraph
      // and collapsing page capacity.
      // A <br> on an empty block (consecutive <br>s, or a standalone <br> between
      // blocks) is a scene-break separator: keep the container margins so deposited
      // vertical spacing survives. Either way the block is tagged so that if it
      // stays empty, startNewTextBlock injects a full line-height gap when the next
      // block opens; once text follows the tag is inert.
      // Style comes from the block style stack, not the current block, so a closed
      // element's style can't leak through (#2679).
      BlockStyle brStyle = self->blockStyleStack.back();
      if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
        brStyle = brStyle.withoutTop().withoutBottom();
      }
      brStyle.fromBrElement = true;
      self->startNewTextBlock(brStyle);
    } else {
      self->currentCssStyle = cssStyle;
      const auto accumulated = self->blockStyleStack.back().getCombinedBlockStyle(userAlignmentBlockStyle,
                                                                                  BlockStyle::CombineAxis::Horizontal);
      self->blockStyleStack.push_back(accumulated);
      self->startNewTextBlock(accumulated.withoutBottom());
      // See the header branch above: raised only once the previous block has been flushed.
      if (cssStyle.pageBreakBefore() == CssPageBreak::Always) self->pendingForcedBreak = true;
      self->updateEffectiveInlineStyle();

      if (strcmp(name, "ol") == 0 || strcmp(name, "ul") == 0) {
        // Track list nesting for list-style-type markers: <ol> counts decimal by
        // default, <ul> draws discs; the element's own list-style-type overrides.
        // Lists sit in the block branch (not a bare side branch) so their block CSS --
        // ul { margin-left: 2.5em } indents, vertical margins, page-breaks -- lays out
        // through the same stack push/pop every other container gets.
        if (self->listDepth < kMaxListDepth) {
          ListCtx ctx;
          ctx.counter = 0;
          ctx.type = cssStyle.hasListStyleType()
                         ? cssStyle.listStyleType
                         : (name[0] == 'o' ? CssListStyleType::Decimal : CssListStyleType::Disc);
          self->listStack[self->listDepth] = ctx;
        }
        self->listDepth++;
      }

      if (strcmp(name, "li") == 0) {
        // Marker per list-style-type: the li's own value wins, else the
        // enclosing list's (bullet when the li sits outside any tracked list).
        // Markers are synthetic text: anchor them at the current visible offset.
        ListCtx* ctx = self->listDepth > 0 ? &self->listStack[std::min(self->listDepth, kMaxListDepth) - 1] : nullptr;
        const CssListStyleType type =
            cssStyle.hasListStyleType() ? cssStyle.listStyleType : (ctx ? ctx->type : CssListStyleType::Disc);
        switch (type) {
          case CssListStyleType::NoMarker:
            break;
          case CssListStyleType::Decimal:
            if (ctx) {
              char marker[8];
              snprintf(marker, sizeof(marker), "%u.", static_cast<unsigned>(++ctx->counter));
              self->currentTextBlock->addWord(marker, EpdFontFamily::REGULAR, false, false, 0, self->visibleTextOffset);
              break;
            }
            [[fallthrough]];  // decimal without a list context: plain bullet
          case CssListStyleType::Disc:
            self->currentTextBlock->addWord("\xe2\x80\xa2", EpdFontFamily::REGULAR, false, false, 0,
                                            self->visibleTextOffset);  // •
            break;
          case CssListStyleType::Circle:
            self->currentTextBlock->addWord("\xe2\x97\x8b", EpdFontFamily::REGULAR, false, false, 0,
                                            self->visibleTextOffset);  // ○
            break;
          case CssListStyleType::Square:
            self->currentTextBlock->addWord("\xe2\x96\xa0", EpdFontFamily::REGULAR, false, false, 0,
                                            self->visibleTextOffset);  // ■
            break;
        }
        self->listItemBulletOnly = true;
      }
    }
  } else if (matches(name, UNDERLINE_TAGS, std::size(UNDERLINE_TAGS))) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->pushDecorationStyleEntry(CssTextDecoration::Underline, cssStyle);
  } else if (matches(name, LINETHROUGH_TAGS, std::size(LINETHROUGH_TAGS))) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->pushDecorationStyleEntry(CssTextDecoration::LineThrough, cssStyle);
  } else if (matches(name, BOLD_TAGS, std::size(BOLD_TAGS))) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->boldUntilDepth = std::min(self->boldUntilDepth, self->depth);
    // Push inline style entry for bold tag
    StyleStackEntry entry;
    entry.depth = self->depth;  // Track depth for matching pop
    entry.hasBold = true;
    entry.bold = true;
    if (cssStyle.hasFontStyle()) {
      entry.hasItalic = true;
      entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
    }
    applyTextDecorationToEntry(entry, cssStyle);
    applyDirectionToEntry(entry, cssStyle);
    applyTextTransformToEntry(entry, cssStyle);
    self->inlineStyleStack.push_back(entry);
    self->updateEffectiveInlineStyle();
  } else if (matches(name, ITALIC_TAGS, std::size(ITALIC_TAGS))) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->italicUntilDepth = std::min(self->italicUntilDepth, self->depth);
    // Push inline style entry for italic tag
    StyleStackEntry entry;
    entry.depth = self->depth;  // Track depth for matching pop
    entry.hasItalic = true;
    entry.italic = true;
    if (cssStyle.hasFontWeight()) {
      entry.hasBold = true;
      entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
    }
    applyTextDecorationToEntry(entry, cssStyle);
    applyDirectionToEntry(entry, cssStyle);
    applyTextTransformToEntry(entry, cssStyle);
    self->inlineStyleStack.push_back(entry);
    self->updateEffectiveInlineStyle();
  } else if (strcmp(name, "sup") == 0 || strcmp(name, "sub") == 0) {
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    StyleStackEntry entry;
    entry.depth = self->depth;
    if (strcmp(name, "sup") == 0) {
      entry.hasSup = true;
      entry.sup = true;
    } else {
      entry.hasSub = true;
      entry.sub = true;
    }
    // The tag's own CSS: `sup { font-style: italic }` etc. must survive like on any inline
    // element. font-size is deliberately NOT forwarded -- sup/sub already render at 50% scale.
    if (cssStyle.hasFontWeight()) {
      entry.hasBold = true;
      entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
    }
    if (cssStyle.hasFontStyle()) {
      entry.hasItalic = true;
      entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
    }
    applyTextDecorationToEntry(entry, cssStyle);
    applyDirectionToEntry(entry, cssStyle);
    applyTextTransformToEntry(entry, cssStyle);
    self->inlineStyleStack.push_back(entry);
    self->updateEffectiveInlineStyle();
  } else if (strcmp(name, "span") == 0 || !isHeaderOrBlock(name)) {
    // Handle span and other inline elements for CSS styling.
    const bool inheritedTableTextAlign = self->tableDepth >= 1 && cssStyle.hasTextAlign();
    if (cssStyle.hasFontWeight() || cssStyle.hasFontStyle() || cssStyle.hasTextDecoration() ||
        cssStyle.hasDirection() || cssStyle.hasVerticalAlign() || cssStyle.hasTextEmphasis() ||
        cssStyle.hasFontVariant() || cssStyle.hasTextTransform() || cssStyle.hasFontSize() || inheritedTableTextAlign) {
      // Flush buffer before style change so preceding text gets current style
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
        self->nextWordContinues = true;
      }
      StyleStackEntry entry;
      entry.depth = self->depth;  // Track depth for matching pop
      if (cssStyle.hasFontWeight()) {
        entry.hasBold = true;
        entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
      }
      if (cssStyle.hasFontStyle()) {
        entry.hasItalic = true;
        entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
      }
      applyTextDecorationToEntry(entry, cssStyle);
      applyDirectionToEntry(entry, cssStyle);
      applyTextTransformToEntry(entry, cssStyle);
      entry.setsParagraphDirection = strcmp(name, "html") == 0 || strcmp(name, "body") == 0;
      if (inheritedTableTextAlign) {
        entry.hasTextAlign = true;
        entry.textAlign = cssStyle.textAlign;
      }
      applyVerticalAlignToEntry(entry, cssStyle);
      if (cssStyle.hasTextEmphasis()) {
        entry.hasEmphasis = true;
        entry.emphasis = cssStyle.textEmphasis;
      }
      if (cssStyle.hasFontVariant()) {
        entry.hasSmallCaps = true;
        entry.smallCaps = cssStyle.fontVariant == CssFontVariant::SmallCaps;
      }
      if (cssStyle.hasFontSize()) {
        entry.hasFontId = true;
        // Body-relative, exactly like the block path (BlockStyle::fromCssStyle passes the
        // reader's font as the em base): the ladder snaps to a resident 12/14/16/18pt size.
        // 0 = no distinct size resident (or SD-card base font) -> the block font is kept.
        entry.fontIdOverride = cssBlockFontId(cssStyle, self->fontId);
      }
      self->inlineStyleStack.push_back(entry);
      self->updateEffectiveInlineStyle();
    }
  }

  // Unprocessed tag, just increasing depth and continue forward
  self->depth += 1;
}

void XMLCALL ChapterHtmlSlimParser::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);
  const bool countVisibleOffsets = self->insideBody && self->nonVisibleTextDepth == 0 && !self->syntheticCharacterData;
  const uint32_t callbackVisibleOffset = self->visibleTextOffset;
  if (countVisibleOffsets) {
    const unsigned char* ptr = reinterpret_cast<const unsigned char*>(s);
    const unsigned char* end = ptr + len;
    while (ptr < end) {
      utf8NextCodepoint(&ptr);
      self->visibleTextOffset++;
    }
  }

  // Nested content needs an enclosing bounded cell collector.
  if (self->tableDepth > 1 && !self->insideTableCell) {
    return;
  }

  // Middle of skip
  if (self->skipUntilDepth < self->depth) {
    return;
  }

  // Collect ruby text instead of normal word processing.
  if (self->collectingRubyText) {
    self->rubyTextBuffer.append(s, len);
    return;
  }

  if (self->tableDepth == 1 && !self->insideTableCell) {
    bool onlyWhitespace = true;
    for (int i = 0; i < len; ++i) {
      if (!isWhitespace(s[i])) {
        onlyWhitespace = false;
        break;
      }
    }
    if (onlyWhitespace) {
      return;
    }
  }

  // Recreate flow storage for valid text (for example a caption) after a row.
  if (!self->currentTextBlock) {
    const BlockStyle flowStyle =
        self->blockStyleStack.empty() ? BlockStyle() : self->blockStyleStack.back().withoutBottom();
    self->currentTextBlock = makeUniqueNoThrow<ParsedText>(self->extraParagraphSpacing, self->hyphenationEnabled,
                                                           self->focusReadingEnabled, flowStyle);
    if (!self->currentTextBlock) {
      LOG_ERR("EHP", "OOM: text block for character data");
      return;
    }
    self->wordsExtractedInBlock = 0;
  }

  // Collect footnote link display text (for the number label)
  // Skip whitespace and brackets to normalize noterefs like "[1]" → "1"
  if (self->insideFootnoteLink) {
    int start = 0;
    int end = len - 1;

    // Example input and output texts:
    // "     [  12  ]   " => "12"
    // "   turn to 256  " => "turn to 256"

    // Ignore leading whitespaces and left square brackets
    while (start < len && (isWhitespace(s[start]) || (s[start] == '['))) {
      ++start;
    }

    // Ignore trailing whitespaces and right square brackets
    while (end >= start && (isWhitespace(s[end]) || (s[end] == ']'))) {
      --end;
    }

    // Extract footnote link text
    for (int i = start; (self->currentFootnoteLinkTextLen < sizeof(self->currentFootnote.number) - 1) && (i <= end);
         ++i) {
      self->currentFootnote.number[self->currentFootnoteLinkTextLen++] = s[i];
    }
    self->currentFootnote.number[self->currentFootnoteLinkTextLen] = '\0';
  }

  uint32_t nextCodepointOffset = callbackVisibleOffset;
  for (int i = 0; i < len; i++) {
    const uint32_t codepointOffset = nextCodepointOffset;
    if (countVisibleOffsets && (static_cast<uint8_t>(s[i]) & 0xC0) != 0x80) {
      nextCodepointOffset++;
    }

    if (isWhitespace(s[i])) {
      // Currently looking at whitespace, if there's anything in the partWordBuffer, flush it
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }
      // Whitespace is a real word boundary — reset continuation state
      self->nextWordContinues = false;
      // Skip the whitespace char
      continue;
    }

    // Detect U+00A0 (non-breaking space, UTF-8: 0xC2 0xA0) or
    //        U+202F (narrow no-break space, UTF-8: 0xE2 0x80 0xAF).
    //
    // Both are rendered as a visible space but must never allow a line break around them.
    // We split the no-break space into its own word token and link the surrounding words
    // with continuation flags so the layout engine treats them as an indivisible group.
    //
    // Example: "200&#xA0;Quadratkilometer" or "200&#x202F;Quadratkilometer"
    //   Input bytes:  "200\xC2\xA0Quadratkilometer"  (or 0xE2 0x80 0xAF for U+202F)
    //   Tokens produced:
    //     [0] "200"               continues=false
    //     [1] " "                 continues=true   (attaches to "200", no gap)
    //     [2] "Quadratkilometer"  continues=true   (attaches to " ", no gap)
    //
    //   The continuation flags prevent the line-breaker from inserting a line break
    //   between "200" and "Quadratkilometer". However, "Quadratkilometer" is now a
    //   standalone word for hyphenation purposes, so Liang patterns can produce
    //   "200 Quadrat-" / "kilometer" instead of the unusable "200" / "Quadratkilometer".
    if (static_cast<uint8_t>(s[i]) == 0xC2 && i + 1 < len && static_cast<uint8_t>(s[i + 1]) == 0xA0) {
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }

      self->partWordBuffer[0] = ' ';
      self->partWordBuffer[1] = '\0';
      self->partWordBufferIndex = 1;
      self->partWordVisibleOffset = codepointOffset;
      self->nextWordContinues = true;  // Attach space to previous word (no break).
      self->flushPartWordBuffer();

      self->nextWordContinues = true;  // Next real word attaches to this space (no break).

      i++;  // Skip the second byte (0xA0)
      continue;
    }

    // U+202F (narrow no-break space) — identical logic to U+00A0 above.
    if (static_cast<uint8_t>(s[i]) == 0xE2 && i + 2 < len && static_cast<uint8_t>(s[i + 1]) == 0x80 &&
        static_cast<uint8_t>(s[i + 2]) == 0xAF) {
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }

      self->partWordBuffer[0] = ' ';
      self->partWordBuffer[1] = '\0';
      self->partWordBufferIndex = 1;
      self->partWordVisibleOffset = codepointOffset;
      self->nextWordContinues = true;
      self->flushPartWordBuffer();

      self->nextWordContinues = true;

      i += 2;  // Skip the remaining two bytes (0x80 0xAF)
      continue;
    }

    // Skip Zero Width No-Break Space / BOM (U+FEFF) = 0xEF 0xBB 0xBF
    const XML_Char FEFF_BYTE_1 = static_cast<XML_Char>(0xEF);
    const XML_Char FEFF_BYTE_2 = static_cast<XML_Char>(0xBB);
    const XML_Char FEFF_BYTE_3 = static_cast<XML_Char>(0xBF);

    if (s[i] == FEFF_BYTE_1) {
      // Check if the next two bytes complete the 3-byte sequence
      if ((i + 2 < len) && (s[i + 1] == FEFF_BYTE_2) && (s[i + 2] == FEFF_BYTE_3)) {
        // Sequence 0xEF 0xBB 0xBF found!
        i += 2;    // Skip the next two bytes
        continue;  // Move to the next iteration
      }
    }

    // If we're about to run out of space, then cut the word off and start a new one.
    // For CJK text (no spaces), this is the primary word-breaking mechanism.
    // We must avoid splitting multi-byte UTF-8 sequences across word boundaries,
    // otherwise the trailing bytes become orphaned continuation bytes that the
    // decoder can't interpret.
    if (self->partWordBufferIndex >= MAX_WORD_SIZE) {
      int safeLen = utf8SafeTruncateBuffer(self->partWordBuffer, self->partWordBufferIndex);

      if (safeLen < self->partWordBufferIndex && safeLen > 0) {
        // Incomplete UTF-8 sequence at the end — save it before flushing
        int overflow = self->partWordBufferIndex - safeLen;
        uint32_t overflowVisibleOffset = self->partWordVisibleOffset;
        const unsigned char* offsetPtr = reinterpret_cast<const unsigned char*>(self->partWordBuffer);
        const unsigned char* const safeEnd = offsetPtr + safeLen;
        while (offsetPtr < safeEnd) {
          utf8NextCodepoint(&offsetPtr);
          overflowVisibleOffset++;
        }
        char saved[4];
        for (int j = 0; j < overflow; j++) {
          saved[j] = self->partWordBuffer[safeLen + j];
        }
        self->partWordBufferIndex = safeLen;
        self->flushPartWordBuffer();
        self->nextWordContinues = true;
        for (int j = 0; j < overflow; j++) {
          self->partWordBuffer[j] = saved[j];
        }
        self->partWordBufferIndex = overflow;
        self->partWordVisibleOffset = overflowVisibleOffset;
      } else {
        self->flushPartWordBuffer();
        self->nextWordContinues = true;
      }
    }

    if (self->partWordBufferIndex == 0) {
      self->partWordVisibleOffset = codepointOffset;
    }
    self->partWordBuffer[self->partWordBufferIndex++] = s[i];
  }

  // Keep token growth bounded: CSS-heavy spans can fragment text into many tiny
  // words, so flush earlier when embedded CSS is active. We still keep the
  // "exclude last line" behavior to preserve paragraph flow across chunks.
  const size_t blockWordCount = self->currentTextBlock->size();
  const size_t softFlushThreshold =
      self->embeddedStyle ? TEXT_BLOCK_SOFT_FLUSH_WORDS_WITH_CSS : TEXT_BLOCK_SOFT_FLUSH_WORDS;
  if (blockWordCount > softFlushThreshold && !self->inRuby) {
    LOG_DBG("EHP", "Text block soft flush (%u words)", static_cast<unsigned>(blockWordCount));
    const int horizontalInset = self->currentTextBlock->getBlockStyle().totalHorizontalInset();
    const uint16_t effectiveWidth = (horizontalInset < self->viewportWidth)
                                        ? static_cast<uint16_t>(self->viewportWidth - horizontalInset)
                                        : self->viewportWidth;
    self->currentTextBlock->layoutAndExtractLines(
        self->renderer, self->fontId, effectiveWidth,
        [self](const std::shared_ptr<TextBlock>& textBlock, const uint32_t offset) {
          self->addLineToPage(textBlock, offset);
        },
        false);
  }
}

void XMLCALL ChapterHtmlSlimParser::defaultHandlerExpand(void* userData, const XML_Char* s, const int len) {
  // Check if this looks like an entity reference (&...;)
  if (len >= 3 && s[0] == '&' && s[len - 1] == ';') {
    const char* utf8Value = lookupHtmlEntity(s, static_cast<size_t>(len));
    if (utf8Value != nullptr) {
      // Known entity: expand to its UTF-8 value
      characterData(userData, utf8Value, strlen(utf8Value));
      return;
    }
    // Unknown entity: preserve original &...; sequence
    characterData(userData, s, len);
    return;
  }
  // Not an entity we recognize - skip it
}

void XMLCALL ChapterHtmlSlimParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);
  if (self->nonVisibleTextDepth > 0) {
    self->nonVisibleTextDepth--;
  }

  // Close this element on the CSS ancestor chain FIRST, pairing with the single push at the
  // top of startElement. Every early return below must leave the chain already popped, or a
  // <rt>/<ruby> subtree would leak an entry and every later element would match against a
  // phantom ancestor.
  self->cssPath.pop();

  // Ruby text: </rt> distributes ruby to base words, </ruby> resets ruby state
  if (strcmp(name, "rt") == 0) {
    self->collectingRubyText = false;
    if (self->inRuby && self->currentTextBlock) {
      const int currentWordCount = static_cast<int>(self->currentTextBlock->size());
      const int baseWordCount = currentWordCount - self->rubyStartWordIndex;
      std::string cleanRuby = trimAndNormalize(self->rubyTextBuffer);
      if (!cleanRuby.empty()) {
        if (baseWordCount > 0) {
          self->currentTextBlock->setRubyGroupAt(self->rubyStartWordIndex, baseWordCount, cleanRuby);
          // Harvest for the per-book furigana glossary: the base is every word this <rt>
          // was just attached to (a group ruby can span several).
          std::string base;
          for (int w = self->rubyStartWordIndex; w < currentWordCount; w++) {
            base += self->currentTextBlock->wordAt(static_cast<size_t>(w));
          }
          if (!base.empty()) {
            RubyGlossary::collect(self->rubyHarvest, base, cleanRuby);
            self->rubyElemBase += base;
            self->rubyElemRuby += cleanRuby;
            self->rubyElemRunCount++;
          }
          self->rubyStartWordIndex = currentWordCount;
        } else if (self->rubyStartWordIndex > 0) {
          int leaderIdx = self->rubyStartWordIndex - 1;
          while (leaderIdx >= 0 &&
                 (self->currentTextBlock->getWordStyleAt(leaderIdx) & EpdFontFamily::RUBY_CONTINUE) != 0) {
            leaderIdx--;
          }
          if (leaderIdx >= 0) {
            std::string prevRuby = self->currentTextBlock->getRubyTextAt(leaderIdx);
            self->currentTextBlock->setRubyForWordAt(leaderIdx, prevRuby + cleanRuby);
          }
        }
      }
    }
    self->rubyTextBuffer.clear();
    // Inline close: the next base (e.g. 字 in <ruby>漢<rt>かん</rt>字<rt>じ</rt></ruby>) joins the
    // preceding one with no space. Whitespace in the source resets this in characterData().
    if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
      self->nextWordContinues = true;
    }
    self->depth -= 1;
    return;
  }
  if (strcmp(name, "ruby") == 0 && self->inRuby) {
    // Mono-ruby element (per-kanji <rt>s): also record the compound as one pair, so
    // 林檎/りんご is found even though the book annotated 林/りん and 檎/ご.
    if (self->rubyElemRunCount >= 2) {
      RubyGlossary::collect(self->rubyHarvest, self->rubyElemBase, self->rubyElemRuby);
    }
    self->rubyElemBase.clear();
    self->rubyElemRuby.clear();
    self->rubyElemRunCount = 0;
    self->inRuby = false;
    self->rubyStartWordIndex = -1;
    self->rubyTextBuffer.clear();
    // Inline close: text following </ruby> joins the annotated base with no space.
    if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
      self->nextWordContinues = true;
    }
    self->depth -= 1;
    return;
  }
  // Check if any style state will change after we decrement depth
  // If so, we MUST flush the partWordBuffer with the CURRENT style first
  // Note: depth hasn't been decremented yet, so we check against (depth - 1)
  const bool willPopStyleStack =
      !self->inlineStyleStack.empty() && self->inlineStyleStack.back().depth == self->depth - 1;
  const bool willClearBold = self->boldUntilDepth == self->depth - 1;
  const bool willClearItalic = self->italicUntilDepth == self->depth - 1;

  const bool styleWillChange = willPopStyleStack || willClearBold || willClearItalic;
  const bool headerOrBlockTag = isHeaderOrBlock(name);
  const bool tableStructuralTag = isTableStructuralTag(name);
  const bool insideSkippedSubtree = self->depth - 1 >= self->skipUntilDepth;

  if (!insideSkippedSubtree && self->tableDepth > 1 && strcmp(name, "table") == 0) {
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
    }
    self->nextWordContinues = false;
    self->tableDepth -= 1;
    self->depth -= 1;
    LOG_DBG("EHP", "nested table flattened into enclosing cell");
    return;
  }

  if (!insideSkippedSubtree && self->tableDepth >= 1 && self->insideTableCell && headerOrBlockTag) {
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
    }
    self->nextWordContinues = false;
    self->depth -= 1;
    return;
  }

  // Flush buffer with current style BEFORE any style changes
  if (self->partWordBufferIndex > 0) {
    // Flush if style will change OR if we're closing a block/structural element
    const bool isInlineTag = !headerOrBlockTag && !tableStructuralTag &&
                             !matches(name, IMAGE_TAGS, std::size(IMAGE_TAGS)) && self->depth != 1;
    // A ruby base wrapped in a styleless inline element (Calibre writes
    // <ruby><span class="xhtml_rb">心臓</span><rt>ハート</rt></ruby> for <rb>) would otherwise
    // never be flushed: `span` is in none of the tag lists below, and with no CSS rule behind
    // its class it pushes no style, so styleWillChange is false. The base then never becomes a
    // word, and the </rt> handler sees baseWordCount == 0 and hangs the annotation on the
    // PRECEDING word instead -- or drops it when the ruby opens a paragraph. Flushing here ends
    // the base at its own closing tag, which is where it ends visually too.
    const bool closingRubyBase = self->inRuby && !self->collectingRubyText;
    const bool shouldFlush = closingRubyBase || styleWillChange || headerOrBlockTag ||
                             matches(name, BOLD_TAGS, std::size(BOLD_TAGS)) ||
                             matches(name, ITALIC_TAGS, std::size(ITALIC_TAGS)) ||
                             matches(name, UNDERLINE_TAGS, std::size(UNDERLINE_TAGS)) ||
                             matches(name, LINETHROUGH_TAGS, std::size(LINETHROUGH_TAGS)) || tableStructuralTag ||
                             matches(name, IMAGE_TAGS, std::size(IMAGE_TAGS)) || self->depth == 1;

    if (shouldFlush) {
      self->flushPartWordBuffer();
      // If closing an inline element, the next word fragment continues the same visual word
      if (isInlineTag) {
        self->nextWordContinues = true;
      }
    }
  }

  self->depth -= 1;

  // Closing the boxed block: lay out its trailing text and draw the rect.
  if (self->boxDepth >= 0 && self->depth == self->boxDepth) {
    self->closeBoxBlock();
  }

  if (strcmp(name, "ol") == 0 || strcmp(name, "ul") == 0) {
    if (self->listDepth > 0) self->listDepth--;
  }

  // Closing a footnote link — create entry from collected text and href
  if (self->insideFootnoteLink && self->depth == self->footnoteLinkDepth) {
    if (self->currentFootnote.number[0] != '\0' && !self->currentFootnote.href.empty()) {
      FootnoteEntry entry;
      strncpy(entry.number, self->currentFootnote.number, sizeof(entry.number) - 1);
      entry.number[sizeof(entry.number) - 1] = '\0';
      entry.href = self->currentFootnote.href;
      int wordIndex =
          self->wordsExtractedInBlock + (self->currentTextBlock ? static_cast<int>(self->currentTextBlock->size()) : 0);
      self->pendingFootnotes.push_back({wordIndex, std::move(entry)});
    }
    self->insideFootnoteLink = false;
    self->currentFootnoteLinkId = 0;
  }

  // Leaving skip
  if (self->skipUntilDepth == self->depth) {
    self->skipUntilDepth = INT_MAX;
  }

  if (!insideSkippedSubtree && self->tableDepth == 1 && (strcmp(name, "td") == 0 || strcmp(name, "th") == 0)) {
    self->closeTableCell();
    self->nextWordContinues = false;
    // The cell's CSS was made current at <td>/<th>; td/th are not block tags, so the generic
    // block close below never resets it -- do it here or it leaks into the next cell's prefix.
    self->currentCssStyle.reset();
    self->updateEffectiveInlineStyle();
  }

  if (!insideSkippedSubtree && self->tableDepth == 1 && (strcmp(name, "tr") == 0)) {
    self->finishTableRow();
    self->nextWordContinues = false;
  }

  if (!insideSkippedSubtree && self->tableDepth == 1 && strcmp(name, "table") == 0) {
    self->finishTableRow();
    if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
      self->makePages();
    }
    self->currentTextBlock.reset();
    self->tableDepth = 0;
    self->insideTableCell = false;
    self->tableRowStacked = false;
    self->tableRowsSpannedRemaining = 0;
    self->tableCellTextBytes = 0;
    self->tableRowCells.clear();
    self->nextWordContinues = false;
    self->currentCssStyle.reset();
    self->updateEffectiveInlineStyle();

    const BlockStyle flowStyle =
        self->blockStyleStack.empty() ? BlockStyle() : self->blockStyleStack.back().withoutBottom();
    self->currentTextBlock = makeUniqueNoThrow<ParsedText>(self->extraParagraphSpacing, self->hyphenationEnabled,
                                                           self->focusReadingEnabled, flowStyle);
    if (!self->currentTextBlock) {
      LOG_ERR("EHP", "OOM: text block after table");
    }
    self->wordsExtractedInBlock = 0;
  }

  // Leaving bold tag
  if (self->boldUntilDepth == self->depth) {
    self->boldUntilDepth = INT_MAX;
  }

  // Leaving italic tag
  if (self->italicUntilDepth == self->depth) {
    self->italicUntilDepth = INT_MAX;
  }

  // Pop from inline style stack if we pushed an entry at this depth
  // This handles all inline elements: b, i, u, span, etc.
  if (!self->inlineStyleStack.empty() && self->inlineStyleStack.back().depth == self->depth) {
    self->inlineStyleStack.pop_back();
    self->updateEffectiveInlineStyle();
  }

  // Clear block style when leaving header or block elements
  if (headerOrBlockTag && !insideSkippedSubtree) {
    self->currentCssStyle.reset();
    self->updateEffectiveInlineStyle();

    // br is self-closing and not a container — it doesn't push/pop the stack.
    if (strcmp(name, "br") != 0 && self->blockStyleStack.size() > 1) {
      // Apply closing element's bottom margin to the current text block so
      // container spacing appears after the element's content (on the last child),
      // not on the first child via the empty-block merge in startNewTextBlock.
      if (self->currentTextBlock) {
        const auto style = self->currentTextBlock->getBlockStyle();
        self->currentTextBlock->setBlockStyle(style.addBottom(self->blockStyleStack.back()));
      }
      // page-break-after: always on the element that is closing. Read from its OWN stack entry
      // (the accumulated style of this element, about to be popped) and raised as a one-shot,
      // so a container gets one break after its content rather than one per child block.
      const bool breakAfterElement =
          cssPageBreakGet(self->blockStyleStack.back().pageBreaks, CssPageBreakSlot::After) == CssPageBreak::Always;
      self->blockStyleStack.pop_back();
      // Start a new text block with the parent style to prevent subsequent bare text
      // from inheriting the closed block style (e.g. alignment or margins).
      // Vertical margins and paddings are stripped
      self->startNewTextBlock(self->blockStyleStack.back().withoutTop().withoutBottom());
      self->updateEffectiveInlineStyle();
      // After startNewTextBlock: it flushes this element's own last block, which belongs on the
      // page it is already on. The break applies to whatever comes next.
      if (breakAfterElement) self->pendingForcedBreak = true;
    }

    // </li> closes: if the bullet never got inline text (empty <li> or <li> with only
    // block children that were flushed), clear the flag so the next sibling doesn't
    // merge into this block.
    if (strcmp(name, "li") == 0) {
      self->listItemBulletOnly = false;
    }
  }
  if (strcmp(name, "body") == 0) {
    self->insideBody = false;
  }
  if (strcmp(name, "html") == 0) {
    self->htmlEnded_ = true;
  }
}

ChapterHtmlSlimParser::~ChapterHtmlSlimParser() { abortParse(); }

bool ChapterHtmlSlimParser::beginParse() {
  htmlEnded_ = false;
  // Initialize block style stack with a root entry representing "no ancestor block elements".
  // The user's paragraph alignment is set as the default so child elements without explicit
  // text-align inherit it correctly through getCombinedBlockStyle.
  BlockStyle rootBlockStyle;
  rootBlockStyle.alignment = (this->paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None))
                                 ? CssTextAlign::Justify
                                 : static_cast<CssTextAlign>(this->paragraphAlignment);
  blockStyleStack.clear();
  blockStyleStack.reserve(8);
  blockStyleStack.push_back(rootBlockStyle);

  tableDepth = 0;
  insideTableCell = false;
  tableRowStacked = false;
  tableRowsSpannedRemaining = 0;
  tableCellTextBytes = 0;
  tableRowCells.clear();
  for (auto& lines : tableCellLines) {
    lines.clear();
  }
  tableLineVisibleOffsets.clear();

  auto paragraphAlignmentBlockStyle = BlockStyle();
  paragraphAlignmentBlockStyle.textAlignDefined = true;
  const auto align = rootBlockStyle.alignment;
  paragraphAlignmentBlockStyle.alignment = align;
  startNewTextBlock(paragraphAlignmentBlockStyle);

  xmlParser_ = XML_ParserCreate(nullptr);
  if (!xmlParser_) {
    LOG_ERR("EHP", "Couldn't allocate memory for parser");
    return false;
  }

  // Handle HTML entities (like &nbsp;) that aren't in XML spec or DTD
  // Using DefaultHandlerExpand preserves normal entity expansion from DOCTYPE
  XML_SetDefaultHandlerExpand(xmlParser_, defaultHandlerExpand);

  if (!Storage.openFileForRead("EHP", filepath, parseFile_)) {
    destroyXmlParser(xmlParser_);
    xmlParser_ = nullptr;
    return false;
  }

  // Get file size to decide whether to show indexing popup.
  if (popupFn && parseFile_.size() >= MIN_SIZE_FOR_POPUP) {
    popupFn();
  }

  XML_SetUserData(xmlParser_, this);
  XML_SetElementHandler(xmlParser_, startElement, endElement);
  XML_SetCharacterDataHandler(xmlParser_, characterData);

  parseStartTime_ = millis();
  return true;
}

ChapterHtmlSlimParser::ParseStatus ChapterHtmlSlimParser::parseStep() {
  void* const buf = XML_GetBuffer(xmlParser_, PARSE_BUFFER_SIZE);
  if (!buf) {
    LOG_ERR("EHP", "Couldn't allocate memory for buffer");
    return ParseStatus::Error;
  }

  const size_t len = parseFile_.read(buf, PARSE_BUFFER_SIZE);

  if (len == 0 && parseFile_.available() > 0) {
    LOG_ERR("EHP", "File read error");
    return ParseStatus::Error;
  }

  const int done = parseFile_.available() == 0;

  if (XML_ParseBuffer(xmlParser_, static_cast<int>(len), done) == XML_STATUS_ERROR) {
    if (htmlEnded_) {
      LOG_DBG("EHP", "Ignoring trailing data after </html>: %s", XML_ErrorString(XML_GetErrorCode(xmlParser_)));
      return ParseStatus::Done;
    }
    LOG_ERR("EHP", "Parse error at line %lu:\n%s", XML_GetCurrentLineNumber(xmlParser_),
            XML_ErrorString(XML_GetErrorCode(xmlParser_)));
    return ParseStatus::Error;
  }

  return done ? ParseStatus::Done : ParseStatus::More;
}

void ChapterHtmlSlimParser::abortParse() {
  if (xmlParser_) {
    destroyXmlParser(xmlParser_);
    xmlParser_ = nullptr;
  }
  // Only close the file if it was successfully opened in beginParse()
  if (parseFile_.isOpen()) {
    parseFile_.close();
  }
}

bool ChapterHtmlSlimParser::finishParse() {
  if (xmlParser_) {
    LOG_DBG("EHP", "Time to parse and build pages: %lu ms", millis() - parseStartTime_);
    destroyXmlParser(xmlParser_);
    xmlParser_ = nullptr;
  }
  parseFile_.close();

  // Process last page if there is still text
  if (currentTextBlock) {
    makePages();
    if (!pendingAnchorId.empty()) {
      anchorData.push_back({std::move(pendingAnchorId), static_cast<uint16_t>(completedPageCount)});
      pendingAnchorId.clear();
    }
    if (currentPage && !currentPage->elements.empty()) {
      setCurrentPageVisibleOffset(visibleTextOffset);
      completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex, currentPageVisibleOffset);
      completedPageCount++;
    }
    currentPage.reset();
    currentTextBlock.reset();
  }

  return true;
}

bool ChapterHtmlSlimParser::parseAndBuildPages() {
  if (!beginParse()) {
    return false;
  }
  for (;;) {
    const ParseStatus status = parseStep();
    if (status == ParseStatus::Error) {
      abortParse();
      return false;
    }
    if (status == ParseStatus::Done) {
      break;
    }
  }
  return finishParse();
}

void ChapterHtmlSlimParser::addLineToPage(std::shared_ptr<TextBlock> line, const uint32_t visibleOffset) {
  // Furigana renders ABOVE the line's ascender, so a ruby-carrying line needs extra leading or
  // the annotation overlaps the line above (and clips at the page top). That headroom goes into
  // the line's HEIGHT only -- deliberately not into its y.
  //
  // TextBlock::render() already shifts every word down by the same getRubyShift(ascender) before
  // drawing, which is what creates the room the ruby is then raised into. Adding it to y here as
  // well applied it twice: the line's text landed at nextY + 2*rubyExtra while the following line
  // sat at nextY + base + rubyExtra, so the gap collapsed to base - rubyExtra. Measured on device
  // as ~0.62 of normal leading, with descenders of the ruby line visibly colliding with the
  // ascenders of the next -- the long-standing "horizontal line spacing is too tight" report,
  // which only ever affected lines carrying furigana.
  // A block with a CSS font-size was measured and will be drawn with its own font, so its
  // leading has to come from that font too -- otherwise an 18pt heading gets 14pt of vertical
  // room and collides with the line below it.
  // A CSS line-height scales the leading that the block's font and the user's Line Spacing
  // setting produced (see cssLineHeightPercent: it is a clamped percentage OF that value, so
  // the user's setting stays inside the result). It is applied BEFORE the ruby headroom, which
  // is not part of the book's line box -- it is room the annotation needs above the ascender.
  // ...and none of it is needed when the annotation will not be drawn: with furigana off the
  // headroom is empty and the page reads looser than it should. Vertical tightens for the same
  // reason (VerticalSection::streamParseAndLayout keeps a second, narrower column-gap table).
  // TextBlock::render() drops its matching shift on the same condition.
  const int lineFontId = line->getBlockStyle().resolveFontId(fontId);
  const int rubyExtra = furiganaEnabled ? line->getRubyShift(renderer.getFontAscenderSize(lineFontId)) : 0;
  // A word enlarged by an inline font-size (span) needs the line's advance to cover its taller
  // glyphs, or it collides with the next line. Only lines that actually carry per-word fonts
  // pay the scan; the base font for the leading stays the block's own.
  int advanceFontId = lineFontId;
  if (line->hasWordFonts()) {
    int tallest = renderer.getLineHeight(lineFontId, lineCompression);
    for (uint16_t w = 0; w < line->wordCount(); ++w) {
      const int32_t wf = line->wordFont(w);
      if (wf == 0 || wf == advanceFontId) continue;
      const int h = renderer.getLineHeight(wf, lineCompression);
      if (h > tallest) {
        tallest = h;
        advanceFontId = wf;
      }
    }
  }
  const int leading =
      applyCssLineHeight(renderer.getLineHeight(advanceFontId, lineCompression), line->getBlockStyle().lineHeightPct);
  const int lineHeight = leading + rubyExtra;

  // Keep-together buffering (page-break-inside/after: avoid): hold the line instead of placing
  // it, so the block can still be moved to the next page as a unit. Buffering is abandoned the
  // moment the block cannot fit on a page by itself -- from there it is laid out and split
  // exactly as an unstyled block would be, which is what guarantees the layout still terminates.
  if (keepingBlockTogether) {
    if (keepBufferHeight + lineHeight <= viewportHeight && keepBuffer.size() < KEEP_MAX_LINES) {
      keepBufferHeight = static_cast<int16_t>(keepBufferHeight + lineHeight);
      keepBuffer.push_back({std::move(line), visibleOffset});
      return;
    }
    flushKeepBuffer();
  }

  if (!currentPage) {
    currentPage.reset(new Page());
    currentPageNextY = 0;
    currentPageVisibleOffsetSet = false;
  }

  if (currentPageNextY + lineHeight > viewportHeight) {
    if (currentPage->elements.empty()) {
      // Nothing on this page to leave behind, so a boundary here would emit a BLANK page and put
      // the line at y = 0 on the next one regardless. Do that directly: drop the pending top
      // spacing (which is all that can push a line off an otherwise empty page, unless the line
      // is simply taller than the viewport) and keep the line here.
      currentPageNextY = 0;
    } else {
      breakPage();
      if (!currentPage) return;
    }
  }
  setCurrentPageVisibleOffset(visibleOffset);

  // First laid-out line inside an open box: its y anchors the box rect's top edge.
  if (boxDepth >= 0 && boxAwaitingFirstLine) {
    boxStartY = currentPageNextY;
    boxAwaitingFirstLine = false;
  }

  // Track cumulative words to assign footnotes to the page containing their anchor
  wordsExtractedInBlock += line->wordCount();
  auto footnoteIt = pendingFootnotes.begin();
  while (footnoteIt != pendingFootnotes.end() && footnoteIt->first <= wordsExtractedInBlock) {
    currentPage->addFootnote(footnoteIt->second.number, footnoteIt->second.href.c_str());
    if (sectionFootnoteData.size() < MAX_SECTION_FOOTNOTES) {
      sectionFootnoteData.push_back({static_cast<uint16_t>(completedPageCount), footnoteIt->second});
    }
    ++footnoteIt;
  }
  pendingFootnotes.erase(pendingFootnotes.begin(), footnoteIt);

  // Panel BEFORE the line: page elements render in insertion order, so the fill has to be pushed
  // first or it would paint over the text it is meant to sit behind.
  if (line->getBlockStyle().isInverted()) {
    emitInvertedPanel(line->getBlockStyle(), static_cast<int16_t>(lineHeight));
  }

  // Apply horizontal left inset (margin + padding) as x position offset
  const int16_t xOffset = line->getBlockStyle().leftInset();
  const int rubyShift = line->getRubyShift(renderer.getFontAscenderSize(fontId));
  const int baseLineHeight = renderer.getLineHeight(fontId, lineCompression);
  for (const auto& link : line->takeLinkSpans()) {
    if (!currentPage->addLink(link.href, static_cast<int16_t>(xOffset + link.x),
                              static_cast<int16_t>(currentPageNextY + rubyShift - link.topLift), link.width,
                              static_cast<int16_t>(baseLineHeight + link.topLift))) {
      LOG_DBG("EHP", "Dropped page link: %.48s", link.href);
    }
  }
  currentPage->elements.push_back(std::make_shared<PageLine>(line, xOffset, currentPageNextY));
  currentPageNextY += lineHeight;
}

void ChapterHtmlSlimParser::makePages() {
  if (!currentTextBlock) {
    LOG_ERR("EHP", "!! No text block to make pages for !!");
    return;
  }

  // A pending `page-break-before/after: always` lands here, ahead of the block's own top spacing
  // (a block that starts a page has no margin above it). It waits for a block with actual content
  // so the break cannot be spent on an empty wrapper and leave the real content mid-page.
  if (pendingForcedBreak && !currentTextBlock->isEmpty()) {
    pendingForcedBreak = false;
    breakPage();
  }

  if (!currentPage) {
    currentPage.reset(new Page());
    currentPageNextY = 0;
    currentPageVisibleOffsetSet = false;
  }

  // Apply top spacing before the paragraph (stored in pixels)
  const BlockStyle& blockStyle = currentTextBlock->getBlockStyle();
  // Paragraph spacing follows the block's own font AND its own line-height, like its line
  // height does: a heading led at 0.83 gets a proportionally tighter half-line after it.
  const int lineHeight = applyCssLineHeight(renderer.getLineHeight(blockStyle.resolveFontId(fontId), lineCompression),
                                            blockStyle.lineHeightPct);
  if (blockStyle.marginTop > 0) {
    currentPageNextY += blockStyle.marginTop;
  }
  if (blockStyle.paddingTop > 0) {
    currentPageNextY += blockStyle.paddingTop;
  }

  // Calculate effective width accounting for horizontal margins/padding
  const int horizontalInset = blockStyle.totalHorizontalInset();
  const uint16_t effectiveWidth =
      (horizontalInset < viewportWidth) ? static_cast<uint16_t>(viewportWidth - horizontalInset) : viewportWidth;

  // Hand the block's top padding to its first panel line: currentPageNextY has already advanced
  // past it, so without this the panel would start below the padding it is meant to fill.
  if (blockStyle.isInverted()) {
    pendingPanelTopPad = std::max<int16_t>(0, blockStyle.paddingTop);
    lastPanelBox = nullptr;
  }

  // page-break-inside/after: avoid -- buffer the lines, then decide (see beginKeepTogether).
  beginKeepTogether(blockStyle);

  currentTextBlock->layoutAndExtractLines(
      renderer, fontId, effectiveWidth,
      [this](const std::shared_ptr<TextBlock>& textBlock, const uint32_t offset) { addLineToPage(textBlock, offset); });

  // Before the panel stitching below: the buffered lines are only placed now, and it is their
  // placement that sets lastPanelBox.
  finishKeepTogether();

  // ... and its bottom padding to the last one, which is only identifiable now the block is laid
  // out. The last line always sits on the still-open page (a page is completed on the NEXT line
  // that does not fit), so the box is still both alive and unwritten.
  if (blockStyle.isInverted()) {
    if (lastPanelBox && blockStyle.paddingBottom > 0) {
      lastPanelBox->extendHeight(std::min<int16_t>(
          blockStyle.paddingBottom, static_cast<int16_t>(std::max(0, viewportHeight - currentPageNextY))));
    }
    pendingPanelTopPad = 0;
    lastPanelBox = nullptr;
  }

  // Fallback: transfer any remaining pending footnotes to current page.
  // Normally addLineToPage handles this via word-index tracking, but this catches
  // edge cases where a footnote's word index equals the exact block size.
  if (!pendingFootnotes.empty() && currentPage) {
    for (const auto& [idx, fn] : pendingFootnotes) {
      currentPage->addFootnote(fn.number, fn.href.c_str());
      if (sectionFootnoteData.size() < MAX_SECTION_FOOTNOTES) {
        sectionFootnoteData.push_back({static_cast<uint16_t>(completedPageCount), fn});
      }
    }
    pendingFootnotes.clear();
  }

  // Apply bottom spacing after the paragraph (stored in pixels)
  if (blockStyle.marginBottom > 0) {
    currentPageNextY += blockStyle.marginBottom;
  }
  if (blockStyle.paddingBottom > 0) {
    currentPageNextY += blockStyle.paddingBottom;
  }

  // Extra paragraph spacing if enabled (default behavior)
  if (extraParagraphSpacing) {
    currentPageNextY += lineHeight / 2;
  }
}
