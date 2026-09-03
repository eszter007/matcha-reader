#pragma once

#include <HalStorage.h>
#include <expat.h>

#include <array>
#include <climits>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Epub/FootnoteEntry.h"
#include "Epub/ParsedText.h"
#include "Epub/RubyGlossary.h"
#include "Epub/blocks/ImageBlock.h"
#include "Epub/blocks/TextBlock.h"
#include "Epub/css/CssParser.h"
#include "Epub/css/CssStyle.h"

class Page;
class PageBox;
class GfxRenderer;
class Epub;

#define MAX_WORD_SIZE 200

class ChapterHtmlSlimParser {
  std::shared_ptr<Epub> epub;
  const std::string& filepath;
  GfxRenderer& renderer;
  std::function<void(std::unique_ptr<Page>, uint16_t, uint16_t, uint32_t)> completePageFn;
  std::function<void()> popupFn;  // Popup callback
  bool imagePopupFired = false;   // popupFn fired for the first image probe (single-shot)
  int depth = 0;
  int skipUntilDepth = INT_MAX;
  int boldUntilDepth = INT_MAX;
  int italicUntilDepth = INT_MAX;
  // buffer for building up words from characters, will auto break if longer than this
  // leave one char at end for null pointer
  char partWordBuffer[MAX_WORD_SIZE + 1] = {};
  int partWordBufferIndex = 0;
  bool nextWordContinues = false;  // true when next flushed word attaches to previous (inline element boundary)
  // French verb-subject inversion splitting (flushPartWordBuffer): -1 = not yet computed,
  // 0 = no, 1 = yes. Cached lazily so epub->getLanguage() is looked up once per chapter
  // instead of once per flushed word.
  int8_t frenchBookCache = -1;
  std::unique_ptr<ParsedText> currentTextBlock = nullptr;
  // Ruby text state
  bool inRuby = false;
  int rubyStartWordIndex = -1;
  bool collectingRubyText = false;
  std::string rubyTextBuffer;
  std::unique_ptr<Page> currentPage = nullptr;
  int16_t currentPageNextY = 0;

  // Bordered-block tracking. Full boxes and partial top/bottom separators are all emitted only
  // after their content has landed, so a page break cannot strand a rule on the preceding page.
  // The packed spec retains edge mask, dotted/dashed/solid style and 1..4px width.
  int boxDepth = -1;
  uint8_t boxBorderSpec = 0;
  int16_t boxStartY = 0;
  int16_t boxEmptyAdvance = 0;
  size_t boxFirstElementIndex = 0;
  bool boxShrinkToContent = false;
  bool boxContinued = false;          // continued from the previous page: omit the top edge
  bool boxAwaitingFirstLine = false;  // capture boxStartY from the first line the box lays out
  // Inverted-block panel tracking (CSS color/background-color, see CssInkMode). Each line of an
  // inverted block gets a filled PageBox pushed just before it, so the panel survives a page break
  // and needs no knowledge of the block's total height. Only the block's own padding needs
  // stitching on: the top pad onto the first line, the bottom pad onto the last.
  int16_t pendingPanelTopPad = 0;                   // consumed by the first panel line of the block
  std::shared_ptr<PageBox> lastPanelBox = nullptr;  // grown by the bottom pad once the block closes
  void emitInvertedPanel(const BlockStyle& blockStyle, int16_t lineHeight);

  // --- CSS page-break control (see BlockStyle::pageBreaks) -------------------------------------
  //
  // Every page boundary in this parser goes through breakPage(), which refuses to flush a page
  // with no elements on it. That single rule is what makes the break properties safe: no
  // combination of them can emit a blank page, and no block can bounce between pages, because a
  // break only ever happens when there is content to leave behind.
  //
  // `always` is raised as a ONE-SHOT pending break by the element that declares it (open for
  // -before, close for -after) rather than read from a block style: a container's accumulated
  // style is reused for every text block it opens, so reading it per block would break a page
  // before every paragraph in the container instead of once.
  bool pendingForcedBreak = false;
  // `inside: avoid` / `after: avoid`: the block's lines are buffered instead of being placed, so
  // the block can move to the next page as a UNIT once its total height is known (streaming
  // layout means the height is not knowable before the last line). Buffering stops the moment
  // the block exceeds what a page can hold, so the buffer is bounded by one viewport height and
  // an over-long block is simply split, as it would have been without the property.
  static constexpr size_t KEEP_MAX_LINES = 64;
  bool keepingBlockTogether = false;
  int16_t keepBufferHeight = 0;     // sum of the buffered lines' heights
  int16_t keepWithNextReserve = 0;  // room to leave after the block for `after: avoid`
  // Buffered (line, first-word visible offset) pairs; the offset replays through
  // addLineToPage so content positions survive the keep-together delay.
  std::vector<std::pair<std::shared_ptr<TextBlock>, uint32_t>> keepBuffer;
  void breakPage();
  void beginKeepTogether(const BlockStyle& blockStyle);
  void finishKeepTogether();
  void flushKeepBuffer();

