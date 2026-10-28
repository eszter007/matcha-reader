#include "TextBlock.h"

#include <BidiUtils.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <MemoryManager.h>
#include <Serialization.h>
#include <Utf8.h>

#include <cstring>
#include <string_view>

#include "../../../../src/fontIds.h"
#include "../CjkBreaks.h"

size_t TextBlock::arenaSize(const uint16_t wordCount, const bool hasFocus, const bool hasFonts,
                            const uint16_t textBytes) {
  // Layout documented in TextBlock.h: source ranges, then the 32-bit array, then 16-bit, 8-bit, text.
  size_t size =
      static_cast<size_t>(wordCount) * (sizeof(SourceRange) + sizeof(uint16_t) + sizeof(int16_t) + sizeof(uint8_t));
  if (hasFocus) {
    size += static_cast<size_t>(wordCount) * (sizeof(uint16_t) + sizeof(uint8_t));
  }
  if (hasFonts) {
    size += static_cast<size_t>(wordCount) * sizeof(int32_t);
  }
  return size + textBytes;
}

void TextBlock::bindArenaPointers() {
  uint8_t* base = arena.get();
  const size_t wc = numWords;
  sourceRanges = reinterpret_cast<const SourceRange*>(base);
  base += wc * sizeof(SourceRange);
  size_t off = 0;
  if (fontsPresent) {
    // The int32s sit 4-byte aligned (source ranges keep the base aligned) and the
    // following 16-bit arrays stay even (wc * 4 is always even).
    wordFontArr = reinterpret_cast<const int32_t*>(base);
    off = wc * 4;
  }
  textOffArr = reinterpret_cast<const uint16_t*>(base + off);
  xposArr = reinterpret_cast<const int16_t*>(base + off + wc * 2);
  off += wc * 4;
  if (focusPresent) {
    focusSplitXArr = reinterpret_cast<const uint16_t*>(base + off);
    off += wc * 2;
  }
  stylesArr = base + off;
  off += wc;
  if (focusPresent) {
    focusBoundaryArr = base + off;
    off += wc;
  }
  textArr = reinterpret_cast<const char*>(base + off);
}

