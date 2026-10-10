#pragma once

#include <EpdFontFamily.h>

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "WordStore.h"
#include "blocks/BlockStyle.h"
#include "blocks/TextBlock.h"

class GfxRenderer;

class ParsedText {
  // Word text lives in wordStore (chunked bump arena, NUL-terminated entries);
  // words holds 8-byte handles into it. This replaces the former
  // std::deque<std::string>: per-word string objects, their SSO spills, and
  // every hyphenation/NFC temporary were the layout path's dominant
  // small-allocation churn, and any failed implicit allocation abort()s under
  // -fno-exceptions. Handles stay in a std::deque for the #2814 reason: no
  // large contiguous reallocation at CJK token counts (deque grows in fixed
  // ~512 B nodes). On arena OOM the word is dropped and hadDroppedWords()
  // latches so the section build can fail readably instead of aborting.
  // rubyTexts stays a deque of strings: ruby is rare and per-block small.
  // The per-token parallel arrays below stay vectors: 1 byte / 1 bit each,
  // they never approach the contiguous-block ceiling.
  WordStore wordStore;
  std::deque<WordStore::StoredWord> words;
  std::vector<EpdFontFamily::Style> wordStyles;
  // Boundary flags use all four combinations:
  //   false,false: ordinary gap; false,true: stretchable zero-width CJK gap;
  //   true,false: unbreakable attachment; true,true: breakable, non-stretching attachment.
  std::vector<bool> wordContinues;
  std::vector<bool> wordNoSpaceBefore;
  // Bytes [0, wordFocusBoundary) render bold; 0 means no Focus Reading emphasis.
  // Keeping the original word as one token lets hyphenation consider the whole word.
  std::vector<uint8_t> wordFocusBoundary;
  // Per-word font id from an inline font-size (span); 0 = the block's font. Lazily
  // materialized like rubyTexts: empty means "no word in this block has one", so the
  // common case (no sized spans) pays nothing. Once non-empty it is kept in lockstep
  // with words[] through every push/insert/erase.
  std::vector<int32_t> wordFonts;
  // Internal-link identity through tokenization, hyphenation and BiDi reorder.
  // Zero means plain text; non-zero indexes linkTargets. Kept at one byte per
  // token and discarded after layout, never added to the page-cache TextBlock.
  std::vector<uint8_t> wordLinkIds;
  std::vector<std::string> linkTargets;
  // Zero-based visible Unicode-codepoint offsets in the spine body, stored as
  // uint16_t deltas from a shared base to keep this layout-only metadata small.
  // Pathological spans wider than uint16_t use sparse rebases; rendered
  // TextBlocks retain absolute ranges for portable clipping anchors.
  struct VisibleOffsetRebase {
    size_t wordIndex;
    uint32_t base;
  };
  std::vector<uint16_t> wordVisibleOffsetDeltas;
  uint32_t visibleOffsetBase = 0;
  std::vector<VisibleOffsetRebase> visibleOffsetRebases;
  // Sparse NFC source positions, in small nothrow chunks rather than a large contiguous array.
  struct AbsorbedSourceChunk {
    std::unique_ptr<AbsorbedSourceChunk> next;
    uint32_t base = 0;
    uint16_t deltas[64] = {};
    uint8_t begin = 0;
    uint8_t count = 0;
    ~AbsorbedSourceChunk() {
      while (next) {
        auto retired = std::move(next);
        next = std::move(retired->next);
      }
    }
  };
  std::unique_ptr<AbsorbedSourceChunk> absorbedSourceHead;
  AbsorbedSourceChunk* absorbedSourceTail = nullptr;
  mutable const AbsorbedSourceChunk* absorbedSourceCursor = nullptr;
  mutable uint32_t absorbedSourceCursorStart = 0;
  std::deque<std::string> rubyTexts;
  BlockStyle blockStyle;
  uint8_t wordSpacingPercent = 100;
  uint8_t paragraphIndentSpaces;
  bool hyphenationEnabled;
  bool focusReadingEnabled;
  bool isNaturalAlign;
  bool hasRtlWord;
  // The enlarged first letter, once prepareDropCap() has claimed it. Attached to the first
  // line this ParsedText emits and cleared with blockStyle.dropCapLines once the lines beside
  // it are out, so a soft flush mid-paragraph cannot start a second one.
  TextBlock::DropCap dropCap;
  bool dropCapResolved = false;
  // Set once the lines beside the enlarged letter are out. The paragraph tail then re-enters
  // line breaking as break index 0, which is otherwise how "this is the paragraph's first
  // line" is spelled -- without this it would take the first-line text-indent a second time,
  // several lines into the paragraph.
  bool dropCapLinesEmitted = false;
  // True once any word was dropped because the text arena could not allocate.
  bool droppedWords = false;
  bool firstLineConsumed = false;
  std::vector<std::string> reorderedWordsScratch;
  std::vector<EpdFontFamily::Style> reorderedStylesScratch;
  std::vector<int32_t> reorderedFontsScratch;
  std::vector<uint16_t> reorderedWidthsScratch;
  std::vector<bool> reorderedContinuesScratch;
  std::vector<bool> reorderedNoSpaceBeforeScratch;
  std::vector<uint8_t> reorderedFocusBoundaryScratch;
  std::vector<uint16_t> visualOrderScratch;

