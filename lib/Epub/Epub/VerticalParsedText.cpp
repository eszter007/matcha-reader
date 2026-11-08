#include "VerticalParsedText.h"

#include <Arduino.h>
#include <Logging.h>
#include <Utf8.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "Epub/VerticalBatch.h"
#include "GfxRenderer.h"
#include "Kinsoku.h"
#include "VerticalLayoutSupport.h"

namespace {

// Index-based UTF-8 decode. The shared utf8NextCodepoint() walks a NUL-terminated buffer via a
// pointer; this bounds every multi-byte sequence against the string's length instead, so a
// truncated tail in EPUB markup yields one replacement char rather than reading past the end.
// Encoding has no such difference and uses utf8AppendCodepoint() directly.
uint32_t decodeUtf8At(const std::string& s, size_t i, size_t* bytesConsumed) {
  const unsigned char c0 = static_cast<unsigned char>(s[i]);
  if (c0 < 0x80) {
    *bytesConsumed = 1;
    return c0;
  }
  if ((c0 & 0xE0) == 0xC0 && i + 1 < s.size()) {
    *bytesConsumed = 2;
    return ((c0 & 0x1F) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3F);
  }
  if ((c0 & 0xF0) == 0xE0 && i + 2 < s.size()) {
    *bytesConsumed = 3;
    return ((c0 & 0x0F) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 6) |
           (static_cast<unsigned char>(s[i + 2]) & 0x3F);
  }
  if ((c0 & 0xF8) == 0xF0 && i + 3 < s.size()) {
    *bytesConsumed = 4;
    return ((c0 & 0x07) << 18) | ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 12) |
           ((static_cast<unsigned char>(s[i + 2]) & 0x3F) << 6) | (static_cast<unsigned char>(s[i + 3]) & 0x3F);
  }
  // Malformed byte -- treat as a single replacement-ish char and move on
  // rather than getting stuck.
  *bytesConsumed = 1;
  return c0;
}