TextBlock::TextBlock(const std::vector<std::string>& words, const std::vector<int16_t>& wordXpos,
                     const std::vector<EpdFontFamily::Style>& wordStyles, const std::vector<uint8_t>& focusBoundary,
                     const std::vector<uint16_t>& focusSplitX, const BlockStyle& blockStyle,
                     std::vector<std::string> rubyTexts, const std::vector<int32_t>& wordFonts,
                     std::vector<LinkSpan> linkSpans, const std::vector<SourceRange>& ranges,
                     const uint16_t paragraphStartWord)
    : blockStyle(blockStyle),
      paragraphStartWord(paragraphStartWord),
      rubyTexts(std::move(rubyTexts)),
      linkSpans(std::move(linkSpans)) {
  // Same invariant as deserialize(): a block never holds an all-empty rubyTexts, so a
  // ruby-less line costs nothing beyond its arena. The layout engine hands one over for
  // every line it extracts, ruby or not; release it here rather than carrying it for the
  // block's lifetime. Move-assigning an empty vector frees the buffer (clear() would not).
  if (!hasRuby()) {
    this->rubyTexts = std::vector<std::string>{};
  }

  // Focus annotations and per-word fonts are optional: empty vectors mean no word in this
  // block has one. When present, they must be sized in lockstep with words[].
  const bool hasFocus = !focusBoundary.empty();
  const bool hasFonts = !wordFonts.empty();
  if ((!ranges.empty() && ranges.size() != words.size()) || words.size() != wordXpos.size() ||
      words.size() != wordStyles.size() || words.size() > 10000 ||
      (paragraphStartWord != UINT16_MAX && paragraphStartWord >= words.size()) ||
      (hasFocus && (words.size() != focusBoundary.size() || words.size() != focusSplitX.size())) ||
      (hasFonts && words.size() != wordFonts.size())) {
    LOG_ERR("TXB",
            "Construction failed: size mismatch (words=%u, xpos=%u, styles=%u, boundary=%u, splitX=%u, fonts=%u)",
            static_cast<uint32_t>(words.size()), static_cast<uint32_t>(wordXpos.size()),
            static_cast<uint32_t>(wordStyles.size()), static_cast<uint32_t>(focusBoundary.size()),
            static_cast<uint32_t>(focusSplitX.size()), static_cast<uint32_t>(wordFonts.size()));
    isValid = false;
    return;
  }

  numWords = static_cast<uint16_t>(words.size());
  focusPresent = hasFocus;
  fontsPresent = hasFonts;
  if (numWords == 0) {
    return;  // valid empty block, no arena
  }

  // Pass 1: total text size, one NUL per word. A line is at most a physical
  // row of the page, so uint16_t offsets are ample; reject anything larger.
  size_t totalText = 0;
  for (const auto& w : words) totalText += w.size() + 1;
  if (totalText > UINT16_MAX) {
    LOG_ERR("TXB", "Construction failed: text size %u exceeds arena limit", static_cast<uint32_t>(totalText));
    numWords = 0;
    focusPresent = false;
    isValid = false;
    return;
  }
  textBytes = static_cast<uint16_t>(totalText);

  const size_t size = arenaSize(numWords, focusPresent, fontsPresent, textBytes);
  arena = makeUniqueNoThrow<uint8_t[]>(size);
  if (!arena) {
    // Evict rebuildable caches (SD-font mini data, render glyph cache) and
    // retry once before declaring the line lost.
    freeink::MemoryManager::instance().ensureFree(size + 4 * 1024);
    arena = makeUniqueNoThrow<uint8_t[]>(size);
  }
  if (!arena) {
    LOG_ERR("TXB", "OOM: arena %u bytes", static_cast<uint32_t>(size));
    numWords = 0;
    textBytes = 0;
    focusPresent = false;
    fontsPresent = false;
    isValid = false;
    return;
  }
  bindArenaPointers();

  // Pass 2: fill. Mutable aliases of the const views bound above.
  auto* textOff = const_cast<uint16_t*>(textOffArr);
  auto* xpos = const_cast<int16_t*>(xposArr);
  auto* styles = const_cast<uint8_t*>(stylesArr);
  auto* text = const_cast<char*>(textArr);
  uint16_t off = 0;
  for (uint16_t i = 0; i < numWords; i++) {
    const_cast<SourceRange*>(sourceRanges)[i] = ranges.empty() ? SourceRange{} : ranges[i];
    textOff[i] = off;
    xpos[i] = wordXpos[i];
    styles[i] = static_cast<uint8_t>(wordStyles[i]);
    memcpy(text + off, words[i].data(), words[i].size());
    off += static_cast<uint16_t>(words[i].size());
    text[off++] = '\0';
  }
  if (focusPresent) {
    auto* splitX = const_cast<uint16_t*>(focusSplitXArr);
    auto* boundary = const_cast<uint8_t*>(focusBoundaryArr);
    for (uint16_t i = 0; i < numWords; i++) {
      splitX[i] = focusSplitX[i];
      boundary[i] = focusBoundary[i];
    }
  }
  if (fontsPresent) {
    auto* fonts = const_cast<int32_t*>(wordFontArr);
    for (uint16_t i = 0; i < numWords; i++) {
      fonts[i] = wordFonts[i];
    }
  }
}

bool TextBlock::hasRuby() const {
  for (const auto& rt : rubyTexts) {
    if (!rt.empty()) return true;
  }
  return false;
}

struct TextBlock::RubyDrawInfo {
  int x;  // spread == 0: centred x. spread > 0: left edge of the base text.
  int width;
  std::string text;
  BidiUtils::BidiBaseDir baseDir;
  // ruby-align: space-around. >0 when the ruby is narrower than its base: the width difference
  // spread over the ruby's characters. 0 means plain centring.
  int spread = 0;
  // Gap before the first and after the last character: spread / 2n, capped at half a base
  // character; the inner gaps take the rest.
  int edge = 0;
  std::vector<size_t> breaks;  // character boundaries inside `text` (see CjkBreaks)
};

namespace {
// Copies ruby character k of s (boundaries in br) into out.
void rubyToken(const std::string& s, const std::vector<size_t>& br, const size_t k, std::string& out) {
  const size_t b = k == 0 ? 0 : br[k - 1];
  const size_t e = k == br.size() ? s.size() : br[k];
  out.assign(s.data() + b, e - b);
}
}  // namespace