  void flushPendingBlockLayout();
  void emitBoxRect(bool openBottom);
  void maybeEmitOpenBoxForPageBreak();
  void closeBoxBlock();
  int fontId;
  float lineCompression;
  bool extraParagraphSpacing;
  uint8_t paragraphAlignment;
  uint16_t viewportWidth;
  uint16_t viewportHeight;
  bool hyphenationEnabled;
  bool focusReadingEnabled;
  // Reserve the leading a ruby annotation needs above the ascender only when it will be drawn.
  // See ReaderRenderSpec::furiganaEnabled.
  bool furiganaEnabled;
  const CssParser* cssParser;
  // Chain of open elements, for descendant (`.callout p`) and child (`div > p`) selectors.
  // Pushed at the top of startElement and popped at the top of endElement -- one push per
  // start tag and one pop per end tag, independent of the tag-specific branches below (which
  // maintain `depth` on their own paths). Fixed 12-entry inline storage, no heap: see
  // CssElementPath.
  CssElementPath cssPath;
  bool embeddedStyle;
  // "Book side margins" setting: honor the book's horizontal CSS margins/padding
  // (clamped per element) instead of zeroing them. See BlockStyle::fromCssStyle.
  bool honorBookInsets;
  uint8_t imageRendering;
  std::string contentBase;
  std::string imageBasePath;
  int imageCounter = 0;

  // Style tracking (replaces depth-based approach)
  struct StyleStackEntry {
    int depth = 0;
    bool hasBold = false, bold = false;
    bool hasItalic = false, italic = false;
    bool hasTextDecoration = false;
    CssTextDecoration textDecoration = CssTextDecoration::None;
    bool hasDirection = false;
    CssTextDirection direction = CssTextDirection::Ltr;
    bool setsParagraphDirection = false;
    bool hasTextAlign = false;
    CssTextAlign textAlign = CssTextAlign::Left;
    bool hasSup = false, sup = false;
    bool hasSub = false, sub = false;
    bool hasEmphasis = false;
    CssTextEmphasis emphasis = CssTextEmphasis::None;
    bool hasSmallCaps = false, smallCaps = false;
    bool hasTextTransform = false;
    CssTextTransform textTransform = CssTextTransform::None;
    // Inline font-size (span): absolute font id resolved at push time via cssBlockFontId.
    // 0 with hasFontId set means "explicitly the block's own font" (a nested reset).
    bool hasFontId = false;
    int32_t fontIdOverride = 0;
  };
  std::vector<StyleStackEntry> inlineStyleStack;
  std::vector<BlockStyle> blockStyleStack;  // accumulated block styles from open ancestor elements
  CssStyle currentCssStyle;
  bool effectiveBold = false;
  bool effectiveItalic = false;
  CssTextDecoration effectiveTextDecoration = CssTextDecoration::None;
  bool effectiveDirectionDefined = false;
  CssTextDirection effectiveDirection = CssTextDirection::Ltr;
  bool effectiveTextAlignDefined = false;
  CssTextAlign effectiveTextAlign = CssTextAlign::Left;
  bool effectiveSup = false;
  bool effectiveSub = false;
  // Active text-emphasis mark (JP bouten) -- rendered as synthetic per-glyph ruby.
  CssTextEmphasis effectiveEmphasis = CssTextEmphasis::None;
  // font-variant: small-caps -- approximated by uppercasing (no per-word size support).
  bool effectiveSmallCaps = false;
  CssTextTransform effectiveTextTransform = CssTextTransform::None;
  // Per-word font from an inline font-size (see StyleStackEntry::fontIdOverride); 0 = block font.
  int32_t effectiveWordFontId = 0;