uint32_t composeKanaDiacritic(uint32_t base, uint32_t mark) {
  // U+3099 COMBINING KATAKANA-HIRAGANA VOICED SOUND MARK
  if (mark == 0x3099) {
    switch (base) {
      case 0x3046:
        return 0x3094;  // う + ゙ = ゔ
      case 0x304B:
        return 0x304C;  // か -> が
      case 0x304D:
        return 0x304E;  // き -> ぎ
      case 0x304F:
        return 0x3050;  // く -> ぐ
      case 0x3051:
        return 0x3052;  // け -> げ
      case 0x3053:
        return 0x3054;  // こ -> ご
      case 0x3055:
        return 0x3056;  // さ -> ざ
      case 0x3057:
        return 0x3058;  // し -> じ
      case 0x3059:
        return 0x305A;  // す -> ず
      case 0x305B:
        return 0x305C;  // せ -> ぜ
      case 0x305D:
        return 0x305E;  // そ -> ぞ
      case 0x305F:
        return 0x3060;  // た -> だ
      case 0x3061:
        return 0x3062;  // ち -> ぢ
      case 0x3064:
        return 0x3065;  // つ -> づ
      case 0x3066:
        return 0x3067;  // て -> で
      case 0x3068:
        return 0x3069;  // と -> ど
      case 0x306F:
        return 0x3070;  // は -> ば
      case 0x3072:
        return 0x3073;  // ひ -> び
      case 0x3075:
        return 0x3076;  // ふ -> ぶ
      case 0x3078:
        return 0x3079;  // へ -> べ
      case 0x307B:
        return 0x307C;  // ほ -> ぼ
      case 0x309D:
        return 0x309E;  // ゝ -> ゞ
      case 0x30A6:
        return 0x30F4;  // ウ -> ヴ
      case 0x30AB:
        return 0x30AC;  // カ -> ガ
      case 0x30AD:
        return 0x30AE;  // キ -> ギ
      case 0x30AF:
        return 0x30B0;  // ク -> グ
      case 0x30B1:
        return 0x30B2;  // ケ -> ゲ
      case 0x30B3:
        return 0x30B4;  // コ -> ゴ
      case 0x30B5:
        return 0x30B6;  // サ -> ザ
      case 0x30B7:
        return 0x30B8;  // シ -> ジ
      case 0x30B9:
        return 0x30BA;  // ス -> ズ
      case 0x30BB:
        return 0x30BC;  // セ -> ゼ
      case 0x30BD:
        return 0x30BE;  // ソ -> ゾ
      case 0x30BF:
        return 0x30C0;  // タ -> ダ
      case 0x30C1:
        return 0x30C2;  // チ -> ヂ
      case 0x30C4:
        return 0x30C5;  // ツ -> ヅ
      case 0x30C6:
        return 0x30C7;  // テ -> デ
      case 0x30C8:
        return 0x30C9;  // ト -> ド
      case 0x30CF:
        return 0x30D0;  // ハ -> バ
      case 0x30D2:
        return 0x30D3;  // ヒ -> ビ
      case 0x30D5:
        return 0x30D6;  // フ -> ブ
      case 0x30D8:
        return 0x30D9;  // ヘ -> ベ
      case 0x30DB:
        return 0x30DC;  // ホ -> ボ
      case 0x30EF:
        return 0x30F7;  // ワ -> ヷ
      case 0x30F0:
        return 0x30F8;  // ヰ -> ヸ
      case 0x30F1:
        return 0x30F9;  // ヱ -> ヹ
      case 0x30F2:
        return 0x30FA;  // ヲ -> ヺ
      case 0x30FD:
        return 0x30FE;  // ヽ -> ヾ
      default:
        return 0;
    }
  }

  // U+309A COMBINING KATAKANA-HIRAGANA SEMI-VOICED SOUND MARK
  if (mark == 0x309A) {
    switch (base) {
      case 0x306F:
        return 0x3071;  // は -> ぱ
      case 0x3072:
        return 0x3074;  // ひ -> ぴ
      case 0x3075:
        return 0x3077;  // ふ -> ぷ
      case 0x3078:
        return 0x307A;  // へ -> ぺ
      case 0x307B:
        return 0x307D;  // ほ -> ぽ
      case 0x30CF:
        return 0x30D1;  // ハ -> パ
      case 0x30D2:
        return 0x30D4;  // ヒ -> ピ
      case 0x30D5:
        return 0x30D7;  // フ -> プ
      case 0x30D8:
        return 0x30DA;  // ヘ -> ペ
      case 0x30DB:
        return 0x30DD;  // ホ -> ポ
      default:
        return 0;
    }
  }

  return 0;
}

}  // namespace

VerticalParsedText::VerticalParsedText(const GfxRenderer& renderer, int fontId, uint16_t viewportWidth,
                                       uint16_t viewportHeight)
    : renderer_(renderer), fontId_(fontId), viewportWidth_(viewportWidth), viewportHeight_(viewportHeight) {}

bool measureGlyphInk(const GfxRenderer& renderer, const int fontId, const uint32_t cp, const uint8_t style,
                     GlyphInk* out) {
  int left = 0, width = 0, top = 0, height = 0;
  if (!renderer.getGlyphMetrics(fontId, cp, static_cast<EpdFontFamily::Style>(style), &left, &width, &top, &height)) {
    return false;
  }
  *out = GlyphInk{left, width, top, height};
  return true;
}

int glyphCellPx(const VerticalGlyph& g, const int bodyCellPx) {
  const int scale8 = g.scale8 == 0 ? 8 : g.scale8;
  return scale8 == 8 ? bodyCellPx : std::max(1, static_cast<int>(std::lround(bodyCellPx * scale8 / 8.0)));
}