// Resolve ruby collisions left-to-right to prevent adjacent ruby texts from overlapping.
// wordShift[i] is how far word i moves right of its laid-out position to make room.
void TextBlock::layoutRuby(const GfxRenderer& renderer, const int fontId, const int x, std::vector<int>& wordShift,
                           std::vector<RubyDrawInfo>& rubies) const {
  wordShift.assign(numWords, 0);
  rubies.resize(numWords);
  int accumulatedShift = 0;
  int lastEnd = -9999;
  for (uint16_t i = 0; i < numWords; i++) {
    wordShift[i] = accumulatedShift;
    if (i < rubyTexts.size() && !rubyTexts[i].empty() && (wordStyle(i) & EpdFontFamily::RUBY_CONTINUE) == 0) {
      // Find the group size (how many words are part of this ruby annotation)
      int groupWordCount = 1;
      while (i + groupWordCount < numWords && (wordStyle(i + groupWordCount) & EpdFontFamily::RUBY_CONTINUE) != 0) {
        groupWordCount++;
      }

      // Compute actual width for the group
      int groupActualWidth = 0;
      for (int k = 0; k < groupWordCount; ++k) {
        groupActualWidth +=
            renderer.getTextAdvanceX(fontId, wordText(i + k), wordStyle(i + k), blockStyle.letterSpacing);
      }

      const char* word = wordText(i);
      const int leaderWordX = xposArr[i] + x;
      const int leaderWordX_shifted = leaderWordX + accumulatedShift;
      const auto baseDir =
          static_cast<BidiUtils::BidiBaseDir>(BidiUtils::detectParagraphLevel(word, blockStyle.isRtl ? 1 : 0));
      const int rubyWidth =
          renderer.getTextAdvanceX(fontId, rubyTexts[i].c_str(), EpdFontFamily::SUP, blockStyle.letterSpacing);
      const int screenWidth = renderer.getScreenWidth();

      int rubyX = 0;
      int groupDrawX = 0;
      if (rubyWidth > groupActualWidth) {
        rubyX = leaderWordX_shifted - (rubyWidth - groupActualWidth) / 2;
        if (i == 0) {
          rubyX = std::max(leaderWordX_shifted, rubyX);
        }
        if (rubyX < lastEnd) {
          rubyX = lastEnd;
        }
        groupDrawX = rubyX + (rubyWidth - groupActualWidth) / 2;
      } else {
        groupDrawX = leaderWordX_shifted;
        rubyX = groupDrawX + (groupActualWidth - rubyWidth) / 2;
        if (i == 0) {
          rubyX = std::max(leaderWordX_shifted, rubyX);
        }
        if (rubyX < lastEnd) {
          const int push = lastEnd - rubyX;
          rubyX = lastEnd;
          groupDrawX += push;
        }
      }
      rubyX = std::max(0, std::min(rubyX, screenWidth - rubyWidth));
      // Keep groupDrawX aligned if rubyX was clamped by screen edges
      if (rubyWidth > groupActualWidth) {
        groupDrawX = rubyX + (rubyWidth - groupActualWidth) / 2;
      }

      rubies[i] = {rubyX, rubyWidth, rubyTexts[i], baseDir};
      // CSS ruby-align: space-around. A ruby narrower than its base is spread across it, each gap
      // between characters twice the gap at either end; one character (a bouten mark) stays centred.
      if (rubyWidth < groupActualWidth) {
        auto breaks = CjkBreaks::characterBreakByteOffsets(rubyTexts[i]);
        if (!breaks.empty()) {
          int tokenSum = 0;
          std::string tok;
          for (size_t k = 0; k <= breaks.size(); ++k) {
            rubyToken(rubyTexts[i], breaks, k, tok);
            tokenSum += renderer.getTextAdvanceX(fontId, tok.c_str(), EpdFontFamily::SUP, blockStyle.letterSpacing);
          }
          if (tokenSum < groupActualWidth) {
            int baseChars = 0;
            for (int k = 0; k < groupWordCount; ++k) {
              for (const char* p = wordText(i + k); *p; ++p) {
                if ((static_cast<uint8_t>(*p) & 0xC0) != 0x80) baseChars++;
              }
            }
            const int edgeCap = groupActualWidth / (2 * std::max(baseChars, 1));
            const int n = static_cast<int>(breaks.size()) + 1;
            RubyDrawInfo& info = rubies[i];
            info.x = groupDrawX;
            info.width = groupActualWidth;
            info.spread = groupActualWidth - tokenSum;
            info.edge = std::min(info.spread / (2 * n), edgeCap);
            info.breaks = std::move(breaks);
          }
        }
      }
      lastEnd = rubies[i].x + rubies[i].width;

      // Propagate shift to all words in the group and subsequent words
      const int groupShift = groupDrawX - leaderWordX;
      accumulatedShift = groupShift;
      for (int k = 0; k < groupWordCount; ++k) {
        wordShift[i + k] = accumulatedShift;
      }
      i += groupWordCount - 1;
    }
  }
}

// How far the line's baseline sits below the block font's: enough that its tallest word (an
// enlarged span, a heading's font-size) fits under the line top instead of rising into the line
// above. The layout made the line that much taller (ChapterHtmlSlimParser::addLineToPage).
int TextBlock::sizedWordDrop(const GfxRenderer& renderer, const int fontId) const {
  if (!fontsPresent) return 0;
  const int ascender = renderer.getFontAscenderSize(fontId);
  int tallest = ascender;
  for (uint16_t i = 0; i < numWords; i++) {
    const int32_t slot = wordFontArr[i];
    if (slot == 0 || slot == fontId) continue;
    const int wordAscender =
        isWordScaleTag(slot) ? (ascender * -slot + 128) / WORD_SCALE_ONE : renderer.getFontAscenderSize(slot);
    tallest = std::max(tallest, wordAscender);
  }
  return tallest - ascender;
}