  // Ordered/unordered list nesting for list-style-type markers. Fixed-depth
  // stack: nesting past kMaxListDepth reuses the innermost tracked context.
  struct ListCtx {
    uint16_t counter = 0;
    CssListStyleType type = CssListStyleType::Disc;
  };
  static constexpr int kMaxListDepth = 8;
  ListCtx listStack[kMaxListDepth];
  int listDepth = 0;

  static constexpr size_t MAX_GRID_TABLE_COLUMNS = 4;
  static constexpr size_t MAX_GRID_TABLE_CELL_WORDS = 32;
  static constexpr size_t MAX_GRID_TABLE_CELL_BYTES = 512;
  int tableDepth = 0;
  bool insideTableCell = false;
  bool tableRowStacked = false;
  bool tableRowRtl = false;
  uint16_t tableRowsSpannedRemaining = 0;
  size_t tableCellTextBytes = 0;
  std::vector<std::unique_ptr<ParsedText>> tableRowCells;
  std::array<std::vector<std::shared_ptr<TextBlock>>, MAX_GRID_TABLE_COLUMNS> tableCellLines;
  std::vector<uint32_t> tableLineVisibleOffsets;
  bool listItemBulletOnly = false;  // true when currentTextBlock has only the <li> bullet

  // Anchor-to-page mapping: tracks which page each HTML id attribute lands on
  int completedPageCount = 0;
  static constexpr size_t MAX_SECTION_FOOTNOTES = 128;
  std::vector<std::pair<uint16_t, FootnoteEntry>> sectionFootnoteData;
  std::vector<std::pair<std::string, uint16_t>> anchorData;
  std::string pendingAnchorId;          // deferred until after previous text block is flushed
  std::vector<std::string> tocAnchors;  // the list of anchors that are TOC chapter boundaries
  uint16_t xpathParagraphIndex = 0;
  uint16_t xpathListItemIndex = 0;
  // Canonical reading-position counter: zero-based Unicode codepoints in visible
  // <body> text. Token offsets flow through line breaking so every completed page
  // records the first source character it renders.
  uint32_t visibleTextOffset = 0;
  uint32_t partWordVisibleOffset = 0;
  uint32_t currentPageVisibleOffset = 0;
  bool currentPageVisibleOffsetSet = false;
  bool insideBody = false;
  bool htmlEnded_ = false;
  bool syntheticCharacterData = false;
  uint16_t nonVisibleTextDepth = 0;

  // Whole-<ruby>-element accumulation for the glossary: mono-ruby elements
  // (小<rt>こ</rt>林<rt>ばやし</rt>) also record 小林 -> こばやし so whole-word
  // lookups match, not just the per-character pairs.
  std::string rubyElemBase;
  std::string rubyElemRuby;
  int rubyElemRunCount = 0;

 public:
  // Per-book furigana glossary harvest: unique (base, ruby) pairs seen during this parse,
  // merged into <cache>/ruby.bin by the section build after a successful parse (see
  // RubyGlossary). Bounded by RubyGlossary's per-section cap.
  std::vector<RubyGlossary::Pair> rubyHarvest;

 private:
  // Footnote link tracking
  bool insideFootnoteLink = false;
  int footnoteLinkDepth = -1;
  uint8_t currentFootnoteLinkId = 0;
  FootnoteEntry currentFootnote = {};
  int currentFootnoteLinkTextLen = 0;
  std::vector<std::pair<int, FootnoteEntry>> pendingFootnotes;  // <wordIndex, entry>
  int wordsExtractedInBlock = 0;

  // Resumable parse state. The one-shot parseAndBuildPages() drives these
  // internally; the incremental section builder drives them across render ticks
  // so a large single chapter can yield between pages instead of blocking the UI
  // until the whole thing is laid out. parseFile_ and the expat parser stay alive
  // for the lifetime of the parse so it can be paused and resumed at buffer
  // boundaries.
  XML_Parser xmlParser_ = nullptr;
  HalFile parseFile_;
  uint32_t parseStartTime_ = 0;