  bool storeWord(std::string_view text, WordStore::StoredWord& out);
  uint32_t visibleOffsetBaseAt(size_t wordIndex) const;
  uint32_t visibleOffsetAt(size_t wordIndex) const;
  uint32_t sourceOffsetAfter(uint32_t start, uint32_t renderedLength) const;
  bool recordAbsorbedSourceOffset(uint32_t offset);
  void retireAbsorbedSourceOffsets(uint32_t remainingStart);
  void pushVisibleOffset(uint32_t offset);
  void insertVisibleOffset(size_t wordIndex, uint32_t offset);
  void eraseVisibleOffsetPrefix(size_t count);
  // Drop ONE word's offset entry, keeping every other word's absolute offset. Erasing the drop
  // cap's token is not always a prefix erase: a paragraph can open with punctuation before the
  // initial (`--<nbsp><span class="let">L</span>`, the French dialogue opening).
  void eraseVisibleOffsetAt(size_t wordIndex);
  int calculateRubyExtraStartOffset(size_t wordIdx, size_t maxWordIdx, const GfxRenderer& renderer, int fontId) const;
  int calculateRubyExtraEndOffset(size_t lineStartIdx, size_t lineBreakIdx, const GfxRenderer& renderer,
                                  int fontId) const;
  int resolveFirstLineIndent(bool isFirstLine, const GfxRenderer& renderer, int fontId) const;
  // The leading-edge indent of line `lineIndex` of the CURRENT extraction pass. The single
  // source of both the width the line is broken to and the x its words start at, so the two
  // cannot disagree. Beside a drop cap this is the reserved column; otherwise it is the
  // paragraph's first-line text-indent, exactly as before.
  int resolveLineIndent(size_t lineIndex, const GfxRenderer& renderer, int fontId) const;
  // Chooses the glyph, magnification and column width for a `::first-letter` drop cap and
  // removes the letter from the text flow. Returns false (leaving the text untouched, so the
  // letter simply renders inline) when the paragraph cannot carry one.
  bool prepareDropCap(const GfxRenderer& renderer, int fontId, int pageWidth, float lineCompression);
  // Word holding the drop cap's letter. 0 for a `::first-letter` rule, which by definition
  // styles the paragraph's first character; an enlarged span names its own word, because the
  // punctuation a paragraph opens with is tokenized ahead of it.
  uint8_t dropCapWordIndex = 0;
  // Greedy line breaks for the lines beside the drop cap. See the implementation for why these
  // do not go through computeLineBreaks' optimal DP.
  std::vector<size_t> computeDropCapLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                               const std::vector<uint16_t>& wordWidths,
                                               const std::vector<bool>& continuesVec,
                                               const std::vector<bool>& noSpaceBeforeVec) const;
  std::vector<size_t> computeLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                        std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                                        std::vector<bool>& noSpaceBeforeVec);
  std::vector<size_t> computeHyphenatedLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                                  std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                                                  std::vector<bool>& noSpaceBeforeVec);
  bool hyphenateWordAtIndex(size_t wordIndex, int availableWidth, const GfxRenderer& renderer, int fontId,
                            std::vector<uint16_t>& wordWidths, bool allowFallbackBreaks);
  void extractLine(size_t breakIndex, int pageWidth, const std::vector<uint16_t>& wordWidths,
                   const std::vector<bool>& continuesVec, const std::vector<bool>& noSpaceBeforeVec,
                   const std::vector<size_t>& lineBreakIndices,
                   const std::function<void(std::unique_ptr<TextBlock>, uint32_t)>& processLine,
                   const GfxRenderer& renderer, int fontId);
  std::vector<uint16_t> calculateWordWidths(const GfxRenderer& renderer, int fontId);
  // Drop the first `consumed` tokens, keeping every parallel per-word array in lockstep.
  void consumeWords(size_t consumed);

 public:
  explicit ParsedText(const bool hyphenationEnabled = false, const bool focusReadingEnabled = false,
                      const BlockStyle& blockStyle = BlockStyle(), const uint8_t paragraphIndentSpaces = 2)
      : blockStyle(blockStyle),
        paragraphIndentSpaces(paragraphIndentSpaces),
        hyphenationEnabled(hyphenationEnabled),
        focusReadingEnabled(focusReadingEnabled),
        isNaturalAlign(false),
        hasRtlWord(false),
        dropCapResolved(false) {}
  ~ParsedText() = default;

  // wordFontId: per-word font override from an inline font-size; 0 keeps the block's font.
  void addWord(std::string word, EpdFontFamily::Style fontStyle, bool underline = false, bool attachToPrevious = false,
               int32_t wordFontId = 0, uint32_t visibleTextOffset = 0, uint8_t linkId = 0);
  // The font a word measures and draws with (block font unless an inline font-size overrode it).
  int effectiveWordFont(size_t index, int blockFontId) const {
    const int32_t slot = index < wordFonts.size() ? wordFonts[index] : 0;
    return (slot != 0 && !TextBlock::isWordScaleTag(slot)) ? slot : blockFontId;
  }
  // Bitmap scale for a word whose size the font ladder could not serve; 256 = unscaled. Widths
  // MUST be scaled by it wherever they are measured -- the drawn word is scaled, and a width
  // measured unscaled would put the next word straight through it.
  uint16_t effectiveWordScale(const size_t index) const {
    const int32_t slot = index < wordFonts.size() ? wordFonts[index] : 0;
    return TextBlock::isWordScaleTag(slot) ? static_cast<uint16_t>(-slot) : TextBlock::WORD_SCALE_ONE;
  }
  uint8_t addLinkTarget(const char* href);
  bool linkTargetMatches(uint8_t linkId, const char* href) const;
  void setRubyForWordAt(size_t index, const std::string& ruby);
  void setRubyGroupAt(size_t startIndex, size_t count, const std::string& ruby);
  EpdFontFamily::Style getWordStyleAt(size_t index) const {
    return index < wordStyles.size()
               ? static_cast<EpdFontFamily::Style>(wordStyles[index] & ~TextBlock::DISCRETIONARY_HYPHEN_FLAG)
               : EpdFontFamily::REGULAR;
  }
  // Caller must check !isEmpty() first (used by the furigana-glossary harvest to pair a
  // just-closed <rt> reading with its base word).
  std::string_view lastWord() const { return wordStore.view(words.back()); }
  // Base text behind a ruby group, for the same harvest (a group ruby spans several words).
  std::string_view wordAt(const size_t i) const { return wordStore.view(words[i]); }
  // Delegates so the sizing rule lives in ONE place: ensureRubyCapacity() only reserve()s, it
  // does not resize, so writing rubyTexts.back() while the vector is still empty (no ruby seen
  // in this block yet) wrote past the end -- device crash: Store access fault inside
  // std::string::operator= during an incremental horizontal build, reached from the
  // text-emphasis (bouten) path in flushPartWordBuffer.
  void setLastWordRuby(const std::string& ruby) {
    if (words.empty()) return;
    setRubyForWordAt(words.size() - 1, ruby);
  }
  std::string getRubyTextAt(size_t index) const { return index < rubyTexts.size() ? rubyTexts[index] : std::string(); }
  void ensureRubyCapacity();
  void setBlockStyle(const BlockStyle& blockStyle) { this->blockStyle = blockStyle; }
  void setDropCapWordIndex(const uint8_t wordIndex) { dropCapWordIndex = wordIndex; }
  BlockStyle& getBlockStyle() { return blockStyle; }
  size_t size() const { return words.size(); }
  bool isEmpty() const { return words.empty(); }
  // True once any word was dropped because the text arena could not allocate.
  // Callers must treat the block as incomplete and fail the section build.
  bool hadDroppedWords() const { return droppedWords; }
  // baseFontId is the reader's font; a block with a CSS font-size lays out (and later draws)
  // with BlockStyle::resolveFontId(baseFontId) instead. processLine receives each line and the
  // visible-codepoint offset of its first word (content-based positions, upstream #2805).
  // lineCompression is the reader's line-spacing factor, the same one the emitted lines' own
  // advance is computed with: the drop cap is sized to the height of the lines it must sit
  // beside, so measuring it against the uncompressed leading would let it run into them.
  //
  // The line is handed over as a unique_ptr (upstream #3518): every consumer takes sole
  // ownership, and dropping the shared_ptr control block is one less allocation per line on a
  // heap where fragmentation is the binding constraint.
  void layoutAndExtractLines(const GfxRenderer& renderer, int baseFontId, uint16_t viewportWidth,
                             const std::function<void(std::unique_ptr<TextBlock>, uint32_t)>& processLine,
                             bool includeLastLine = true, float lineCompression = 1.0f, int8_t characterSpacing = 0,
                             uint8_t wordSpacingPercent = 100);
};