void TextBlock::wordPlacements(const GfxRenderer& renderer, const int baseFontId, const int x, const int y,
                               const bool suppressRuby, std::vector<WordPlacement>& out) const {
  out.clear();
  if (!isValid) return;
  const int fontId = blockStyle.resolveFontId(baseFontId);
  std::vector<int> wordShift;
  if (hasRuby()) {
    std::vector<RubyDrawInfo> rubies;
    layoutRuby(renderer, fontId, x, wordShift, rubies);
  }
  const int wordY =
      y + (suppressRuby ? 0 : getRubyShift(renderer.getFontAscenderSize(fontId))) + sizedWordDrop(renderer, fontId);
  out.reserve(numWords);
  for (uint16_t i = 0; i < numWords; i++) {
    const int32_t fontSlot = fontsPresent ? wordFontArr[i] : 0;
    const int wordFontId = (!isWordScaleTag(fontSlot) && fontSlot != 0) ? fontSlot : fontId;
    const int wordX = xposArr[i] + x + (wordShift.empty() ? 0 : wordShift[i]);
    out.push_back({static_cast<int16_t>(wordX), static_cast<int16_t>(wordY), wordFontId});
  }
}

void TextBlock::render(const GfxRenderer& renderer, const int baseFontId, const int x, const int y,
                       const bool suppressRuby) const {
  if (!isValid) {
    LOG_ERR("TXB", "Render skipped: invalid block");
    return;
  }
  // Same resolution ParsedText::layoutAndExtractLines() used to position these words: the
  // per-word x offsets in the arena are only valid for THIS font.
  const int fontId = blockStyle.resolveFontId(baseFontId);

  // Ink polarity for every mark this block puts down: glyphs, ruby and decoration lines. The
  // black panel underneath was already painted by the PageBox emitted just before this line
  // (ChapterHtmlSlimParser::addLineToPage), so drawing white here lands on black, not on the page.
  const bool inkBlack = !blockStyle.isInverted();

  const bool scanning = renderer.isFontCacheScanning();
  const int ascender = renderer.getFontAscenderSize(fontId);

  // Drop cap: the words on this line and the ones beneath it were already positioned clear of
  // its column at layout time, so it only has to be drawn. Both offsets are relative to the
  // block's origin and were resolved against the same font metrics the layout used.
  if (dropCap.present()) {
    if (dropCap.prefixCp != 0) {
      // Body size, on the first line's own top edge -- it belongs to the text it opens, not to
      // the magnified letter beside it.
      char prefix[5] = {};
      utf8EncodeCodepoint(dropCap.prefixCp, prefix);
      renderer.drawText(fontId, x, y, prefix, inkBlack, static_cast<EpdFontFamily::Style>(dropCap.style));
    }
    renderer.drawCharUpscaled(fontId, dropCap.cp, dropCap.scale, x + dropCap.inkLeft, y + dropCap.inkTop, inkBlack,
                              static_cast<EpdFontFamily::Style>(dropCap.style));
  }

  // hasRuby() is an O(numWords) scan, so resolve it once here rather than per word.
  // Both arrays below are only ever read when the line carries ruby, so they stay
  // empty (zero allocations) for the ruby-less case, which is every line of a
  // non-CJK book.
  const bool blockHasRuby = hasRuby();
  std::vector<int> wordShiftArr;
  std::vector<RubyDrawInfo> rubies;
  if (blockHasRuby) layoutRuby(renderer, fontId, x, wordShiftArr, rubies);

  struct DecorationLineTracker {
    EpdFontFamily::Style style;
    int startX = -1;
    int endX = -1;
    int yPos = 0;

    bool active() const { return startX != -1; }
    void reset() {
      startX = -1;
      endX = -1;
      yPos = 0;
    }
  };

  DecorationLineTracker decorationLines[] = {
      {EpdFontFamily::UNDERLINE},
      {EpdFontFamily::STRIKETHROUGH},
  };

  const auto flushDecoration = [&](DecorationLineTracker& line) {
    if (line.active()) {
      renderer.drawLine(line.startX, line.yPos, line.endX, line.yPos, 2, inkBlack);
      line.reset();
    }
  };
  const auto flushDecorations = [&]() {
    for (auto& line : decorationLines) {
      flushDecoration(line);
    }
  };

  // Loop-invariant: hoisted out of the word loop so rubyTexts is scanned once,
  // not once per word.
  //
  // The shift makes room ABOVE the words for the annotation, and the layout reserved exactly
  // this much in the line's height. When the annotation is suppressed the layout reserves
  // nothing (see ChapterHtmlSlimParser::addLineToPage), so shifting here would push the words
  // out of their own line box and into the one below.
  const int rubyShift = (suppressRuby ? 0 : getRubyShift(ascender)) + sizedWordDrop(renderer, fontId);

  for (uint16_t i = 0; i < numWords; i++) {
    const char* word = wordText(i);
    const int wordX = xposArr[i] + x;
    // Inline font-size override: this word was MEASURED with its own font (see
    // ParsedText::calculateWordWidths), so it must draw with the same one.
    const int32_t fontSlot = fontsPresent ? wordFontArr[i] : 0;
    const bool wordScaled = isWordScaleTag(fontSlot);
    // A scale tag means the block's own font drawn smaller/larger, not a different font.
    const uint16_t wordScale = wordScaled ? static_cast<uint16_t>(-fontSlot) : WORD_SCALE_ONE;
    const int wordFontId = (!wordScaled && fontSlot != 0) ? fontSlot : fontId;
    const EpdFontFamily::Style currentStyle = wordStyle(i);
    const auto baseDir =
        static_cast<BidiUtils::BidiBaseDir>(BidiUtils::detectParagraphLevel(word, blockStyle.isRtl ? 1 : 0));
    const uint8_t boundary = focusBoundary(i);

    // SUP/SUB shift the baseline passed to drawText; the glyph is also scaled 50% inside
    // drawText, so these offsets are chosen relative to the full-size ascender:
    //   SUP: raise by 40% of ascender — sits clearly above the cap-height
    //   SUB: lower by 25% of ascender — descends below baseline without clashing with ascenders below
    int wordY = y + rubyShift;
    // drawTextScaled treats y as the top of a line box drawn ENTIRELY at `scale`, so it puts the
    // baseline at y + ascender*scale. That holds for the dictionary popup, which scales whole
    // paragraphs and scales their line advance to match. Here the line box is the block's own
    // height and only this word is scaled, so that baseline is wrong in both directions: an
    // enlarged word (a lettrine) sinks a full extra ascender and overlaps the line below, and a
    // shrunk one (a small-caps run) floats above its neighbours. Shift back onto the line's
    // baseline so a scaled word sits on the same one as the text around it.
    // Rounds the way GfxRenderer's own scaleSigned does, so the shift cancels its offset exactly.
    if (wordScaled) wordY += ascender - static_cast<int>((ascender * wordScale + 128) / WORD_SCALE_ONE);
    // A word in another loaded size: drawText puts its baseline at its own ascender, so the same
    // correction lands it on the line's baseline.
    if (wordFontId != fontId) wordY += ascender - renderer.getFontAscenderSize(wordFontId);
    if ((currentStyle & EpdFontFamily::SUP) != 0) {
      wordY -= ascender * 2 / 5;
    } else if ((currentStyle & EpdFontFamily::SUB) != 0) {
      wordY += ascender / 4;
    }

    const int drawX = wordX + (blockHasRuby ? wordShiftArr[i] : 0);

    if (boundary > 0) {
      // Place the logical prefix on the right for RTL words. Each run is reordered by drawText.
      // The bold prefix is bounded to 9 codepoints by the clamp on targetBoldChars in
      // ParsedText::addWord; 9 UTF-8 codepoints occupy at most 9 * 4 = 36 bytes, +1 for null = 37.
      // The split offset is computed at cache-creation time to avoid font metric lookups at render time.
      static constexpr size_t MAX_FOCUS_PREFIX_BYTES = 9 * 4 + 1;
      char boldBuf[40];
      static_assert(sizeof(boldBuf) >= MAX_FOCUS_PREFIX_BYTES,
                    "boldBuf too small for max focus prefix (9 codepoints * 4 UTF-8 bytes + null)");
      const auto boldStyle = static_cast<EpdFontFamily::Style>(currentStyle | EpdFontFamily::BOLD);
      const size_t boldLen =
          std::min<size_t>({static_cast<size_t>(boundary), static_cast<size_t>(wordTextLen(i)), sizeof(boldBuf) - 1});
      memcpy(boldBuf, word, boldLen);
      boldBuf[boldLen] = '\0';
      // Numeric runs keep LTR order even inside an RTL paragraph. Match layout.
      const bool rtl = BidiUtils::startsWithRtl(word);
      const int prefixX = drawX + (rtl ? focusSplitXArr[i] : 0);
      const int suffixX = drawX + (rtl ? 0 : focusSplitXArr[i]);
      renderer.drawTextScaled(wordFontId, prefixX, wordY, boldBuf, wordScale, inkBlack, boldStyle, baseDir,
                              blockStyle.letterSpacing);
      renderer.drawTextScaled(wordFontId, suffixX, wordY, word + boldLen, wordScale, inkBlack, currentStyle, baseDir,
                              blockStyle.letterSpacing);
    } else {
      renderer.drawTextScaled(wordFontId, drawX, wordY, word, wordScale, inkBlack, currentStyle, baseDir,
                              blockStyle.letterSpacing);
    }

    // Horizontal ruby text rendering
    if (blockHasRuby && !suppressRuby && i < rubyTexts.size() && !rubyTexts[i].empty() &&
        (wordStyle(i) & EpdFontFamily::RUBY_CONTINUE) == 0) {
      // Slightly above the ascender: fonts whose glyphs fill the em to the top
      // (UDDigiKyokasho) otherwise overlap their own base word.
      const int rubyY = wordY - ascender - std::max(1, ascender / 12);
      const RubyDrawInfo& ruby = rubies[i];
      if (ruby.spread == 0) {
        renderer.drawText(fontId, ruby.x, rubyY, ruby.text.c_str(), inkBlack, EpdFontFamily::SUP, ruby.baseDir,
                          blockStyle.letterSpacing);
      } else {
        // The edge gap is capped, so the rest of the space is split evenly over the n-1 inner gaps.
        // Each position is computed from k rather than accumulated, so rounding does not build up.
        const int n = static_cast<int>(ruby.breaks.size()) + 1;  // n >= 2 here
        const int innerTotal = ruby.spread - 2 * ruby.edge;
        int pen = 0;
        std::string tok;
        for (int k = 0; k < n; ++k) {
          const int tokenIndex = ruby.baseDir == BidiUtils::BidiBaseDir::RTL ? n - 1 - k : k;
          rubyToken(ruby.text, ruby.breaks, static_cast<size_t>(tokenIndex), tok);
          const int gx = ruby.x + pen + ruby.edge + (innerTotal * k + (n - 1) / 2) / (n - 1);
          renderer.drawText(fontId, gx, rubyY, tok.c_str(), inkBlack, EpdFontFamily::SUP, ruby.baseDir,
                            blockStyle.letterSpacing);
          pen += renderer.getTextAdvanceX(fontId, tok.c_str(), EpdFontFamily::SUP, blockStyle.letterSpacing);
        }
      }
    }

    if (scanning) {
      continue;
    }

    if (EpdFontFamily::hasTextDecoration(currentStyle)) {
      int lineStartX = drawX;
      int lineWidth =
          renderer.getTextWidthScaled(wordFontId, word, wordScale, currentStyle, baseDir, blockStyle.letterSpacing);

      if ((currentStyle & (EpdFontFamily::SUP | EpdFontFamily::SUB)) != 0) {
        lineWidth = (lineWidth + 1) / 2;
      }

      // Do not decorate the synthetic em-space used for paragraph indentation.
      if (wordTextLen(i) >= 3 && static_cast<uint8_t>(word[0]) == 0xE2 && static_cast<uint8_t>(word[1]) == 0x80 &&
          static_cast<uint8_t>(word[2]) == 0x83) {
        const char* visibleText = word + 3;
        lineStartX += renderer.getTextAdvanceX(wordFontId, "\xe2\x80\x83", currentStyle, blockStyle.letterSpacing);
        lineWidth = renderer.getTextWidthScaled(wordFontId, visibleText, wordScale, currentStyle, baseDir,
                                                blockStyle.letterSpacing);
        if ((currentStyle & (EpdFontFamily::SUP | EpdFontFamily::SUB)) != 0) {
          lineWidth = (lineWidth + 1) / 2;
        }
      }

      // Placed from this word's own glyphs: its font or scale, and the half-size SUP/SUB glyphs,
      // which keep the baseline drawText was given. The underline sits just under the baseline;
      // the strike crosses the middle of the x-height (about 27% of the ascender above it).
      const int glyphAscender = wordScaled ? static_cast<int>((ascender * wordScale + 128) / WORD_SCALE_ONE)
                                           : renderer.getFontAscenderSize(wordFontId);
      const int baseline = wordY + glyphAscender;
      const bool script = (currentStyle & (EpdFontFamily::SUP | EpdFontFamily::SUB)) != 0;
      const int inkAscender = script ? glyphAscender / 2 : glyphAscender;
      for (auto& line : decorationLines) {
        if ((currentStyle & line.style) == 0) {
          flushDecoration(line);
          continue;
        }

        const int lineY =
            line.style == EpdFontFamily::UNDERLINE ? baseline + (script ? 1 : 2) : baseline - inkAscender * 27 / 100;
        if (line.active() && line.yPos != lineY) {
          flushDecoration(line);
        }
        if (!line.active()) {
          line.startX = lineStartX;
          line.yPos = lineY;
        }
        line.endX = lineStartX + lineWidth;
      }
    } else {
      flushDecorations();
    }
  }
  flushDecorations();
}

