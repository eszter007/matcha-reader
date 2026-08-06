#include "VerticalParsedText.h"

#include <Arduino.h>
#include <Logging.h>
#include <Utf8.h>

#include <algorithm>
#include <cmath>

#include "GfxRenderer.h"
#include "Kinsoku.h"

namespace {
// A reserve() big enough to satisfy this margin should always succeed even under pressure --
// below it, skip reserving and let the vector grow incrementally (smaller, more-likely-to-succeed
// allocations) rather than attempt one large upfront allocation that's more likely to fail outright.
//
// Was 32KB, then 16KB -- both confirmed on a real device to be actively counterproductive for
// large legitimate requests. At 32KB: a furigana-dense paragraph's bulk stream_ reserve needed
// 38600 bytes with 65524 available (comfortably enough) but was skipped because 38600+32768
// exceeded 65524. At 16KB: the per-page glyphs reserve (13824 bytes) was refused with 29684 free
// because 13824+16384 overshot by 524 bytes. Each refusal forces incremental doubling growth --
// hundreds to 1000+ separate reallocations for the same data -- which fragments the heap far
// worse (a measured ~22KB net loss) than the single bulk reserve it was "protecting" against.
// The getMaxAllocHeap() check already guarantees the reserve() itself succeeds; this constant is
// only cushion for OTHER allocations during the build, and while a chapter build runs the rest of
// the app is quiescent (largest concurrent needs: SD write buffers and log lines, low KBs). 4KB
// lets the real page buffer win over fragmenting incremental growth.
constexpr uint32_t MIN_FREE_HEAP_FOR_RESERVE = 4 * 1024;

// Cushion left for the rest of the app when growing a vector one element at a time, as opposed
// to MIN_FREE_HEAP_FOR_RESERVE's cushion for a bulk reserve.
constexpr uint32_t SMALL_ALLOC_MARGIN = 8 * 1024;

// Every reserve() here is one contiguous allocation that ABORTS the process on failure under
// -fno-exceptions, so nothing may be requested without first checking it fits. `margin` is what
// the request must leave behind for everything else.
inline bool heapCanAfford(const size_t bytes, const uint32_t margin) { return ESP.getMaxAllocHeap() >= bytes + margin; }

// Make room for one more push_back. Tries a doubling first (amortised growth), then a small
// linear step at each margin in turn, most protective first -- so a heap too tight for the
// doubled request still gets a cheap retry instead of latching. Without that fallback one
// refusal blocked every later element, since the doubled request never shrinks on its own:
// confirmed on device as the real cause of "sparse" pages.
// A margin of 0 is a deliberate last resort where the alternative is losing content outright.
template <typename T, size_t N>
bool growForOnePush(std::vector<T>& vec, const uint32_t (&margins)[N], const size_t linearStep) {
  if (vec.size() < vec.capacity()) return true;  // no reallocation needed, cheap path
  const size_t doubled = vec.capacity() == 0 ? 1 : vec.capacity() * 2;
  if (heapCanAfford(doubled * sizeof(T), margins[0])) {
    vec.reserve(doubled);
    return true;
  }
  const size_t linear = vec.capacity() + linearStep;
  for (size_t i = 0; i < N; i++) {
    if (heapCanAfford(linear * sizeof(T), margins[i])) {
      vec.reserve(linear);
      return true;
    }
  }
  return false;
}
}  // namespace

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

// Ink boxes for the handful of codepoints placed by quadrant. A miss is NOT stored: a probe
// against a not-yet-resident SD font fails transiently, and caching that would place every
// later occurrence by the fallback for the rest of the chapter.
struct InkMemo {
  static constexpr int CAPACITY = 24;
  uint32_t keys[CAPACITY] = {};  // codepoint | style << 24; 0 = free
  GlyphInk inks[CAPACITY];
  int count = 0;
  static uint32_t keyOf(const uint32_t cp, const uint8_t style) { return cp | (static_cast<uint32_t>(style) << 24); }
  bool lookup(const uint32_t cp, const uint8_t style, GlyphInk* out) const {
    const uint32_t k = keyOf(cp, style);
    for (int i = 0; i < count; i++) {
      if (keys[i] == k) {
        *out = inks[i];
        return true;
      }
    }
    return false;
  }
  void store(const uint32_t cp, const uint8_t style, const GlyphInk& ink) {
    if (count >= CAPACITY) return;  // full: fall back to probing, never evict
    keys[count] = keyOf(cp, style);
    inks[count] = ink;
    count++;
  }
};

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
  const bool ok = measureGlyphInk(renderer, fontId, 0x6F22 /* 漢 */, 0, &ink) && ink.height > 0 && ink.height <= cellPx;
  if (measured) *measured = ok;
  return ok ? cellPx - ink.height : 0;
}

// Record a paragraph boundary before stream index `idx`, ignoring a repeat at the same index --
// several sources (a source newline, a run boundary, a carried-over trailing break) can each
// report the same boundary, and a duplicate would open an extra empty column.
void VerticalParsedText::recordParagraphBreakAt(const size_t idx) {
  if (!paragraphBreaksBeforeIndex_.empty() && paragraphBreaksBeforeIndex_.back() == idx) return;
  paragraphBreaksBeforeIndex_.push_back(idx);
}

void VerticalParsedText::reserveStreamFor(size_t utf8Bytes) {
  // CJK prose is ~3 UTF-8 bytes per codepoint, so bytes/3 (+ slack for embedded ASCII) closely
  // estimates the PendingChar slots this text needs. An earlier version of this reserve used the
  // raw byte count as the slot count -- a 3x over-request at 32 bytes per slot (~96 bytes reserved
  // per actual character), which crashed a real device. The affordability check below is on the
  // REQUEST size, not just current free heap: reserve() is one contiguous allocation that aborts
  // the process on failure under -fno-exceptions, so a request that doesn't comfortably fit is
  // skipped entirely -- incremental push_back growth (guarded by canPushStreamChar) is the
  // lower-risk path once memory is tight.
  const size_t slots = utf8Bytes / 3 + 8;
  const size_t needed = stream_.size() + slots;
  if (needed <= stream_.capacity()) return;
  const size_t requestBytes = needed * sizeof(PendingChar);
  if (!heapCanAfford(requestBytes, MIN_FREE_HEAP_FOR_RESERVE)) {
    LOG_ERR("VPT", "Reserve of %u bytes doesn't fit (free=%u); growing incrementally",
            static_cast<unsigned>(requestBytes), ESP.getMaxAllocHeap());
    return;
  }
  stream_.reserve(needed);
}

void VerticalParsedText::preallocateStream() {
  constexpr size_t STREAM_STABLE_ENTRIES = 512;
  const size_t bytes = STREAM_STABLE_ENTRIES * sizeof(PendingChar);
  if (stream_.capacity() >= STREAM_STABLE_ENTRIES) return;
  if (heapCanAfford(bytes, MIN_FREE_HEAP_FOR_RESERVE)) {
    stream_.reserve(STREAM_STABLE_ENTRIES);
  } else {
    LOG_ERR("VPT", "preallocateStream: %u bytes don't fit (maxAlloc=%u); falling back to incremental growth",
            static_cast<unsigned>(bytes), ESP.getMaxAllocHeap());
  }
}

bool VerticalParsedText::canPushStreamChar() {
  if (oom_) return false;
  static constexpr uint32_t MARGINS[] = {SMALL_ALLOC_MARGIN};
  constexpr size_t LINEAR_GROWTH_STEP = 64;  // PendingChar elements; keeps stalled retries cheap
  if (growForOnePush(stream_, MARGINS, LINEAR_GROWTH_STEP)) return true;
  LOG_ERR("VPT", "Low heap (%u bytes) while building vertical text stream; truncating batch", ESP.getMaxAllocHeap());
  oom_ = true;
  everDroppedForHeap_ = true;
  return false;
}