int verticalCellPx(const GfxRenderer& renderer, const int fontId, bool* measured) {
  // SD fonts measure through their advance table; make sure the reference glyph is in it. A cold
  // table measures 漢 as 0, and the getLineHeight fallback includes interline spacing -- cells
  // jump ~1em -> ~1.45em, and layout and draw disagree for that frame.
  renderer.ensureSdCardFontReady(fontId, "\xe6\xbc\xa2", 0x01);
  const int cjkAdvance = renderer.getTextAdvanceX(fontId, "\xe6\xbc\xa2", static_cast<EpdFontFamily::Style>(0));  // 漢
  if (measured) *measured = cjkAdvance > 0;
  if (cjkAdvance > 0) return cjkAdvance;
  return renderer.getLineHeight(fontId);
}

int verticalNominalInkGapPx(const GfxRenderer& renderer, const int fontId, const int cellPx, bool* measured) {
  renderer.ensureSdCardFontReady(fontId, "\xe6\xbc\xa2", 0x01);
  GlyphInk ink;
  const bool ok = measureGlyphInk(renderer, fontId, 0x6F22 /* 漢 */, 0, &ink) && ink.height > 0;
  if (measured) *measured = ok;
  // A face whose ink is TALLER than its own em leaves no white at all -- that is a measurement,
  // not a failure, so clamp it to zero and let it be cached. Treating it as unmeasured (NotoSerif
  // JP: 30px of ink in a 29px em) re-probed 漢 on every batch for the whole chapter, which is the
  // SD-glyph eviction the memo exists to prevent.
  return ok ? std::max(0, cellPx - ink.height) : 0;
}

// Record a paragraph boundary before stream index `idx`, ignoring a repeat at the same index --
// several sources (a source newline, a run boundary, a carried-over trailing break) can each
// report the same boundary, and a duplicate would open an extra empty column.
void VerticalParsedText::recordParagraphBreakAt(const size_t idx) {
  if (!paragraphBreaksBeforeIndex_.empty() && paragraphBreaksBeforeIndex_.back() == idx) return;
  paragraphBreaksBeforeIndex_.push_back(idx);
}

void VerticalParsedText::reserveStreamFor(size_t utf8Bytes) {
  // CJK prose is ~3 UTF-8 bytes per codepoint, so bytes/3 (+ slack for embedded ASCII) sizes the
  // PendingChar slots. Not the raw byte count: at 32 bytes per slot that over-requests 3x.
  // The affordability check is on the REQUEST size, not free heap. reserve() is one contiguous
  // allocation and aborts under -fno-exceptions, so a request that does not comfortably fit is
  // skipped in favour of incremental growth (guarded by canPushStreamChar).
  const size_t slots = utf8Bytes / 3 + 8;
  const size_t needed = stream_.size() + slots;
  if (needed <= stream_.capacity()) return;
  const size_t requestBytes = needed * sizeof(PendingChar);
  if (!heapCanAfford(requestBytes, MIN_FREE_HEAP_FOR_RESERVE)) {
    LOG_DBG("VPT", "Reserve of %u bytes doesn't fit (free=%u); growing incrementally",
            static_cast<unsigned>(requestBytes), ESP.getMaxAllocHeap());
    return;
  }
  stream_.reserve(needed);
}

void VerticalParsedText::preallocateStream() {
  constexpr size_t STREAM_STABLE_ENTRIES = 512 * vertical_batch::kScale;
  const size_t bytes = STREAM_STABLE_ENTRIES * sizeof(PendingChar);
  if (stream_.capacity() >= STREAM_STABLE_ENTRIES) return;
  if (heapCanAfford(bytes, MIN_FREE_HEAP_FOR_RESERVE)) {
    stream_.reserve(STREAM_STABLE_ENTRIES);
    // A batch's ruby, pinned with the stream for the same reason (a few bytes per ruby'd char).
    rubyPool_.reserve(1024 * vertical_batch::kScale);
  } else {
    LOG_DBG("VPT", "preallocateStream: %u bytes don't fit (maxAlloc=%u); falling back to incremental growth",
            static_cast<unsigned>(bytes), ESP.getMaxAllocHeap());
  }
}