bool TextBlock::serialize(HalFile& file) const {
  if (!isValid) {
    LOG_ERR("TXB", "Serialization failed: invalid block");
    return false;
  }

  // Word data: scalars, then the arena verbatim -- its in-memory layout is
  // exactly the on-disk layout (see TextBlock.h), so one write covers all
  // per-word arrays and the text blob. Flags byte: bit 0 = focus split
  // annotations, bit 1 = per-word font ids (inline font-size).
  serialization::writePod(file, numWords);
  serialization::writePod(file, static_cast<uint8_t>((focusPresent ? 1 : 0) | (fontsPresent ? 2 : 0)));
  serialization::writePod(file, textBytes);
  serialization::writePod(file, paragraphStartWord);
  if (numWords > 0) {
    const size_t size = arenaSize(numWords, focusPresent, fontsPresent, textBytes);
    if (file.write(arena.get(), size) != size) {
      LOG_ERR("TXB", "Serialization failed: arena write (%u bytes)", static_cast<uint32_t>(size));
      return false;
    }
  }

  // Ruby text data
  for (size_t i = 0; i < numWords; i++) {
    serialization::writeString(file, (i < rubyTexts.size()) ? rubyTexts[i] : std::string());
  }

  // Style (alignment + margins/padding/indent)
  serialization::writePod(file, blockStyle.alignment);
  serialization::writePod(file, blockStyle.textAlignDefined);
  serialization::writePod(file, blockStyle.marginTop);
  serialization::writePod(file, blockStyle.marginBottom);
  serialization::writePod(file, blockStyle.marginLeft);
  serialization::writePod(file, blockStyle.marginRight);
  serialization::writePod(file, blockStyle.paddingTop);
  serialization::writePod(file, blockStyle.paddingBottom);
  serialization::writePod(file, blockStyle.paddingLeft);
  serialization::writePod(file, blockStyle.paddingRight);
  serialization::writePod(file, blockStyle.textIndent);
  serialization::writePod(file, blockStyle.textIndentDefined);
  serialization::writePod(file, blockStyle.isRtl);
  serialization::writePod(file, blockStyle.directionDefined);
  serialization::writePod(file, blockStyle.fontId);
  serialization::writePod(file, blockStyle.letterSpacing);
  serialization::writePod(file, blockStyle.inkMode);
  serialization::writePod(file, blockStyle.inkModeDefined);

  // Drop cap. Written on every line (cp 0 = none) rather than behind a flag byte: the record
  // stays fixed-width, which is what lets deserialize() read it back without a framing branch.
  serialization::writePod(file, dropCap.cp);
  serialization::writePod(file, dropCap.prefixCp);
  serialization::writePod(file, dropCap.inkLeft);
  serialization::writePod(file, dropCap.inkTop);
  serialization::writePod(file, dropCap.scale);
  serialization::writePod(file, dropCap.style);

  return true;
}