void VerticalParsedText::addParagraph(const std::string& utf8Text) {
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
    // Note: a plain space is deliberately NOT skipped here even though CJK prose
    // itself never uses inter-word spaces, because Kinsoku::
    // isRotatedRunCharacter() now treats ' ' as part of a Latin run --
    // dropping it here would merge multi-word embedded English phrases
    // ("CrossPoint Reader") into one unreadable token
    // ("CrossPointReader"). A stray space between two CJK characters
    // (rare, but it happens in some EPUB markup) just renders as a
    // harmless near-invisible 1-character rotated "run".
    if (cp == '\t') {
      i += consumed;
      continue;
    }
    if (!canPushStreamChar()) return;
    stream_.push_back(PendingChar{cp, paragraphIndex, static_cast<uint32_t>(i), 0, false, {}});
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

    // Decode base text into codepoints, then distribute ruby across them.
    // These are fresh, unreserved local vectors on every run -- for a furigana-dense paragraph
    // (many short RubyRun entries, one per annotated word) that's several unguarded doubling-growth
    // vectors PER RUN, repeated for every run in the paragraph. Confirmed on a real device as a
    // major, previously-unaccounted-for contributor: a single 26-run/2.9KB paragraph cost ~20KB of
    // contiguous heap inside this function alone. Reserving by byte count (a safe upper bound on
    // codepoint count for UTF-8) eliminates that internal churn; the vectors are still freed at the
    // end of each loop iteration since they're loop-local, so this doesn't increase steady-state
    // memory, only removes the many small alloc/realloc/free cycles getting there.
    std::vector<size_t> baseOffsets;
    std::vector<uint32_t> baseCps;
    std::vector<size_t> breakBeforeBaseIndex;
    baseOffsets.reserve(run.baseText.size());
    baseCps.reserve(run.baseText.size());
    {
      size_t i = 0;
      while (i < run.baseText.size()) {
        size_t consumed = 1;
        const uint32_t cp = decodeUtf8At(run.baseText, i, &consumed);
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
        stream_.push_back(PendingChar{
            baseCps[k], paragraphIndex, static_cast<uint32_t>(baseOffsets[k]), run.style, run.emphasis, {}});
      }
    } else {
      // Decode ruby codepoints to distribute evenly across base characters.
      std::vector<uint32_t> rubyCps;
      rubyCps.reserve(run.rubyText.size());
      {
        size_t ri = 0;
        while (ri < run.rubyText.size()) {
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
        stream_.push_back(PendingChar{baseCps[k], paragraphIndex, static_cast<uint32_t>(baseOffsets[k]), run.style,
                                      run.emphasis, std::move(slice)});
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

std::vector<VerticalPage> VerticalParsedText::layoutPages(void* ctx, PageReadyCallback onPageReady, bool isFinalFlush) {
  std::vector<VerticalPage> pages;
  // A break recorded at exactly stream-end (a trailing '\n' in this batch's last run) can never
  // fire in the loop below (it visits idx < stream size). Carry it across the caller's reset()
  // as a flag; addAnnotatedParagraph() re-records it at the next batch's start. Without this,
  // paragraph boundaries landing exactly on a batch boundary silently merged -- confirmed with
  // the full-pipeline host repro on a real chapter (books that wrap chapters in one <div> feed
  // whole paragraphs as embedded newlines, so this fired constantly).
  if (!paragraphBreaksBeforeIndex_.empty() && paragraphBreaksBeforeIndex_.back() == stream_.size() &&
      !stream_.empty()) {
    pendingTrailingBreak_ = true;
  }
  // Nothing new to lay out AND nothing left over from a previous non-final call to finalize.
  if (stream_.empty() && !(isFinalFlush && pendingPageValid_)) return pages;

  // Coordinate convention, uniform for every RenderKind: VerticalGlyph::y is the TOP of the
  // glyph's cell, never a baseline -- baselines belong to drawing, which converts per draw call
  // (see VerticalTextBlock::drawGlyphs). Ink offsets come from verticalCellBaselineOffset().
  // Measured once per chapter, not per paragraph -- see the memo fields' comment.
  bool metricOk = false;
  int cellPxNow = cellPxMemo_;
  if (cellPxNow <= 0) {
    cellPxNow = verticalCellPx(renderer_, fontId_, &metricOk);
    if (metricOk) cellPxMemo_ = cellPxNow;
  }
  const int cellPx = std::max(1, cellPxNow);
  const int columnAdvancePx = cellPx + columnGapPx_;
  // What a normal grid-adjacent pair leaves between its ink boxes; off-grid runs match it.
  int inkGapNow = inkGapPxMemo_;
  if (inkGapNow < 0) {
    metricOk = false;
    inkGapNow = verticalNominalInkGapPx(renderer_, fontId_, cellPx, &metricOk);
    if (metricOk) inkGapPxMemo_ = inkGapNow;
  }
  const int inkGapPx = inkGapNow;
  const int ascender = renderer_.getFontAscenderSize(fontId_);
  // As many whole cells as the text area's height allows. The last row's ink hangs a few px past
  // its cell; that goes in the margin below (screenMargin + status bar), just as ruby goes in the
  // margin beside the text (JLREQ Fig 2.37). Reserving it here instead costs a whole character
  // per column whenever the height divides evenly.
  int baselineNow = baselineInCellMemo_;
  if (baselineNow < 0) {
    metricOk = false;
    baselineNow = verticalCellBaselineOffset(renderer_, fontId_, cellPx, &metricOk);
    if (metricOk) baselineInCellMemo_ = baselineNow;
  }
  const int baselineInCellPx = baselineNow;
  const uint16_t rowsPerColumn = static_cast<uint16_t>(std::max(1, static_cast<int>(viewportHeight_) / cellPx));
  const int usableWidthPx = std::max(cellPx, static_cast<int>(viewportWidth_) - rightPaddingPx_);
  const uint16_t columnsPerPage = static_cast<uint16_t>(std::max(1, usableWidthPx / columnAdvancePx));
  // Columns are anchored to the right edge and march leftwards, so the width that does not divide
  // evenly into columns would pile up as dead space on the LEFT. Split it between the two margins
  // instead of adding it to the gaps: the gap is 行間, set in quarter ems by the line-spacing
  // setting (and by whether furigana needs room in it), so widening it here overrides the very
  // rule that chose it -- measurably, a 21px gap became 25px against a 29px em.
  // (x stays >= 0: the leftmost column sits at leftover - leftover/2.)
  const int leftoverPx = std::max(0, usableWidthPx - cellPx - (columnsPerPage - 1) * columnAdvancePx);
  const int blockShiftPx = leftoverPx / 2;

  // Index into paragraphBreaksBeforeIndex_ of the *next* paragraph start,
  // so we know when we've crossed into a new paragraph and should force a
  // fresh column. For a chapter-fresh call (no pending page), index 0 is the chapter's very
  // first paragraph, already "started" at the top of a fresh page -- skip it. For a RESUMED
  // call (pendingPageValid_, checked before the init block below sets it), a break recorded at
  // stream index 0 is a real paragraph starting exactly at the batch boundary and must force
  // its fresh column like any other; continuation chunks no longer record one (see
  // addAnnotatedParagraph), so honoring it can't split a paragraph mid-flow anymore.
  size_t nextParagraphBreakIdx = pendingPageValid_ ? 0 : 1;

  // Snapshot the geometry for box-rect building: finalizePendingPage() runs OUTSIDE this
  // function and must still be able to close an open box on the final page.
  boxGeomCellPx_ = cellPx;
  boxGeomColumnAdvancePx_ = columnAdvancePx;
  // Fold the block shift in here: appendBoxRectToPage's colLeft is columnLeftX with the shift
  // already applied, so the two must not drift apart.
  boxGeomUsableWidthPx_ = usableWidthPx - blockShiftPx;
  boxGeomRowsPerColumn_ = rowsPerColumn;

  // Re-record box markers carried across a batch boundary (see reset()) at index 0.
  if (boxEndCarry_) {
    boxEndsBeforeIndex_.insert(boxEndsBeforeIndex_.begin(), 0);
    boxEndCarry_ = false;
  }
  if (boxStartCarry_) {
    boxStartsBeforeIndex_.insert(boxStartsBeforeIndex_.begin(), 0);
    boxStartCarry_ = false;
  }
  size_t nextBoxStartIdx = 0;
  size_t nextBoxEndIdx = 0;

  // One page's cell grid is fixed by screen geometry -- reserving it up front turns what used to
  // be several dozen incremental (and, on a fragmented heap, crash-prone) reallocations per page
  // into a single allocation. Confirmed via a real device crash inside this exact glyphs vector's
  // reallocation, even with ~97KB nominally free (heap fragmentation, not exhaustion).
  const size_t glyphsPerPage = static_cast<size_t>(columnsPerPage) * rowsPerColumn;

  // Worst case for `pages`: every column is forced to end after a single row (rowsPerColumn=1,
  // or every character forces a fresh column via a paragraph/line break) -- i.e. one page per
  // `columnsPerPage` characters in this batch. A fixed guess (previously 4) undercounted on
  // narrower columns and still crashed inside this same reallocation; computing the real bound
  // costs nothing since VerticalPage itself is small (~80 bytes) even when over-reserved.
  {
    const size_t worstCasePages = stream_.size() / std::max<size_t>(1, columnsPerPage) + 2;
    const size_t requestBytes = worstCasePages * sizeof(VerticalPage);
    if (heapCanAfford(requestBytes, MIN_FREE_HEAP_FOR_RESERVE)) {
      pages.reserve(worstCasePages);
    } else {
      LOG_ERR("VPT", "Skipping pages reserve (%u bytes doesn't fit, free=%u); growing incrementally",
              static_cast<unsigned>(requestBytes), ESP.getMaxAllocHeap());
    }
  }

  // Every reserve() below this point is a single contiguous allocation that aborts the whole
  // process on failure under -fno-exceptions -- confirmed via a real device crash inside this
  // exact reserve, immediately after the *previous* page was pushed (which can itself burst
  // memory use during its own relocation). Check the request against free heap every time.
  auto reservePageGlyphs = [&](VerticalPage& p) {
    const size_t requestBytes = glyphsPerPage * sizeof(VerticalGlyph);
    if (heapCanAfford(requestBytes, MIN_FREE_HEAP_FOR_RESERVE)) {
      p.glyphs.reserve(glyphsPerPage);
      return;
    }
    // Partial fallback: reserve the largest fitting fraction of the page instead of nothing.
    // Starting from capacity 0 makes the vector double 1-2-4-...-256, and every doubling's
    // grow-copy (old + new buffer coexisting) is a fresh chance to land on a heap dip -- on
    // the X3's tighter heap that fired the emergency page split constantly, which the reader
    // sees as pages breaking at random fill levels. This runs right after the previous page
    // was flushed and freed (the heap's local best), so one medium block now prevents most of
    // the risky growth steps later at the dips.
    for (size_t fraction = 2; fraction <= 8; fraction *= 2) {
      const size_t partial = glyphsPerPage / fraction;
      if (partial < 32) break;
      if (heapCanAfford(partial * sizeof(VerticalGlyph), MIN_FREE_HEAP_FOR_RESERVE)) {
        p.glyphs.reserve(partial);
        LOG_DBG("VPT", "Partial page glyphs reserve: %u/%u elements (maxAlloc=%u)", static_cast<unsigned>(partial),
                static_cast<unsigned>(glyphsPerPage), ESP.getMaxAllocHeap());
        return;
      }
    }
    LOG_ERR("VPT", "Skipping page glyphs reserve (%u bytes doesn't fit, free=%u); growing incrementally",
            static_cast<unsigned>(requestBytes), ESP.getMaxAllocHeap());
  };

  // Skipping the reserve above is only safe if every individual push_back is ALSO guarded --
  // "grows incrementally" isn't automatically safe on a heap this tight, and a real device crash
  // confirmed exactly that: the skipped-reserve fallback still aborted inside the first
  // push_back's own reallocation. Only checks free heap when a reallocation is actually imminent
  // (size == capacity), so this is cheap in the (now common, thanks to reservePageGlyphs) case
  // where headroom already covers the push. Drops the glyph (visually a rare missing character in
  // an extreme low-memory tail case) rather than crash the whole device.
  //
  // The check must be against the ACTUAL next allocation size, not a flat margin: vector growth
  // roughly doubles capacity each time, so the very first push from empty needs ~1 element
  // (~50 bytes) while a push near a nearly-full page needs nearly as much as the original bulk
  // reserve. A real device crash confirmed the failure mode of getting this wrong: using
  // MIN_FREE_HEAP_FOR_RESERVE (32KB) as a flat per-glyph margin meant that once free heap sat
  // anywhere below 32KB -- which is otherwise completely survivable for a ~50-byte push -- every
  // single glyph was dropped, silently blanking entire pages.
  // Exponential (x2) growth here is a trap once the heap is tight: if one doubling attempt fails,
  // capacity stays put, so *every* subsequent push_back needs that exact same (large, ever-doubling)
  // contiguous block and fails identically -- silently dropping every remaining glyph on the page,
  // not just the one that triggered it. This is what produced the "sparse page" bug reports: a
  // single transient dip below the doubled-capacity requirement blanked the rest of the page.
  // Falling back to a small LINEAR growth step once doubling would be too big keeps each retry's
  // request small and roughly constant, so a later push (after some other allocation frees up) has
  // a real chance to succeed instead of being permanently walled off behind the same big ask.
  // An embedded Latin word reserves whole cells, so the leftover of that rounding (up to a
  // full cell) landed as dead space before the next character. Instead of moving that one
  // character -- which only pushes the hole one position further along (device photo:
  // "Lombroso なんぞ", gap between な and ん) -- the whole rest of the column slides up by
  // the same amount. Spacing stays even, nothing collides, and only this column's tail sits
  // slightly higher than its neighbours.
  int columnYShift = 0;
  uint16_t shiftColumn = UINT16_MAX;

  auto pushGlyph = [this, &columnYShift, &shiftColumn](VerticalPage& pg, VerticalGlyph g,
                                                       const std::string& text = std::string()) {
    std::vector<VerticalGlyph>& glyphs = pg.glyphs;
    // Intern before the push: on a dropped glyph the orphaned pool entry is a few wasted
    // bytes, while interning after would need the glyph's index to patch -- not worth it.
    if (!text.empty()) g.textId = pg.internText(text);
    if (g.column != shiftColumn) {
      columnYShift = 0;
      shiftColumn = g.column;
    } else if (columnYShift != 0) {
      // Positive slides the column tail up (cell-rounding leftover after a run), negative
      // slides it down (ink that leaves its own cell, e.g. the low ellipsis dot stack).
      g.y = static_cast<uint16_t>(std::max(0, static_cast<int>(g.y) - columnYShift));
    }
    // Tiers, most protective first. The 4K tier: device evidence (450+ page chapter) showed
    // maxAlloc dips bottoming at 14324 while the linear request sat at ~9216 -- the 8K margin
    // missed by 12 bytes, dropped glyphs, and the stale mark re-indexed the chapter on every
    // open. The 0 tier: at that point the margin protects only allocations that fail gracefully
    // and retry (font advance tables, staging buffers, the next page's reserve), while a dropped
    // glyph is a character permanently missing from the page.
    static constexpr uint32_t MARGINS[] = {SMALL_ALLOC_MARGIN, 4 * 1024, 0};
    constexpr size_t LINEAR_GROWTH_STEP = 16;  // elements; keeps a stalled page's retries cheap
    if (growForOnePush(glyphs, MARGINS, LINEAR_GROWTH_STEP)) {
      glyphs.push_back(g);
      return true;
    }
    LOG_ERR("VPT", "Low heap (%u bytes); dropping glyph", ESP.getMaxAllocHeap());
    everDroppedForHeap_ = true;
    return false;
  };

  // First call ever (or first since the last isFinalFlush=true call): start a fresh page. A
  // resumed call (pendingPageValid_ already true) picks up exactly where the previous non-final
  // call left off -- same page object, same column/row -- so a batch boundary never truncates a
  // page that isn't actually full.
  if (!pendingPageValid_) {
    pendingPage_ = VerticalPage{};
    pendingPage_.columnCount = columnsPerPage;
    pendingPage_.rowsPerColumn = rowsPerColumn;
    reservePageGlyphs(pendingPage_);
    pendingColumn_ = 0;
    pendingRow_ = 0;
    pendingPageValid_ = true;
  }
  VerticalPage& page = pendingPage_;
  uint16_t& column = pendingColumn_;
  uint16_t& row = pendingRow_;

  auto columnLeftX = [&](uint16_t col) -> int { return usableWidthPx - cellPx - blockShiftPx - col * columnAdvancePx; };

  // Row where a fresh column starts: 0 normally; inside a styled block, the block's start
  // offset (start-Xem), plus the hanging indent (h-indent-Xem) when the column continues a
  // wrapped paragraph line rather than beginning a new paragraph.
  auto columnStartRow = [&](const bool paragraphStart) -> uint16_t {
    if (!inBox_) return 0;
    const int startRows = static_cast<int>(activeBlock_.startEm + 0.5f);
    const int hangRows = paragraphStart ? 0 : static_cast<int>(activeBlock_.hangEm + 0.5f);
    return static_cast<uint16_t>(std::min<int>(rowsPerColumn - 1, std::max(0, startRows + hangRows)));
  };

  auto finalizePageIfNeeded = [&]() {
    if (column >= columnsPerPage) {
      // A box spanning the page boundary gets one rect per page: close it at this page's last
      // column and continue from column 0 on the next page.
      if (inBox_) {
        appendBoxRectToPage(page, boxStartCol_, static_cast<uint16_t>(columnsPerPage - 1),
                            /*openLeft=*/true, /*openRight=*/boxContinuedFromPrevPage_);
        if (activeBlock_.alignCenter) {
          centerBlockColumns(page, boxStartCol_, static_cast<uint16_t>(columnsPerPage - 1));
        }
        boxStartCol_ = 0;
        boxContinuedFromPrevPage_ = true;
      }
      pages.push_back(std::move(page));
      anyPageEverProduced_ = true;
      // Stream out everything except the single most-recently-completed page: the oikomi
      // pull-back check below only ever looks at pages.back() (the page immediately before the
      // one currently being started), so once THIS page is pushed, any earlier page in `pages`
      // can never be touched again and is safe to write out and free now.
      if (onPageReady) {
        while (pages.size() > 1) {
          onPageReady(ctx, std::move(pages.front()));
          pages.erase(pages.begin());
        }
      }
      page = VerticalPage{};
      page.columnCount = columnsPerPage;
      page.rowsPerColumn = rowsPerColumn;
      reservePageGlyphs(page);
      column = 0;
      row = 0;
    }
  };

  // JLREQ 3.1.4, consecutive brackets / commas / full stops / middle dots. Each is a half-em
  // glyph occupying one half of its em, which is what decides the white between two neighbours:
  //
  //   first half (top, in vertical)  - closing brackets, 、。，．
  //   second half (bottom)           - opening brackets
  //   centre                         - middle dot ・
  auto punctEmHalf = [](const uint32_t cp) -> int {  // 0 none, 1 first half, 2 second half, 3 centre
    if (cp == 0x30FB || cp == 0x00B7) return 3;      // ・
    switch (Kinsoku::verticalShiftType(cp)) {
      case 1:
        return 1;  // 、。，．
      case 2:
        return 1;  // closing brackets
      case 3:
        return 2;  // opening brackets
      default:
        return 0;
    }
  };
  auto applyPunctPairSpacing = [&](const uint32_t cp, const uint16_t columnArg) {
    if (page.glyphs.empty()) return;
    const VerticalGlyph& prev = page.glyphs.back();
    if (prev.column != columnArg || prev.codepoint == 0) return;
    const int prevHalf = punctEmHalf(prev.codepoint);
    const int thisHalf = punctEmHalf(cp);
    if (prevHalf == 0 || thisHalf == 0) return;

    // Every pair in the figures closes up by a half em: 、「 and 」「 from a full em to a half
    // (figures 3, 4); 。」, ）。, 「『, ）」 from a half em to solid (1, 2, 5, 6); 」・「 from three
    // quarters to a quarter each side of the dot (7).
    int reduction = cellPx / 2;

    // 。and 、 already end a half em after their own ink (commaTighten), so that half is gone:
    // an opening bracket wants exactly it (figure 3), a closing bracket or another 。/、 is set
    // solid (1, 2), a middle dot keeps a quarter.
    if (Kinsoku::verticalShiftType(prev.codepoint) == 1) {
      if (thisHalf == 2) return;
      reduction = (thisHalf == 3) ? cellPx / 4 : cellPx / 2;
    }
    columnYShift += reduction;
    shiftColumn = columnArg;
  };

  InkMemo inkMemo;

  // Upper-RIGHT quadrant placement, from the glyph's own ink box: these marks are drawn with
  // their ink at the bottom-left of the em, so how far it must travel is font-specific. Returns
  // false when metrics are unavailable.
  //
  // centreInHalf distinguishes the two users:
  //   。、        - half-width advance, so the ink is centred in a half-em box at the cell's head
  //   small kana - letter face against the top edge of its box
  auto quadrantTopRight = [&](const uint32_t cp, const uint8_t style, const uint16_t col, const uint16_t rowIdx,
                              const bool centreInHalf, int* gxOut, int* gyOut, int* inkHeightOut = nullptr) -> bool {
    // Only 。、 and the small kana reach here -- about fifteen codepoints per style for a whole
    // chapter, each otherwise re-probed for every occurrence. A probe pages the glyph in from the
    // SD font and evicts another from its small on-demand table, so the repeat cost is SD reads,
    // not arithmetic.
    GlyphInk ink;
    if (!inkMemo.lookup(cp, style, &ink)) {
      if (!measureGlyphInk(renderer_, fontId_, cp, style, &ink)) return false;
      inkMemo.store(cp, style, ink);
    }
    if (ink.width <= 0 || ink.height <= 0) return false;
    // Draw resolves ink left as x + glyph->left, and ink top as (cellTop + baselineInCell) - top.
    const int inkTopInCell = centreInHalf ? std::max(0, (cellPx / 2 - ink.height) / 2) : 0;
    *gxOut = columnLeftX(col) + cellPx - ink.left - ink.width;
    *gyOut = std::max(0, rowIdx * cellPx + inkTopInCell + ink.top - baselineInCellPx);
    if (inkHeightOut) *inkHeightOut = inkTopInCell + ink.height;
    return true;
  };

  auto placeUprightAt = [&](const PendingChar& pc, uint16_t col, uint16_t rowIdx) {
    applyPunctPairSpacing(pc.codepoint, col);
    VerticalGlyph g;
    g.codepoint = pc.codepoint;
    g.column = col;
    g.row = rowIdx;
    g.paragraphIndex = pc.paragraphIndex;
    g.byteOffset = pc.byteOffset;
    g.style = pc.style;
    g.emphasis = pc.emphasis;

    if (Kinsoku::needsVerticalRotation(pc.codepoint)) {
      // Bracket / dash / chōonpu: keep one-cell layout, but mark as rotated
      // punctuation so the renderer can center it by glyph metrics and apply
      // opening/closing bracket flow-direction bias.
      g.x = static_cast<uint16_t>(columnLeftX(col));
      g.y = static_cast<uint16_t>(rowIdx * cellPx);
      g.renderKind = VerticalGlyph::RotatedPunct;
      // JLREQ: an opening bracket at the LINE HEAD is set flush to it (tentsuki), the half em
      // before it deleted. "Line head" is the first glyph of THIS column, not row 0 -- a styled
      // block (CSS indent, hanging indent) opens its column further down, and dialogue paragraphs
      // are typically indented. Checked before the push, when the bracket is not yet the last.
      const bool atLineHead = page.glyphs.empty() || page.glyphs.back().column != col;
      const bool flushOpeningBracket = atLineHead && Kinsoku::verticalShiftType(pc.codepoint) == 3;
      if (flushOpeningBracket) g.lineHeadFlush = 1;
      pushGlyph(page, g, pc.rubyText);
      if (flushOpeningBracket) {
        // The half em it no longer needs comes off the rest of the column.
        columnYShift += cellPx / 2;
        shiftColumn = col;
      }
      return;
    }

    if (Kinsoku::isSmallKana(pc.codepoint)) {
      // Small kana (っゃゅょ ィ ョ) sit upper-right, letter face against the TOP edge of the box.
      int qx = 0, qy = 0;
      if (quadrantTopRight(pc.codepoint, pc.style, col, rowIdx, /*centreInHalf=*/false, &qx, &qy)) {
        g.x = static_cast<uint16_t>(qx);
        g.y = static_cast<uint16_t>(qy);
      } else {
        g.x = static_cast<uint16_t>(columnLeftX(col) + std::max(1, cellPx / 8));
        g.y = static_cast<uint16_t>(rowIdx * cellPx);
      }
      g.renderKind = VerticalGlyph::Upright;
      pushGlyph(page, g, pc.rubyText);
      return;
    }

    int gx = columnLeftX(col);
    int gy = rowIdx * cellPx;
    if (pc.codepoint >= '0' && pc.codepoint <= '9') {
      GlyphInk ink;
      if (measureGlyphInk(renderer_, fontId_, pc.codepoint, pc.style, &ink)) {
        gx = columnLeftX(col) + (cellPx - ink.width) / 2 - ink.left;
      }
    }
    int commaTighten = 0;
    if (Kinsoku::verticalShiftType(pc.codepoint) == 1) {
      // Comma/period: bottom-left of the em -> upper-right of the cell.
      int qx = 0, qy = 0, inkH = 0;
      if (quadrantTopRight(pc.codepoint, pc.style, col, rowIdx, /*centreInHalf=*/true, &qx, &qy, &inkH)) {
        gx = qx;
        gy = qy;
        // Not a whole square: the mark takes its own ink height plus a half em, then the next
        // character follows. The remainder of the cell comes off the rest of the column.
        commaTighten = std::max(0, cellPx - (inkH + cellPx / 2));
      } else {
        gx += cellPx / 2;
        gy = std::max(0, gy - cellPx / 2);
      }
    }
    g.x = static_cast<uint16_t>(gx);
    g.y = static_cast<uint16_t>(gy);
    g.renderKind = VerticalGlyph::Upright;
    pushGlyph(page, g, pc.rubyText);
    if (commaTighten > 0) {
      columnYShift += commaTighten;
      shiftColumn = col;
    }
  };

  auto placeUpright = [&](const PendingChar& pc) { placeUprightAt(pc, column, row); };
  auto isAsciiDigit = [](uint32_t cp) {
    return (cp >= '0' && cp <= '9') ||      // ASCII digits: U+0030-U+0039
           (cp >= 0xFF10 && cp <= 0xFF19);  // Fullwidth digits: U+FF10-U+FF19
  };
  auto isAsciiAlnum = [](uint32_t cp) {
    return (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z');
  };
  // Exclamation/question marks that books put in tate-chu-yoko pairs (!? / !!).
  // Halfwidth only: fullwidth ！？ already render upright as normal CJK cells.
  auto isBangOrQuestion = [](uint32_t cp) { return cp == '!' || cp == '?'; };
  // Place a two-character tate-chu-yoko run (a 2-digit number like 26, or a
  // !?/!! pair) upright in a single cell, ink-centered on the column, and
  // advance row/column past it. Shared by the digit and punctuation-pair
  // branches in the loop below.
  auto placeTcyPairAt = [&](size_t i0) {
    std::string runUtf8;
    utf8AppendCodepoint(stream_[i0].codepoint, runUtf8);
    utf8AppendCodepoint(stream_[i0 + 1].codepoint, runUtf8);
    // Measure with the pair's ACTUAL style -- rendered with g.style, and bold digits are
    // wider, so an unstyled measurement mis-centers the pair in its cell.
    const auto tcyStyle = static_cast<EpdFontFamily::Style>(stream_[i0].style);
    const auto tcyStyleBit = static_cast<uint8_t>(1u << (stream_[i0].style & 3));
    renderer_.ensureSdCardFontReady(fontId_, runUtf8.c_str(), tcyStyleBit);
    const int runWidthPx = renderer_.getRenderAdvanceX(fontId_, runUtf8.c_str(), tcyStyle);

    // Center the run on its INK box, not its advance width. drawText puts ink at
    // pen + firstGlyph.left, and digit advances carry trailing whitespace, so advance-based
    // centering sat the run visibly right of the column's kanji (device photo, 「築26年」).
    // Mirrors the single-digit ink centering in placeUprightAt.
    int runX = columnLeftX(column) + std::max(0, (cellPx - runWidthPx) / 2);
    {
      const uint32_t cpFirst = stream_[i0].codepoint;
      const uint32_t cpLast = stream_[i0 + 1].codepoint;
      int l1 = 0, w1 = 0, t1 = 0, h1 = 0, lN = 0, wN = 0, tN = 0, hN = 0;
      if (renderer_.getGlyphMetrics(fontId_, cpFirst, tcyStyle, &l1, &w1, &t1, &h1) &&
          renderer_.getGlyphMetrics(fontId_, cpLast, tcyStyle, &lN, &wN, &tN, &hN)) {
        std::string lastUtf8;
        utf8AppendCodepoint(cpLast, lastUtf8);
        const int lastAdvance = renderer_.getRenderAdvanceX(fontId_, lastUtf8.c_str(), tcyStyle);
        // Ink spans pen+l1 .. pen+(runWidthPx-lastAdvance)+lN+wN.
        const int inkWidth = (runWidthPx - lastAdvance + lN + wN) - l1;
        if (inkWidth > 0 && inkWidth <= cellPx) {
          // Align the pair's ink center with the ink center of the column's CJK glyphs rather
          // than the geometric cell center: CJK glyphs carry uneven side bearings, so pure cell
          // centering reads shifted next to them. The previous fixed nudge of
          // -max(4, cellPx/4) was tuned on one photo/font (Kyokasho, 「築26年」) and
          // over-corrected elsewhere -- device photo: 「10年」 sat a quarter cell LEFT of the
          // column in Mincho. Measure the 中 reference glyph's ink center at this size/style
          // and use its delta from the cell center. A well-formed full-width glyph cannot be
          // off-centre by more than its own side bearing, so that bounds the delta and a metrics
          // outlier can't fling the pair off-axis.
          // Falls back to plain ink centering when metrics are unavailable.
          int cjkDelta = 0;
          int kl = 0, kw = 0, kt = 0, kh = 0;
          // String literal, no allocation -- this sits in the pagination path (review finding:
          // the earlier runUtf8 + 中 concatenation allocated per pair).
          renderer_.ensureSdCardFontReady(fontId_, "\xe4\xb8\xad", tcyStyleBit);
          if (renderer_.getGlyphMetrics(fontId_, 0x4E2D /* 中 */, tcyStyle, &kl, &kw, &kt, &kh) && kw > 0) {
            cjkDelta = (kl + kw / 2) - cellPx / 2;
            const int clampPx = std::max(1, (cellPx - kw) / 2);
            if (cjkDelta > clampPx) cjkDelta = clampPx;
            if (cjkDelta < -clampPx) cjkDelta = -clampPx;
          }
          runX = columnLeftX(column) + (cellPx - inkWidth) / 2 - l1 + cjkDelta;
        }
      }
    }

    VerticalGlyph g;
    g.codepoint = 0;
    g.column = column;
    g.row = row;
    g.x = static_cast<uint16_t>(std::max(0, runX));
    g.y = static_cast<uint16_t>(row * cellPx);
    g.paragraphIndex = stream_[i0].paragraphIndex;
    g.byteOffset = stream_[i0].byteOffset;
    g.style = stream_[i0].style;
    g.renderKind = VerticalGlyph::UprightRun;
    pushGlyph(page, g, runUtf8);

    row++;
    if (row >= rowsPerColumn) {
      column++;
      row = 0;
      finalizePageIfNeeded();
      row = columnStartRow(false);
    }
  };

  // Whether the current page's glyph vector can take one more PendingChar's worth of glyphs
  // without an impossible copy-and-grow (old and new buffer must coexist during a vector
  // reallocation -- on the observed dense-ruby pages that is a 10-12KB block at exactly the
  // layout's low-heap dip). HEADROOM covers the worst single-character expansion: a sliced
  // rotated Latin run plus ruby.
  // Mirrors pushGlyph's LAST growth fallback exactly (+16 elements, zero margin) so the
  // early break fires only when the very next growth attempt would otherwise start
  // dropping -- not sooner. A bigger headroom here inflated the page count with premature
  // breaks (device evidence: breaks at 81-148 glyphs against dips the +16 step survived).
  auto pageVectorCanTakeMore = [&]() -> bool {
    constexpr size_t GROWTH_STEP = 16;  // = pushGlyph's LINEAR_GROWTH_STEP
    if (page.glyphs.size() + GROWTH_STEP <= page.glyphs.capacity()) return true;
    const size_t growBytes = (page.glyphs.capacity() + GROWTH_STEP) * sizeof(VerticalGlyph);
    return ESP.getMaxAllocHeap() >= growBytes;
  };

  // Shared placement geometry for every sideways run -- embedded Latin words AND multi-digit
  // numbers. drawText() takes the em-box TOP, so an upright glyph's ink starts (ascender -
  // top) below its y while a rotated run is drawn from its ink start; these helpers are where
  // that difference is accounted for once instead of per call site.
  //
  // Where the run's ink may start: just under the previous glyph's measured ink, or flush with
  // the cell top when nothing precedes it in this column.
  auto rotatedRunStartY = [&](const uint16_t columnArg, const int topYArg) -> int {
    if (page.glyphs.empty()) return topYArg;
    const VerticalGlyph& pg = page.glyphs.back();
    if (pg.column != columnArg) return topYArg;
    // Start on the grid; the only reason to push further is a predecessor whose ink actually
    // reaches into this cell, which is measured below.
    int startY = topYArg;
    if (pg.renderKind == VerticalGlyph::RotatedPunct) {
      // Brackets are placed from their cell box and hang roughly a full cell lower than that
      // box (「 opens the character in the NEXT cell), so a run that only stepped one cell
      // started inside the bracket's ink (device photo, 「Furz」).
      int inkTop = 0, inkHeight = 0;
      if (renderer_.verticalPunctInkBox(fontId_, pg.codepoint, static_cast<EpdFontFamily::Style>(pg.style),
                                        static_cast<int>(pg.y), cellPx, Kinsoku::verticalShiftType(pg.codepoint),
                                        &inkTop, &inkHeight)) {
        startY = std::max(startY, inkTop + inkHeight + inkGapPx);
      }
    } else if (pg.renderKind == VerticalGlyph::Upright && pg.codepoint != 0) {
      GlyphInk ink;
      if (measureGlyphInk(renderer_, fontId_, pg.codepoint, pg.style, &ink) && ink.height > 0) {
        // pg.y already carries this column's slide-up; startY is computed on the raw grid and
        // gets the same slide applied at push time, so compare in raw space.
        const int pgRawY = static_cast<int>(pg.y) + ((pg.column == shiftColumn) ? columnYShift : 0);
        startY = std::max(startY, pgRawY + baselineInCellPx - ink.top + ink.height);
      }
    }
    return startY;
  };

  // How far the character AFTER the run may be intruded upon: its ink only starts that far
  // below its own cell top. Reports the ink offset inside the cell, or -1 when unknown.
  //
  // Resolving one costs an ensureSdCardFontReady() -- a potential SD round-trip -- and a
  // chapter hits this once per embedded word, number and ellipsis, often for the same few
  // characters (は、。 dominate). A tiny direct-mapped cache keeps the repeat lookups off the
  // card without holding a map. Deliberately small: this runs on the render task, and the
  // project's stack budget for locals is 256 bytes total -- 16 entries is 96 bytes.
  struct InkOffsetCacheEntry {
    uint32_t key = 0;  // codepoint | style << 24; 0 = empty (never a real key, cp 0 is unused)
    int16_t offset = 0;
  };
  static constexpr size_t INK_CACHE_SLOTS = 16;
  InkOffsetCacheEntry inkOffsetCache[INK_CACHE_SLOTS];

  auto nextGlyphInkOffset = [&](const size_t nextIdx, const uint32_t paragraphIndex) -> int {
    if (nextIdx >= stream_.size() || stream_[nextIdx].paragraphIndex != paragraphIndex) return -1;
    const auto& next = stream_[nextIdx];
    const uint32_t key = (next.codepoint & 0x00FFFFFFu) | (static_cast<uint32_t>(next.style & 3) << 24);
    auto& slot = inkOffsetCache[key % INK_CACHE_SLOTS];
    if (slot.key == key) return slot.offset;

    const auto nextStyle = static_cast<EpdFontFamily::Style>(next.style);
    int offset = -1;
    if (Kinsoku::needsVerticalRotation(next.codepoint)) {
      int inkTop = 0, inkHeight = 0;
      if (renderer_.verticalPunctInkBox(fontId_, next.codepoint, nextStyle, 0, cellPx,
                                        Kinsoku::verticalShiftType(next.codepoint), &inkTop, &inkHeight)) {
        offset = inkTop;
      }
    } else {
      // Metrics for a glyph the SD font has not paged in yet come back empty, which silently
      // disabled the tail allowance; page the one character in first.
      char nextChar[5] = {};
      utf8EncodeCodepoint(next.codepoint, nextChar);
      renderer_.ensureSdCardFontReady(fontId_, nextChar, static_cast<uint8_t>(1u << (next.style & 3)));
      GlyphInk ink;
      if (measureGlyphInk(renderer_, fontId_, next.codepoint, next.style, &ink) && ink.height > 0) {
        offset = baselineInCellPx - ink.top;
      }
    }
    // A miss is worth caching too -- it is the expensive case, and a glyph the font lacks
    // stays missing for the rest of the chapter.
    slot = {key, static_cast<int16_t>(offset)};
    return offset;
  };

  // Rows the run occupies: enough that the next character's ink clears the run's, no more.
  auto rotatedRunRows = [&](const int startY, const int inkWidthPx, const uint16_t rowArg,
                            const int nextInkOffset) -> uint16_t {
    const int intrusion = (nextInkOffset >= 0) ? nextInkOffset : 0;
    const int endRow = static_cast<int>(std::ceil(static_cast<double>(startY + inkWidthPx - intrusion) / cellPx));
    return static_cast<uint16_t>(std::max(1, endRow - static_cast<int>(rowArg)));
  };

  // getRenderAdvanceX ends at the pen, not at the ink: the last glyph's right side bearing is
  // blank. Counting it made every run reserve up to a whole extra cell.
  auto runInkWidth = [&](const std::string& s, const int advanceWidth, const EpdFontFamily::Style style) -> int {
    if (s.empty()) return advanceWidth;
    size_t lastStart = s.size() - 1;
    while (lastStart > 0 && (static_cast<unsigned char>(s[lastStart]) & 0xC0) == 0x80) lastStart--;
    const std::string lastChar = s.substr(lastStart);
    const auto c0 = static_cast<unsigned char>(lastChar[0]);
    uint32_t lastCp = c0;
    if (c0 >= 0xC0 && lastChar.size() >= 2) {
      lastCp = static_cast<uint32_t>(c0 & 0x1F) << 6 | (static_cast<unsigned char>(lastChar[1]) & 0x3F);
    }
    GlyphInk ink;
    if (!measureGlyphInk(renderer_, fontId_, lastCp, static_cast<uint8_t>(style), &ink) || ink.width <= 0) {
      return advanceWidth;
    }
    const int lastAdvance = renderer_.getRenderAdvanceX(fontId_, lastChar.c_str(), style);
    return advanceWidth - std::max(0, lastAdvance - (ink.left + ink.width));
  };

  // Take up the cell-rounding leftover after a run: the rest of the column slides up by it, so
  // the next character follows at normal spacing instead of after a partly empty cell.
  auto takeUpRunSlack = [&](const int startY, const int inkWidthPx, const uint16_t rowAfter, const uint16_t columnArg,
                            const int nextInkOffset) {
    if (nextInkOffset < 0 || rowAfter >= rowsPerColumn) return;
    const int nextGridInkTop = rowAfter * cellPx + nextInkOffset;
    const int slack = nextGridInkTop - (startY + inkWidthPx) - inkGapPx;
    if (slack > 0) {
      columnYShift += slack;
      shiftColumn = columnArg;
    }
  };

  size_t idx = 0;
  size_t suppressKinsokuUntilIdx = 0;  // watchdog escape hatch: place plainly up to here
  // Kinsoku oidashi rewinds one character (idx--) so a prohibited pair starts a column together.
  // Re-running the SAME rewind forever is possible when the re-placed character lands right back
  // at a column end, and the alternating index defeats a naive "stuck on one index" check -- so
  // this tracks how far the layout has ever got instead. A live-locked build holds the render
  // lock and freezes the reader, so this must be a bound, not an assumption.
  size_t highWaterIdx = 0;
  int spinsSinceProgress = 0;
  while (idx < stream_.size()) {
    if (idx > highWaterIdx) {
      highWaterIdx = idx;
      spinsSinceProgress = 0;
    } else if (++spinsSinceProgress > 64) {
      LOG_ERR("VPT", "Layout made no progress past idx=%u (cp=U+%04X col=%u row=%u rows/col=%u); skipping rule",
              static_cast<unsigned>(idx), stream_[idx].codepoint, column, row, rowsPerColumn);
      spinsSinceProgress = 0;
      suppressKinsokuUntilIdx = highWaterIdx + 1;
    }
    // Emergency page split: the page's glyph vector is effectively full and the heap has no
    // block for the grow-copy. Close the page at this character boundary and continue on a
    // fresh one (whose vector starts small and grows in fragment-sized steps) -- an early
    // page break the reader barely notices, instead of the silent character loss that
    // followed once pushGlyph's growth ladder was exhausted mid-page.
    if (column < columnsPerPage && !page.glyphs.empty() && !pageVectorCanTakeMore()) {
      LOG_INF("VPT", "Page glyph buffer cannot grow (%u glyphs, maxAlloc=%u); early page break",
              static_cast<unsigned>(page.glyphs.size()), ESP.getMaxAllocHeap());
      everSplitForHeap_ = true;
      column = columnsPerPage;
      finalizePageIfNeeded();
      row = columnStartRow(false);
    }

    const PendingChar& pc = stream_[idx];

    // Force a fresh column at the start of every paragraph after the
    // first, the same way horizontal layout starts a new line per
    // paragraph.
    // Tracks whether a forced paragraph break just fired for THIS position, so the kinsoku
    // line-start pull-back below can be suppressed: a paragraph the author starts with
    // prohibited punctuation (……でも) must keep its fresh column -- oikomi would otherwise
    // drag its opening characters back into the previous paragraph's column, visually merging
    // the two (confirmed with the full-pipeline host repro). Kinsoku governs WRAPPED line
    // starts, not author-intended paragraph openings.
    // Box END before the paragraph break: the box's content ended with the previous glyph, so
    // its rect closes at the CURRENT column -- the break below then advances to a fresh one.
    while (nextBoxEndIdx < boxEndsBeforeIndex_.size() && idx == boxEndsBeforeIndex_[nextBoxEndIdx]) {
      if (inBox_) {
        appendBoxRectToPage(page, boxStartCol_, column, /*openLeft=*/false,
                            /*openRight=*/boxContinuedFromPrevPage_);
        if (activeBlock_.alignCenter) centerBlockColumns(page, boxStartCol_, column);
        const bool wantAfterGap = activeBlock_.afterEm >= 0.75f;
        inBox_ = false;
        boxContinuedFromPrevPage_ = false;
        // Content after the block must not share its last column.
        if (row != 0) {
          column++;
          row = 0;
          finalizePageIfNeeded();
        }
        // m-after-Xem approximation: one blank column of extra separation.
        if (wantAfterGap && column != 0) {
          column++;
          row = 0;
          finalizePageIfNeeded();
        }
      }
      nextBoxEndIdx++;
    }

    bool paraBreakJustFired = false;
    while (nextParagraphBreakIdx < paragraphBreaksBeforeIndex_.size() &&
           idx == paragraphBreaksBeforeIndex_[nextParagraphBreakIdx]) {
      paraBreakJustFired = true;
      if (row != 0 || column == 0) {
        column++;
        row = 0;
        finalizePageIfNeeded();
      }
      row = columnStartRow(true);
      nextParagraphBreakIdx++;
    }

    // Box START after the paragraph break has advanced to the box's first (fresh) column.
    while (nextBoxStartIdx < boxStartsBeforeIndex_.size() && idx == boxStartsBeforeIndex_[nextBoxStartIdx]) {
      // Consume this marker's params unconditionally -- markers and queue entries are recorded
      // 1:1, and skipping a pop (e.g. a start while already in a block) would desync every
      // later block's params.
      VerticalBlockParams params;
      if (!blockParamsQueue_.empty()) {
        params = blockParamsQueue_.front();
        blockParamsQueue_.erase(blockParamsQueue_.begin());
      }
      if (!inBox_) {
        // Defensive: if no break fired (block element without a <p>), still start fresh.
        if (row != 0) {
          column++;
          row = 0;
          finalizePageIfNeeded();
        }
        // m-before-Xem approximation: one blank column of extra separation.
        if (params.beforeEm >= 0.75f && column != 0) {
          column++;
          row = 0;
          finalizePageIfNeeded();
        }
        activeBlock_ = params;
        inBox_ = true;
        boxStartCol_ = column;
        row = columnStartRow(true);
        LOG_DBG("VPT", "block active at col=%u row=%u start=%.1f hang=%.1f edges=0x%X", column, row,
                activeBlock_.startEm, activeBlock_.hangEm, activeBlock_.borderEdges);
      }
      nextBoxStartIdx++;
    }

    const size_t boundaryLimit = (nextParagraphBreakIdx < paragraphBreaksBeforeIndex_.size())
                                     ? paragraphBreaksBeforeIndex_[nextParagraphBreakIdx]
                                     : stream_.size();

    // Tate-chu-yoko for !?/!! pairs: JP books mark these with a tcy span, but
    // like the digit path below we detect them directly so books without the
    // class benefit too. Only a run of exactly two marks qualifies, and only
    // when no ASCII letter/digit adjoins it -- "Hello!?" stays part of the
    // rotated latin run. Other run lengths keep their existing handling.
    if (isBangOrQuestion(pc.codepoint)) {
      const bool prevAlnum = idx > 0 && isAsciiAlnum(stream_[idx - 1].codepoint);
      size_t markEnd = idx;
      while (markEnd < boundaryLimit && isBangOrQuestion(stream_[markEnd].codepoint)) {
        markEnd++;
      }
      const bool nextAlnum = markEnd < stream_.size() && isAsciiAlnum(stream_[markEnd].codepoint);
      if (markEnd - idx == 2 && !prevAlnum && !nextAlnum) {
        placeTcyPairAt(idx);
        idx = markEnd;
        continue;
      }
    }

    if (isAsciiDigit(pc.codepoint)) {
      size_t digitEnd = idx;
      while (digitEnd < boundaryLimit && isAsciiDigit(stream_[digitEnd].codepoint)) {
        digitEnd++;
      }

      const size_t digitCount = digitEnd - idx;

      if (digitCount == 2) {
        placeTcyPairAt(idx);
        idx = digitEnd;
        continue;
      }

      if (digitCount > 2) {
        std::string runUtf8;
        for (size_t i = idx; i < digitEnd; i++) {
          utf8AppendCodepoint(stream_[i].codepoint, runUtf8);
        }

        // Render-truth measurement with the run's ACTUAL style: getRenderAdvanceX resolves
        // glyphs exactly as drawTextRotated90CCW will (the advance-table fast path priced
        // non-resident digits from a companion font and under-reported -- device photo:
        // a year-run measured ~half its drawn width, so 年 overprinted the digits).
        const auto runStyle = static_cast<EpdFontFamily::Style>(pc.style);
        renderer_.ensureSdCardFontReady(fontId_, runUtf8.c_str(), static_cast<uint8_t>(1u << (pc.style & 3)));
        const int runWidthPx = renderer_.getRenderAdvanceX(fontId_, runUtf8.c_str(), runStyle);
        // Same measured placement as an embedded Latin word: start under the previous glyph's
        // real ink, reserve only what the digits' ink needs. The old fixed 9/10-cell drop was
        // a blunt guard against 4-digit years overprinting the following character (1914年)
        // and left most of a cell empty behind the number (device photo, SCP－1305 だ。).
        const int digitInkWidth = runInkWidth(runUtf8, runWidthPx, runStyle);
        const int nextInkOffset = nextGlyphInkOffset(digitEnd, pc.paragraphIndex);
        int startY = rotatedRunStartY(column, row * cellPx);
        uint16_t rowsNeeded = rotatedRunRows(startY, digitInkWidth, row, nextInkOffset);

        if (row != 0 && row + rowsNeeded > rowsPerColumn) {
          column++;
          row = 0;
          finalizePageIfNeeded();
          row = columnStartRow(false);
          startY = rotatedRunStartY(column, row * cellPx);
          rowsNeeded = rotatedRunRows(startY, digitInkWidth, row, nextInkOffset);
        }

        VerticalGlyph g;
        g.codepoint = 0;
        g.column = column;
        g.row = row;
        g.x = static_cast<uint16_t>(columnLeftX(column) + cellPx - ascender);
        g.y = static_cast<uint16_t>(startY);
        g.paragraphIndex = pc.paragraphIndex;
        g.byteOffset = pc.byteOffset;
        g.style = pc.style;
        g.renderKind = VerticalGlyph::RotatedRun;
        pushGlyph(page, g, runUtf8);

        const uint16_t digitColumn = column;
        row = static_cast<uint16_t>(row + rowsNeeded);
        takeUpRunSlack(startY, digitInkWidth, row, digitColumn, nextInkOffset);
        if (row >= rowsPerColumn) {
          column++;
          row = 0;
          finalizePageIfNeeded();
          row = columnStartRow(false);
        }
        idx = digitEnd;
        continue;
      }

      // Single digit (digitCount == 1): place centered upright
      placeUprightAt(pc, column, row);
      row++;
      if (row >= rowsPerColumn) {
        column++;
        row = 0;
        finalizePageIfNeeded();
        row = columnStartRow(false);
      }
      idx++;
      continue;
    }

    if (Kinsoku::isRotatedRunCharacter(pc.codepoint)) {
      // Gather the contiguous run of rotated-run characters (e.g. an
      // embedded English phrase) so it's laid out, and later rendered,
      // as a single sideways block instead of one cell per character.
      size_t runEnd = idx;
      std::string runUtf8;
      while (runEnd < boundaryLimit && Kinsoku::isRotatedRunCharacter(stream_[runEnd].codepoint) &&
             stream_[runEnd].paragraphIndex == pc.paragraphIndex) {
        utf8AppendCodepoint(stream_[runEnd].codepoint, runUtf8);
        runEnd++;
        if (runEnd - idx > 64) break;
      }

      // Split the run into chunks that fit in columns, breaking at spaces.
      // Measure with the run's ACTUAL style (drawn with g.style; bold is wider than an
      // unstyled measurement, which under-reserved rows and overprinted the next glyph).
      const auto runStyle = static_cast<EpdFontFamily::Style>(pc.style);
      renderer_.ensureSdCardFontReady(fontId_, runUtf8.c_str(), static_cast<uint8_t>(1u << (pc.style & 3)));
      const int maxColumnPx = rowsPerColumn * cellPx;
      // JP sources separate embedded Latin from kana with ASCII spaces (それは Germinal や).
      // Drawn verbatim, the leading space pushes the first letter deep into the run's first
      // cell and the trailing space inflates the reserved rows -- the word floats low with a
      // dead cell after it (device photo, Vita Sexualis). The measured start position plus
      // the cell raster already provide the visual separation, so trim boundary spaces and
      // keep only the inner ones (word gaps and break points).
      const auto trimSpaces = [](std::string& s) {
        const size_t b = s.find_first_not_of(' ');
        if (b == std::string::npos) {
          s.clear();
          return;
        }
        const size_t e = s.find_last_not_of(' ');
        s.assign(s, b, e - b + 1);
      };
      const int nextInkOffset = nextGlyphInkOffset(runEnd, pc.paragraphIndex);
      std::string remaining = runUtf8;
      const bool hadLeadingSpace = !runUtf8.empty() && runUtf8[0] == ' ';
      trimSpaces(remaining);
      if (remaining.empty()) {
        idx = runEnd;
        continue;
      }
      // A source space before the word (哲学と Sokrates) is a visible separator in
      // tategaki: give it a full empty cell so the word starts one character below the
      // preceding glyph instead of sharing its cell (device photo: S printed onto the と).
      // Skipped at a fresh column start -- line-leading spaces collapse, as in kinsoku.
      if (hadLeadingSpace && row > columnStartRow(false)) {
        row++;
        if (row >= rowsPerColumn) {
          column++;
          row = 0;
          finalizePageIfNeeded();
          row = columnStartRow(false);
        }
      }

      while (!remaining.empty()) {
        const int remWidthPx = renderer_.getRenderAdvanceX(fontId_, remaining.c_str(), runStyle);
        const int startY = rotatedRunStartY(column, row * cellPx);
        const uint16_t remRows =
            rotatedRunRows(startY, runInkWidth(remaining, remWidthPx, runStyle), row, nextInkOffset);
        const uint16_t availRows = rowsPerColumn - row;

        if (remRows <= availRows) {
          // Fits in the current column.
          VerticalGlyph g;
          g.codepoint = 0;
          g.column = column;
          g.row = row;
          g.x = static_cast<uint16_t>(columnLeftX(column) + cellPx - ascender);
          g.y = static_cast<uint16_t>(startY);
          g.paragraphIndex = pc.paragraphIndex;
          g.byteOffset = pc.byteOffset;
          g.style = pc.style;
          g.renderKind = VerticalGlyph::RotatedRun;
          pushGlyph(page, g, remaining);
          row = static_cast<uint16_t>(row + remRows);
          takeUpRunSlack(startY, runInkWidth(remaining, remWidthPx, runStyle), row, g.column, nextInkOffset);
          if (row >= rowsPerColumn) {
            column++;
            row = 0;
            finalizePageIfNeeded();
            row = columnStartRow(false);
          }
          break;
        }

        // Doesn't fit — find a space to break at that fits within availRows.
        // Measure progressively shorter prefixes ending at a space.
        size_t breakAt = std::string::npos;
        for (size_t sp = remaining.rfind(' '); sp != std::string::npos;
             sp = (sp == 0) ? std::string::npos : remaining.rfind(' ', sp - 1)) {
          std::string prefix = remaining.substr(0, sp);
          const int prefixPx = renderer_.getRenderAdvanceX(fontId_, prefix.c_str(), runStyle);
          const uint16_t prefixRows =
              rotatedRunRows(startY, runInkWidth(prefix, prefixPx, runStyle), row, nextInkOffset);
          if (prefixRows <= availRows) {
            breakAt = sp;
            break;
          }
        }

        if (breakAt == std::string::npos) {
          // No space-break fits. Retry from a fresh column ONLY when the current position is
          // deeper than where a fresh column would start; otherwise force-place. The comparison
          // MUST be against columnStartRow(false), not 0: inside a styled block (start-Xem, e.g.
          // Aozora/EBPAJ div.mtN margins, defined up to 22em+) fresh columns begin at a non-zero
          // row, and comparing against 0 made this branch loop forever -- every retry re-seeded
          // row = startRows != 0, the force-place arm below was unreachable, and the layout
          // marched through columns/pages without consuming a single byte ("Indexing" never
          // finished; host-reproduced with a mtN block whose start offset left fewer rows than a
          // short embedded Latin word needs). After one retry row == columnStartRow(false), so
          // the force-place arm is guaranteed on the next pass -- termination is structural.
          if (row > columnStartRow(false)) {
            column++;
            row = 0;
            finalizePageIfNeeded();
            row = columnStartRow(false);
          } else {
            // At (or above) a fresh column's start row and still doesn't fit — force-place the
            // whole thing at the CURRENT row to guarantee progress. It may overrun the column
            // bottom (the renderer clips); an unbreakable over-long run has no better placement.
            VerticalGlyph g;
            g.codepoint = 0;
            g.column = column;
            g.row = row;
            g.x = static_cast<uint16_t>(columnLeftX(column) + cellPx - ascender);
            g.y = static_cast<uint16_t>(startY);
            g.paragraphIndex = pc.paragraphIndex;
            g.byteOffset = pc.byteOffset;
            g.style = pc.style;
            g.renderKind = VerticalGlyph::RotatedRun;
            pushGlyph(page, g, remaining);
            row = static_cast<uint16_t>(std::min<int>(row + remRows, rowsPerColumn));
            if (row >= rowsPerColumn) {
              column++;
              row = 0;
              finalizePageIfNeeded();
              row = columnStartRow(false);
            }
            break;
          }
          continue;
        }

        // Place the prefix chunk.
        std::string chunk = remaining.substr(0, breakAt);
        trimSpaces(chunk);
        if (chunk.empty()) {
          // Nothing but spaces before the break point (double spaces in the source);
          // consume them and retry with the remainder instead of pushing an empty glyph.
          remaining = remaining.substr(breakAt + 1);
          trimSpaces(remaining);
          continue;
        }
        const int chunkPx = renderer_.getRenderAdvanceX(fontId_, chunk.c_str(), runStyle);
        const uint16_t chunkRows = rotatedRunRows(startY, runInkWidth(chunk, chunkPx, runStyle), row, nextInkOffset);

        VerticalGlyph g;
        g.codepoint = 0;
        g.column = column;
        g.row = row;
        g.x = static_cast<uint16_t>(columnLeftX(column) + cellPx - ascender);
        g.y = static_cast<uint16_t>(startY);
        g.paragraphIndex = pc.paragraphIndex;
        g.byteOffset = pc.byteOffset;
        g.style = pc.style;
        g.renderKind = VerticalGlyph::RotatedRun;
        pushGlyph(page, g, chunk);

        row = static_cast<uint16_t>(row + chunkRows);
        if (row >= rowsPerColumn) {
          column++;
          row = 0;
          finalizePageIfNeeded();
          row = columnStartRow(false);
        }

        // Skip the space and continue with the rest.
        remaining = remaining.substr(breakAt + 1);
        trimSpaces(remaining);
      }

      idx = runEnd;
      continue;
    }

    // Single upright CJK/kana/punctuation character.
    // A column start is NOT always row 0: styled blocks (start-Xem, hanging indents) open their
    // columns further down, which used to dodge this check and let 。/、 head an indented column.
    // "Nothing placed in this column yet" is the real condition -- glyphs are appended in layout
    // order, so the last glyph sitting in an earlier column (or an empty page) means this
    // character would be the column's first.
    const bool startingNewColumn = page.glyphs.empty() || page.glyphs.back().column < column;
    if (startingNewColumn && !paraBreakJustFired && idx >= suppressKinsokuUntilIdx &&
        Kinsoku::isLineStartProhibited(pc.codepoint)) {
      if (!page.glyphs.empty()) {
        const VerticalGlyph& prev = page.glyphs.back();
        // Oikomi (追い込み) only while the previous column has a row spare -- otherwise it writes
        // past the bottom of the grid, over the status bar.
        if (prev.row + 1 < rowsPerColumn) {
          placeUprightAt(pc, prev.column, static_cast<uint16_t>(prev.row + 1));
          idx++;
          continue;
        }
        // JLREQ 3.1.4: two adjacent half-em marks SHARE one em -- the first takes the first half,
        // the second the second. That covers 。」 and 、」 as well as 』」 and 」」, since a closing
        // bracket is itself a half-em glyph set in the first half of its cell (and 。/、 are set
        // tight for the same reason). The half the first mark left free is exactly where the
        // second belongs: no new cell, and neither character leaves the column its sentence
        // ended in. Only the FIRST of the pair may already be placed -- a full-em character
        // before the bracket (？」) genuinely needs 1.5 em and falls through to oidashi.
        const int prevHalfEm = Kinsoku::verticalShiftType(prev.codepoint);
        if ((prevHalfEm == 1 || prevHalfEm == 2) && Kinsoku::verticalShiftType(pc.codepoint) == 2 &&
            Kinsoku::needsVerticalRotation(pc.codepoint)) {
          VerticalGlyph g;
          g.codepoint = pc.codepoint;
          g.column = prev.column;
          g.row = prev.row;
          g.x = static_cast<uint16_t>(columnLeftX(prev.column));
          // Raw grid position: pushGlyph applies this column's slide, as every other path expects.
          g.y = static_cast<uint16_t>(prev.row * cellPx + cellPx / 2);
          g.renderKind = VerticalGlyph::RotatedPunct;
          g.paragraphIndex = pc.paragraphIndex;
          g.byteOffset = pc.byteOffset;
          g.style = pc.style;
          g.emphasis = pc.emphasis;
          pushGlyph(page, g, pc.rubyText);
          idx++;
          continue;
        }

        // Burasage (ぶら下げ): 。/、 hang past the end of a full column rather than evict the
        // character before them. They are half-em marks whose ink sits in the top half of the
        // cell, so the overhang is small and falls in the bottom margin -- the same margin ruby
        // uses beside the text (JLREQ Fig 2.37). Without this, oidashi pulled the preceding
        // character along and left a column holding just those two.
        if (Kinsoku::verticalShiftType(pc.codepoint) == 1 && prev.row + 1 == rowsPerColumn) {
          placeUprightAt(pc, prev.column, static_cast<uint16_t>(prev.row + 1));
          idx++;
          continue;
        }

        // Oidashi (追い出し): move the character BEFORE this one forward, so the pair starts this
        // column together (。」 must not be split across columns).
        //
        // Move it EXPLICITLY into this column. Popping it and rewinding idx does not work: the
        // normal path re-places it in the very cell that was just freed, so the bracket is back at
        // a column head and asks for the same rewind -- forever. That was a live-locked build
        // holding the render lock, i.e. a frozen reader.
        //
        // Only safe when that glyph is this batch's previous stream entry: streamed layout can
        // leave glyphs from an earlier batch on the page, which are not ours to re-place. And only
        // when the pair actually fits, so a short column cannot strand the bracket instead.
        if (idx > 0 && row + 1 < rowsPerColumn && prev.byteOffset == stream_[idx - 1].byteOffset &&
            prev.paragraphIndex == stream_[idx - 1].paragraphIndex) {
          page.glyphs.pop_back();
          placeUprightAt(stream_[idx - 1], column, row);
          row++;
          // Fall through: this character is placed normally, now one row below its partner.
        }
      }
      if (page.glyphs.empty() && !pages.empty()) {
        // Page just broke — pull back to the last column of the previous page.
        VerticalPage& prevPage = pages.back();
        // prevPage.glyphs was reserved for exactly one page's grid capacity when it was created;
        // this oikomi pull-back is the one place that can push a page over that reservation,
        // forcing libstdc++ to reallocate+relocate an already-near-full glyph array. Confirmed via
        // a real device crash inside this exact reallocation. If there's no reservation headroom
        // and heap is tight, skip the pull-back (the character starts the next page/column
        // normally instead) rather than risk it -- a minor formatting nicety, not correctness.
        const bool hasHeadroom = prevPage.glyphs.size() < prevPage.glyphs.capacity();
        // Same bound as the in-page pull-back: only while that column has a row spare.
        const bool prevColumnHasRoom = !prevPage.glyphs.empty() && prevPage.glyphs.back().row + 1 < rowsPerColumn;
        if (prevColumnHasRoom && (hasHeadroom || ESP.getMaxAllocHeap() >= MIN_FREE_HEAP_FOR_RESERVE)) {
          const VerticalGlyph& prev = prevPage.glyphs.back();
          VerticalGlyph g;
          g.codepoint = pc.codepoint;
          g.column = prev.column;
          g.row = static_cast<uint16_t>(prev.row + 1);
          int gx = columnLeftX(prev.column);
          int gy = g.row * cellPx;
          if (Kinsoku::verticalShiftType(pc.codepoint) == 1) {
            int qx = 0, qy = 0;
            if (quadrantTopRight(pc.codepoint, pc.style, prev.column, g.row, /*centreInHalf=*/true, &qx, &qy)) {
              gx = qx;
              gy = qy;
            } else {
              gx += cellPx / 2;
              gy = std::max(0, gy - cellPx / 2);
            }
          }
          g.x = static_cast<uint16_t>(gx);
          g.y = static_cast<uint16_t>(gy);
          g.renderKind = VerticalGlyph::Upright;
          if (Kinsoku::needsVerticalRotation(pc.codepoint)) {
            g.x = static_cast<uint16_t>(columnLeftX(prev.column) + cellPx - ascender);
            g.y = static_cast<uint16_t>(g.row * cellPx);
            g.renderKind = VerticalGlyph::RotatedPunct;
          }
          g.paragraphIndex = pc.paragraphIndex;
          g.byteOffset = pc.byteOffset;
          pushGlyph(prevPage, g, pc.rubyText);
          idx++;
          continue;
        }
      }
    }

    bool endingColumn = (row == rowsPerColumn - 1);
    if (endingColumn && Kinsoku::isLineEndProhibited(pc.codepoint)) {
      // Oidashi (追い出し): push this character forward into a fresh
      // column instead of letting it end the current one.
      column++;
      row = 0;
      finalizePageIfNeeded();
      row = columnStartRow(false);
    }

    placeUpright(pc);
    const uint16_t placedRow = row;
    row++;
    // The ellipsis dot stack is drawn low enough to leave its own cell (it has to, or it
    // floats above the rhythm of the upright glyphs around it). Reserve that overflow by
    // sliding the rest of the column down, or the next character prints into the dots
    // (device photo, book 2).
    // Applied once per ellipsis GROUP (…… is two codepoints): the dots keep their own
    // spacing, only what follows the group moves.
    const bool endsEllipsisGroup =
        (pc.codepoint == 0x2026 || pc.codepoint == 0x2025) &&
        (idx + 1 >= stream_.size() || (stream_[idx + 1].codepoint != 0x2026 && stream_[idx + 1].codepoint != 0x2025));
    if (endsEllipsisGroup && row < rowsPerColumn) {
      const int nextInkOffset = nextGlyphInkOffset(idx + 1, pc.paragraphIndex);
      int inkTop = 0, inkHeight = 0;
      if (nextInkOffset >= 0 &&
          renderer_.verticalPunctInkBox(fontId_, pc.codepoint, static_cast<EpdFontFamily::Style>(pc.style),
                                        placedRow * cellPx, cellPx, Kinsoku::verticalShiftType(pc.codepoint), &inkTop,
                                        &inkHeight)) {
        // The dots sit low in their cell, so close the resulting overlap down to the same ink
        // gap a normal pair of characters leaves.
        const int deficit = (inkTop + inkHeight + inkGapPx) - (row * cellPx + nextInkOffset);
        if (deficit > 0) {
          columnYShift -= deficit;
          shiftColumn = column;
        }
      }
    }
    if (row >= rowsPerColumn) {
      column++;
      row = 0;
      finalizePageIfNeeded();
      row = columnStartRow(false);
    }
    idx++;
  }

  if (isFinalFlush) {
    if (!page.glyphs.empty() || !anyPageEverProduced_) {
      pages.push_back(std::move(page));
      anyPageEverProduced_ = true;
    }
    // Chapter done -- next layoutPages() call (if any, e.g. a new VerticalParsedText/chapter)
    // should start a fresh page rather than resuming this one.
    pendingPageValid_ = false;
    anyPageEverProduced_ = false;
  }
  // Non-final flush: the trailing page is intentionally NOT pushed here -- it's held in
  // pendingPage_ and continued by the next layoutPages() call instead of being cut short.
  return pages;
}

// Build the pixel rect for a box spanning columns [startCol..endCol] (startCol is the RIGHTMOST
// column in tategaki order) and append it to the page. Full column height, with the border lines
// centered in the surrounding column gaps / padded by a fraction of the cell vertically.
void VerticalParsedText::appendBoxRectToPage(VerticalPage& p, const uint16_t startCol, const uint16_t endCol,
                                             const bool openLeft, const bool openRight) const {
  if (boxGeomCellPx_ <= 0 || activeBlock_.borderEdges == 0) return;
  const int gap = boxGeomColumnAdvancePx_ - boxGeomCellPx_;
  // Sideways: the border sits on the column axis, i.e. halfway across the gap it runs in.
  // Lengthwise: the same half-gap at the head, and a full character of air at the foot -- the
  // reference rendering (Apple Books) sets the last line one character clear of the rule. The
  // box may extend past the text area into the bottom margin.
  const int padX = std::max(2, gap / 2);
  const int padTop = padX;
  const int padBottom = boxGeomCellPx_;
  auto colLeft = [&](const uint16_t c) -> int {
    return boxGeomUsableWidthPx_ - boxGeomCellPx_ - static_cast<int>(c) * boxGeomColumnAdvancePx_;
  };
  const int right = colLeft(startCol) + boxGeomCellPx_ + padX;
  const int left = colLeft(endCol) - padX;
  VerticalBoxRect r;
  r.x = static_cast<int16_t>(left);
  r.y = static_cast<int16_t>(-padTop);
  r.w = static_cast<int16_t>(right - left);
  r.h = static_cast<int16_t>(static_cast<int>(boxGeomRowsPerColumn_) * boxGeomCellPx_ + padTop + padBottom);
  // CSS physical edges map 1:1 to draw bits; a page-boundary side loses its vertical line and
  // gains the extend flag instead (half-open print style).
  r.edges = activeBlock_.borderEdges;
  if (openLeft) {
    r.edges &= static_cast<uint8_t>(~VerticalBoxRect::DRAW_LEFT);
    r.edges |= VerticalBoxRect::EXTEND_LEFT;
  }
  if (openRight) {
    r.edges &= static_cast<uint8_t>(~VerticalBoxRect::DRAW_RIGHT);
    r.edges |= VerticalBoxRect::EXTEND_RIGHT;
  }
  p.boxes.push_back(r);
}

// Vertically center the glyphs of columns [startCol..endCol] within the column height
// (text-align: center in vertical writing). Runs once when a centered block closes on a page.
void VerticalParsedText::centerBlockColumns(VerticalPage& p, const uint16_t startCol, const uint16_t endCol) const {
  if (boxGeomCellPx_ <= 0 || boxGeomRowsPerColumn_ == 0) return;
  for (uint16_t c = startCol; c <= endCol; c++) {
    int maxRow = -1;
    int minRow = boxGeomRowsPerColumn_;
    for (const auto& g : p.glyphs) {
      if (g.column != c) continue;
      maxRow = std::max(maxRow, static_cast<int>(g.row));
      minRow = std::min(minRow, static_cast<int>(g.row));
    }
    if (maxRow < 0) continue;
    const int usedRows = maxRow - minRow + 1;
    const int shiftPx =
        ((static_cast<int>(boxGeomRowsPerColumn_) - usedRows) * boxGeomCellPx_) / 2 - minRow * boxGeomCellPx_;
    if (shiftPx == 0) continue;
    for (auto& g : p.glyphs) {
      if (g.column != c) continue;
      g.y = static_cast<uint16_t>(std::max(0, static_cast<int>(g.y) + shiftPx));
    }
  }
}

bool VerticalParsedText::finalizePendingPage(VerticalPage& out) {
  if (!pendingPageValid_) return false;
  pendingPageValid_ = false;  // next layoutPages() call starts a fresh page either way
  if (pendingPage_.glyphs.empty()) return false;
  // Chapter ended while inside a box (end marker carried past the last batch, or a truncated
  // chapter): close the rect on this final page so the border isn't silently dropped.
  if (inBox_) {
    appendBoxRectToPage(pendingPage_, boxStartCol_, pendingColumn_, /*openLeft=*/false,
                        /*openRight=*/boxContinuedFromPrevPage_);
    if (activeBlock_.alignCenter) centerBlockColumns(pendingPage_, boxStartCol_, pendingColumn_);
    inBox_ = false;
    boxContinuedFromPrevPage_ = false;
  }
  out = std::move(pendingPage_);
  anyPageEverProduced_ = true;  // set, NOT reset -- see the header doc comment
  return true;
}