void VerticalParsedText::setRuby(PendingChar& pc, const char* s, const size_t len) {
  pc.rubyLen = 0;
  if (len == 0 || len > 1023) return;  // PendingChar::rubyLen holds 10 bits
  const size_t need = rubyPool_.size() + len;
  if (need > rubyPool_.capacity() && !heapCanAfford(std::max(need, rubyPool_.capacity() * 2), SMALL_ALLOC_MARGIN)) {
    everDroppedForHeap_ = true;
    return;
  }
  pc.rubyOffset = static_cast<uint32_t>(rubyPool_.size());
  pc.rubyLen = static_cast<uint16_t>(len);
  rubyPool_.append(s, len);
}

void VerticalParsedText::pushCarried(PendingChar& c) {
  if (c.rubyLen) {
    const uint32_t from = c.rubyOffset;
    setRuby(c, carriedRubyPool_.data() + from, c.rubyLen);
  }
  stream_.push_back(c);
}

bool VerticalParsedText::canPushStreamChar() {
  if (oom_) return false;
  static constexpr uint32_t MARGINS[] = {SMALL_ALLOC_MARGIN};
  constexpr size_t LINEAR_GROWTH_STEP = 64;  // PendingChar elements; keeps stalled retries cheap
  if (growForOnePush(stream_, MARGINS, LINEAR_GROWTH_STEP)) return true;
  LOG_DBG("VPT", "Low heap (%u bytes) while building vertical text stream; truncating batch", ESP.getMaxAllocHeap());
  oom_ = true;
  everDroppedForHeap_ = true;
  return false;
}

void VerticalParsedText::addParagraph(const std::string& utf8Text) {
  // Characters held back from the previous batch (a rotated run that ran to its end). They belong
  // to the paragraph that was in flight then, so they go in BEFORE this batch records its own
  // break -- the rest of the word is about to follow them and the two must gather as one run.
  if (!carriedRunTail_.empty()) {
    const uint32_t carryIndex =
        paragraphBreaksBeforeIndex_.empty() ? 0 : static_cast<uint32_t>(paragraphBreaksBeforeIndex_.size() - 1);
    for (auto& carried : carriedRunTail_) {
      if (!canPushStreamChar()) break;
      carried.paragraphIndex = carryIndex;
      pushCarried(carried);
    }
    carriedRunTail_.clear();
    carriedRubyPool_.clear();
  }

  const uint32_t paragraphIndex = static_cast<uint32_t>(paragraphBreaksBeforeIndex_.size());
  paragraphBreaksBeforeIndex_.push_back(stream_.size());

  reserveStreamFor(utf8Text.size());

  size_t i = 0;
  while (i < utf8Text.size()) {
    size_t consumed = 1;
    const uint32_t cp = decodeUtf8At(utf8Text, i, &consumed);
    if ((cp == 0x3099 || cp == 0x309A) && !stream_.empty() && stream_.back().paragraphIndex == paragraphIndex) {
      const uint32_t composed = composeKanaDiacritic(stream_.back().codepoint, cp);
      if (composed != 0) {
        stream_.back().codepoint = composed;
        i += consumed;
        continue;
      }
    }
    // Keep explicit source line breaks as hard vertical column breaks.
    // Tabs are still ignored.
    if (cp == '\n' || cp == '\r') {
      recordParagraphBreakAt(stream_.size());
      i += consumed;
      continue;
    }
    // Keep plain spaces. Kinsoku::isRotatedRunCharacter() counts ' ' as part of a Latin run, so
    // dropping it here merges embedded English into one token ("CrossPointReader"). A stray space
    // between two CJK characters renders as a near-invisible 1-character rotated run.
    if (cp == '\t') {
      i += consumed;
      continue;
    }
    if (!canPushStreamChar()) return;
    // visibleTextOffset stays 0: this overload takes plain text with no extractor context, so
    // there is no chapter-wide position to attribute it to. Only the annotated path (the one
    // the real chapter build uses) carries offsets.
    stream_.push_back(
        PendingChar{Kinsoku::verticalFormOf(cp), paragraphIndex, 0, static_cast<uint32_t>(i), 0, false, 0});
    i += consumed;
  }
}