std::unique_ptr<TextBlock> TextBlock::deserialize(HalFile& file) {
  uint16_t wc;
  uint8_t flags;
  uint16_t textBytes;
  uint16_t paragraphStartWord;
  serialization::readPod(file, wc);
  serialization::readPod(file, flags);
  serialization::readPod(file, textBytes);
  serialization::readPod(file, paragraphStartWord);

  // Sanity checks: cap the arena allocation and reject impossible geometry
  // (every word carries at least its NUL terminator).
  if (wc > 10000 || (paragraphStartWord != UINT16_MAX && paragraphStartWord >= wc)) {
    LOG_ERR("TXB", "Deserialization failed: word count %u exceeds maximum", wc);
    return nullptr;
  }
  if ((flags & ~0x03u) != 0) {
    LOG_ERR("TXB", "Deserialization failed: unknown flags 0x%02x", flags);
    return nullptr;
  }
  if ((wc == 0 && textBytes != 0) || (wc > 0 && textBytes < wc)) {
    LOG_ERR("TXB", "Deserialization failed: bad text size %u for %u words", textBytes, wc);
    return nullptr;
  }

  std::unique_ptr<TextBlock> block(new (std::nothrow) TextBlock());
  if (!block) {
    LOG_ERR("TXB", "OOM: TextBlock");
    return nullptr;
  }
  block->numWords = wc;
  block->textBytes = textBytes;
  block->paragraphStartWord = paragraphStartWord;
  block->focusPresent = (flags & 1) != 0;
  block->fontsPresent = (flags & 2) != 0;

  if (wc > 0) {
    const size_t size = arenaSize(wc, block->focusPresent, block->fontsPresent, textBytes);
    block->arena = makeUniqueNoThrow<uint8_t[]>(size);
    if (!block->arena) {
      LOG_ERR("TXB", "OOM: arena %u bytes", static_cast<uint32_t>(size));
      return nullptr;
    }
    if (file.read(block->arena.get(), size) != size) {
      LOG_ERR("TXB", "Deserialization failed: arena read (%u bytes)", static_cast<uint32_t>(size));
      return nullptr;
    }
    block->bindArenaPointers();

    // Validate offsets before anything dereferences wordText(): offset 0 first,
    // strictly increasing, in bounds, and every word NUL-terminated (word i ends
    // at the byte before offset i+1; the last word at the last text byte).
    const uint16_t* textOff = block->textOffArr;
    const char* text = block->textArr;
    if (textOff[0] != 0 || text[textBytes - 1] != '\0') {
      LOG_ERR("TXB", "Deserialization failed: corrupt text layout");
      return nullptr;
    }
    for (uint16_t i = 1; i < wc; i++) {
      if (textOff[i] <= textOff[i - 1] || textOff[i] >= textBytes || text[textOff[i] - 1] != '\0') {
        LOG_ERR("TXB", "Deserialization failed: corrupt word offset %u", i);
        return nullptr;
      }
    }
  }

  // Ruby text data. Ruby is a CJK feature, so for nearly every book every entry here
  // is the empty string. Materializing the vector regardless costs wordCount * 24 bytes
  // (sizeof(std::string)) plus a heap block per line, held for as long as the page is
  // resident -- several KB of DRAM on a full page, none of it ever read. An empty
  // rubyTexts is already the "no ruby" representation: hasRuby() reports false and every
  // other reader is guarded by `i < rubyTexts.size()`, so allocate lazily and only once a
  // non-empty annotation actually shows up.
  //
  // `scratch` is reused across words: readString() resizes it to the incoming length and
  // overwrites every byte, so a moved-from value carries nothing into the next iteration.
  std::string scratch;
  for (uint16_t i = 0; i < wc; i++) {
    serialization::readString(file, scratch);
    if (scratch.empty()) continue;
    if (block->rubyTexts.empty()) {
      block->rubyTexts.resize(wc);
    }
    block->rubyTexts[i] = std::move(scratch);
  }

  // Style (alignment + margins/padding/indent)
  BlockStyle& blockStyle = block->blockStyle;
  serialization::readPod(file, blockStyle.alignment);
  serialization::readPod(file, blockStyle.textAlignDefined);
  serialization::readPod(file, blockStyle.marginTop);
  serialization::readPod(file, blockStyle.marginBottom);
  serialization::readPod(file, blockStyle.marginLeft);
  serialization::readPod(file, blockStyle.marginRight);
  serialization::readPod(file, blockStyle.paddingTop);
  serialization::readPod(file, blockStyle.paddingBottom);
  serialization::readPod(file, blockStyle.paddingLeft);
  serialization::readPod(file, blockStyle.paddingRight);
  serialization::readPod(file, blockStyle.textIndent);
  serialization::readPod(file, blockStyle.textIndentDefined);
  serialization::readPod(file, blockStyle.isRtl);
  serialization::readPod(file, blockStyle.directionDefined);
  serialization::readPod(file, blockStyle.fontId);
  serialization::readPod(file, blockStyle.letterSpacing);
  serialization::readPod(file, blockStyle.inkMode);
  serialization::readPod(file, blockStyle.inkModeDefined);
  // A corrupt byte must not commit the line to white-on-nothing: only the exact Inverted value
  // survives, anything else falls back to the always-legible Normal.
  if (blockStyle.inkMode != CssInkMode::Inverted) blockStyle.inkMode = CssInkMode::Normal;

  serialization::readPod(file, block->dropCap.cp);
  serialization::readPod(file, block->dropCap.prefixCp);
  serialization::readPod(file, block->dropCap.inkLeft);
  serialization::readPod(file, block->dropCap.inkTop);
  serialization::readPod(file, block->dropCap.scale);
  serialization::readPod(file, block->dropCap.style);
  block->dropCap.style &= DROP_CAP_STYLE_MASK;
  // Bound what a corrupt record can ask the glyph scaler for: an absurd scale would blow up
  // the block-replication loop (and paint over the page) rather than fail. The cap matches the
  // ceiling ParsedText applies when it chooses the scale.
  if (block->dropCap.scale > MAX_DROP_CAP_SCALE) block->dropCap = DropCap{};

  return block;
}