  void updateEffectiveInlineStyle();
  void startNewTextBlock(const BlockStyle& blockStyle);
  void flushPendingAnchor();
  void flushPartWordBuffer();
  void fallbackTableRowToStacked();
  void closeTableCell();
  void finishTableRow();
  void addTableRowSeparator();
  void setCurrentPageVisibleOffset(uint32_t offset);
  void makePages();
  static EpdFontFamily::Style fontStyleForTextDecoration(CssTextDecoration decoration);
  static void applyDirectionToEntry(StyleStackEntry& entry, const CssStyle& css);
  static void applyTextDecorationToEntry(StyleStackEntry& entry, const CssStyle& css);
  static void applyTextTransformToEntry(StyleStackEntry& entry, const CssStyle& css);
  static void applyVerticalAlignToEntry(StyleStackEntry& entry, const CssStyle& css);
  void pushTableTextStyleEntry(const CssStyle& cssStyle);
  void pushDecorationStyleEntry(CssTextDecoration defaultDecoration, const CssStyle& cssStyle);
  void emitHorizontalRule(const BlockStyle& blockStyle);
  // XML callbacks
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void XMLCALL characterData(void* userData, const XML_Char* s, int len);
  static void XMLCALL defaultHandlerExpand(void* userData, const XML_Char* s, int len);
  static void XMLCALL endElement(void* userData, const XML_Char* name);

 public:
  explicit ChapterHtmlSlimParser(
      std::shared_ptr<Epub> epub, const std::string& filepath, GfxRenderer& renderer, const int fontId,
      const float lineCompression, const bool extraParagraphSpacing, const uint8_t paragraphAlignment,
      const uint16_t viewportWidth, const uint16_t viewportHeight, const bool hyphenationEnabled,
      const bool focusReadingEnabled, const bool furiganaEnabled,
      const std::function<void(std::unique_ptr<Page>, uint16_t, uint16_t, uint32_t)>& completePageFn,
      const bool embeddedStyle, const std::string& contentBase, const std::string& imageBasePath,
      const uint8_t imageRendering = 0, std::vector<std::string> tocAnchors = {},
      const std::function<void()>& popupFn = nullptr, const CssParser* cssParser = nullptr,
      const bool honorBookInsets = false)

      : epub(epub),
        filepath(filepath),
        renderer(renderer),
        fontId(fontId),
        lineCompression(lineCompression),
        extraParagraphSpacing(extraParagraphSpacing),
        paragraphAlignment(paragraphAlignment),
        viewportWidth(viewportWidth),
        viewportHeight(viewportHeight),
        hyphenationEnabled(hyphenationEnabled),
        focusReadingEnabled(focusReadingEnabled),
        furiganaEnabled(furiganaEnabled),
        completePageFn(completePageFn),
        popupFn(popupFn),
        cssParser(cssParser),
        embeddedStyle(embeddedStyle),
        honorBookInsets(honorBookInsets),
        imageRendering(imageRendering),
        contentBase(contentBase),
        imageBasePath(imageBasePath),
        tocAnchors(std::move(tocAnchors)) {}

  ~ChapterHtmlSlimParser();

  // One-shot parse: builds every page before returning (begin + step* + finish).
  bool parseAndBuildPages();

  // Resumable parse, for the incremental section builder. Drive as:
  //   if (!beginParse()) fail;
  //   loop: switch (parseStep()) { More: keep going / yield; Done: finishParse(); Error: abortParse(); }
  // Pages are emitted via completePageFn as they complete during parseStep(), so
  // the caller can stop once enough pages are built and resume on a later tick.
  enum class ParseStatus { More, Done, Error };
  bool beginParse();
  ParseStatus parseStep();
  bool finishParse();  // flush the trailing page and tear down; returns true
  void abortParse();   // tear down without flushing (error / abandon)

  void addLineToPage(std::shared_ptr<TextBlock> line, uint32_t visibleOffset);
  const std::vector<std::pair<std::string, uint16_t>>& getAnchors() const { return anchorData; }
  // Every footnote reference in the section with the page it appears on (same page counter as
  // getAnchors), for the section-wide footnote table -- see Section::loadSectionFootnotes().
  const std::vector<std::pair<uint16_t, FootnoteEntry>>& getSectionFootnotes() const { return sectionFootnoteData; }

  // Byte progress of the in-flight parse, used to estimate a still-building section's total page
  // count (a giant single-spine book never fully lays out, so its real count is unknown). Valid
  // between beginParse() and finishParse()/abortParse().
  size_t parseBytesConsumed() { return parseFile_ ? parseFile_.position() : 0; }
  size_t parseTotalBytes() { return parseFile_ ? parseFile_.size() : 0; }
};