void VerticalParsedText::addAnnotatedParagraph(const std::vector<RubyRun>& runs,
                                               const bool continuesPreviousParagraph) {
  // A paragraph break recorded at the very END of the previous batch (a trailing '\n' in the
  // last run) could never fire there -- the layout loop only visits indices < stream size --
  // and reset() would have discarded it. layoutPages() flags it instead; re-record it here at
  // the start of the new batch so the break lands where the next character actually goes.
  // This is DELIBERATELY independent of continuesPreviousParagraph: the flag comes from a
  // real newline in the source, not from the sink's memory-bound chunking.
  if (pendingTrailingBreak_) {
    recordParagraphBreakAt(stream_.size());
    pendingTrailingBreak_ = false;
  }

  // Characters held back from the previous batch (a rotated run that ran to its end). They belong
  // to the paragraph that was in flight then, so they go in BEFORE this batch records its own
  // break -- the rest of the word is about to follow them and the two must gather as one run.
  if (!carriedRunTail_.empty()) {
    const uint32_t carryIndex =
        paragraphBreaksBeforeIndex_.empty() ? 0 : static_cast<uint32_t>(paragraphBreaksBeforeIndex_.size() - 1);
    for (auto& carried : carriedRunTail_) {
      if (!canPushStreamChar()) break;
      carried.paragraphIndex = carryIndex;
      pushCarried(carried);
    }
    carriedRunTail_.clear();
    carriedRubyPool_.clear();
  }

  // A continuation chunk belongs to the paragraph already in flight: no break is recorded and
  // the glyphs share the previous chunk's paragraph index (see the header doc comment).
  uint32_t paragraphIndex;
  if (continuesPreviousParagraph) {
    const size_t breaks = paragraphBreaksBeforeIndex_.size();
    paragraphIndex = breaks == 0 ? 0 : static_cast<uint32_t>(breaks - 1);
  } else {
    paragraphIndex = static_cast<uint32_t>(paragraphBreaksBeforeIndex_.size());
    paragraphBreaksBeforeIndex_.push_back(stream_.size());
  }

  {
    size_t totalBaseBytes = 0;
    for (const auto& run : runs) totalBaseBytes += run.baseText.size();
    reserveStreamFor(totalBaseBytes);
  }

  for (const auto& run : runs) {
    if (run.baseText.empty()) continue;

    // Decode base text into codepoints, then distribute ruby across them. The scratch vectors are
    // members, cleared per run and kept for the build: one small block each for the whole chapter
    // instead of three or four allocate-copy-free cycles per run, which on a furigana-dense chapter
    // is hundreds of them, each a chance to leave a fragment between the stream and the pages.
    std::vector<size_t>& baseOffsets = scratchOffsets_;
    std::vector<uint32_t>& baseCps = scratchCps_;
    // Source codepoint index of each kept base character, relative to the run's first codepoint.
    // Counted over EVERY decoded codepoint, including the newlines/tabs dropped below and the
    // second half of a composed kana diacritic -- the extractor's visible-text counter advances
    // for those too, so skipping them here would drift the two counts apart character by
    // character. Parallel to baseCps/baseOffsets.
    std::vector<uint32_t>& baseCpIndex = scratchCpIndex_;
    std::vector<size_t>& breakBeforeBaseIndex = scratchBreaks_;
    baseOffsets.clear();
    baseCps.clear();
    baseCpIndex.clear();
    breakBeforeBaseIndex.clear();
    // One entry per codepoint bounds all three: the decode below only ever drops entries
    // (newlines, the second half of a composed kana diacritic), never adds one.
    const size_t maxEntries = countUtf8Codepoints(run.baseText);
    reserveIfAffordable(baseOffsets, maxEntries);
    reserveIfAffordable(baseCps, maxEntries);
    reserveIfAffordable(baseCpIndex, maxEntries);
    {
      size_t i = 0;
      uint32_t cpIndex = 0;
      while (i < run.baseText.size()) {
        size_t consumed = 1;
        const uint32_t cp = decodeUtf8At(run.baseText, i, &consumed);
        const uint32_t thisCpIndex = cpIndex++;
        // The reserves above are skipped when they don't fit, which leaves these vectors growing by
        // doubling through the THROWING operator new -- it aborts the device under -fno-exceptions
        // rather than returning null (observed: a furigana-dense chapter entered this function at
        // maxAlloc=2036 and aborted here). Truncate the run instead, the same degradation
        // canPushStreamChar already accepts for the stream itself.
        if (!canGrowScratch(baseOffsets) || !canGrowScratch(baseCps) || !canGrowScratch(baseCpIndex) ||
            !canGrowScratch(breakBeforeBaseIndex)) {
          LOG_DBG("VPT", "Low heap (%u bytes) decoding run; truncating", ESP.getMaxAllocHeap());
          everDroppedForHeap_ = true;
          break;
        }
        if ((cp == 0x3099 || cp == 0x309A) && !baseCps.empty()) {
          const uint32_t composed = composeKanaDiacritic(baseCps.back(), cp);
          if (composed != 0) {
            baseCps.back() = composed;
            i += consumed;
            continue;
          }
        }
        if (cp == '\n' || cp == '\r') {
          if (breakBeforeBaseIndex.empty() || breakBeforeBaseIndex.back() != baseCps.size()) {
            breakBeforeBaseIndex.push_back(baseCps.size());
          }
          i += consumed;
          continue;
        }
        if (cp == '\t') {
          i += consumed;
          continue;
        }
        baseOffsets.push_back(i);
        baseCps.push_back(cp);
        baseCpIndex.push_back(thisCpIndex);
        i += consumed;
      }
    }

    // Record the run's newline breaks BEFORE the empty-run skip: a run consisting only of
    // newlines (e.g. the inter-tag whitespace between </p> and <p> arriving as its own run at
    // a style boundary) has no characters to push but its break is a real paragraph boundary.
    // The old order silently dropped exactly those breaks, merging the following paragraph
    // into the current column.
    if (baseCps.empty()) {
      for (size_t relIdx : breakBeforeBaseIndex) {
        recordParagraphBreakAt(stream_.size() + relIdx);  // relIdx is always 0 here
      }
      continue;
    }

    const size_t runStartStreamIndex = stream_.size();

    if (run.rubyText.empty()) {
      for (size_t k = 0; k < baseCps.size(); k++) {
        if (!canPushStreamChar()) return;
        PendingChar pc{Kinsoku::verticalFormOf(baseCps[k]),
                       paragraphIndex,
                       run.fontSlot,
                       static_cast<uint32_t>(baseOffsets[k]),
                       run.style,
                       run.emphasis,
                       run.indentQuarters};
        pc.scale8 = run.scale8;
        pc.visibleTextOffset = run.visibleTextOffset + baseCpIndex[k];
        stream_.push_back(pc);
      }
    } else {
      // Decode ruby codepoints to distribute evenly across base characters.
      std::vector<uint32_t>& rubyCps = scratchRuby_;
      rubyCps.clear();
      reserveIfAffordable(rubyCps, run.rubyText.size());
      {
        size_t ri = 0;
        while (ri < run.rubyText.size()) {
          if (!canGrowScratch(rubyCps)) {  // the reading is cut short rather than the device aborted
            everDroppedForHeap_ = true;
            break;
          }
          size_t consumed = 1;
          rubyCps.push_back(decodeUtf8At(run.rubyText, ri, &consumed));
          ri += consumed;
        }
      }

      // Distribute ruby codepoints across base characters. Each base char
      // gets a roughly equal share of the annotation string, re-encoded
      // back to UTF-8.
      const size_t baseCount = baseCps.size();
      const size_t rubyCount = rubyCps.size();
      for (size_t k = 0; k < baseCount; k++) {
        const size_t rubyStart = rubyCount * k / baseCount;
        const size_t rubyEnd = rubyCount * (k + 1) / baseCount;
        std::string slice;
        for (size_t r = rubyStart; r < rubyEnd; r++) utf8AppendCodepoint(rubyCps[r], slice);
        if (!canPushStreamChar()) return;
        // Group ruby: the characters after the first are marked, so layout keeps the group in one
        // column and the renderer sets the annotation over the whole group (JLREQ 3.3.6). Each
        // still carries its share, which keeps a group split across a page drawable.
        const uint8_t style = k > 0 ? static_cast<uint8_t>(run.style | EpdFontFamily::RUBY_CONTINUE) : run.style;
        PendingChar pc{Kinsoku::verticalFormOf(baseCps[k]),
                       paragraphIndex,
                       run.fontSlot,
                       static_cast<uint32_t>(baseOffsets[k]),
                       style,
                       run.emphasis,
                       run.indentQuarters};
        pc.scale8 = run.scale8;
        pc.visibleTextOffset = run.visibleTextOffset + baseCpIndex[k];
        setRuby(pc, slice.data(), slice.size());
        stream_.push_back(pc);
      }
    }

    for (size_t relIdx : breakBeforeBaseIndex) {
      recordParagraphBreakAt(runStartStreamIndex + relIdx);
    }
  }
}

int verticalCellBaselineOffset(const GfxRenderer& renderer, const int fontId, const int cellPx, bool* measured) {
  const int ascender = renderer.getFontAscenderSize(fontId);
  // String literal, no allocation -- this is called from the pagination path.
  renderer.ensureSdCardFontReady(fontId, "\xe4\xb8\xad", 0x01);
  GlyphInk ink;
  const bool ok =
      measureGlyphInk(renderer, fontId, 0x4E2D /* 中 */, 0, &ink) && ink.height > 0 && ink.height <= cellPx * 2;
  if (measured) *measured = ok;
  if (!ok) return ascender;
  // getGlyphMetrics reports `top` as the ink top ABOVE the baseline, so centring the ink box in
  // the cell puts the baseline at (cell - inkHeight)/2 + top below the cell's top edge.
  return (cellPx - ink.height) / 2 + ink.top;
}

void VerticalParsedText::addInlineImage(std::string info, const uint16_t columns, const int widthPx) {
  if (inlineImages_.size() >= 0x10000 || !canPushStreamChar()) return;
  const uint32_t id = static_cast<uint32_t>(inlineImages_.size());
  inlineImages_.push_back({std::move(info), columns, static_cast<int16_t>(widthPx)});
  // Its own paragraph: text before it ends its column, text after starts a fresh one.
  const uint32_t paragraphIndex = static_cast<uint32_t>(paragraphBreaksBeforeIndex_.size());
  paragraphBreaksBeforeIndex_.push_back(stream_.size());
  PendingChar pc{};
  pc.codepoint = IMAGE_MARKER_BASE + id;
  pc.paragraphIndex = paragraphIndex;
  stream_.push_back(std::move(pc));
  paragraphBreaksBeforeIndex_.push_back(stream_.size());
}

int VerticalParsedText::columnAdvancePx() const { return verticalCellPx(renderer_, fontId_) + columnGapPx_; }
