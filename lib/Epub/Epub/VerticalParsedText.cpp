#include "VerticalParsedText.h"

#include <Arduino.h>
#include <Logging.h>

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
// the app is quiescent (largest concurrent needs: SD write buffers and log lines, low KBs). 8KB
// matches SMALL_ALLOC_MARGIN, the equivalent cushion used for per-push growth in this file.
constexpr uint32_t MIN_FREE_HEAP_FOR_RESERVE = 8 * 1024;
}  // namespace

namespace {

// Minimal local UTF-8 decoder. Deliberately self-contained rather than
// depending on the project's internal utf8NextCodepoint() (used inside
// GfxRenderer.cpp) since that helper's visibility/signature wasn't
// confirmed against the exact checkout this lands on -- swap this out for
// the shared helper if/when it's exposed publicly, to avoid having two
// implementations to keep in sync.
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

std::string encodeCp(uint32_t cp) {
  std::string out;
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
  return out;
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

int VerticalParsedText::charAdvancePx() const {
  // Measure the advance width of a reference CJK character to get the
  // true em-square size. For CJK fonts this matches advanceY; for
  // Latin-oriented fonts (NotoSerif) where advanceY includes extra
  // interline spacing, this gives the correct tighter cell size.
  // SD fonts measure through their advance table; make sure the reference glyph is in it.
  // Without this, a freshly loaded SD font (e.g. the JP companion serving as the effective
  // reader font) measured 漢 as 0 and the cell size fell back to getLineHeight() -- for CJK
  // fonts that includes interline spacing, blowing cells up from ~1em to ~1.45em (visibly
  // "very wide" character spacing).
  renderer_.ensureSdCardFontReady(fontId_, "\xe6\xbc\xa2", 0x01);
  const int cjkAdvance =
      renderer_.getTextAdvanceX(fontId_, "\xe6\xbc\xa2", static_cast<EpdFontFamily::Style>(0));  // 漢
  if (cjkAdvance > 0) return cjkAdvance + cjkAdvance / 6;
  return renderer_.getLineHeight(fontId_);
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
  if (ESP.getMaxAllocHeap() < requestBytes + MIN_FREE_HEAP_FOR_RESERVE) {
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
  if (ESP.getMaxAllocHeap() >= bytes + MIN_FREE_HEAP_FOR_RESERVE) {
    stream_.reserve(STREAM_STABLE_ENTRIES);
  } else {
    LOG_ERR("VPT", "preallocateStream: %u bytes don't fit (maxAlloc=%u); falling back to incremental growth",
            static_cast<unsigned>(bytes), ESP.getMaxAllocHeap());
  }
}

bool VerticalParsedText::canPushStreamChar() {
  if (oom_) return false;
  if (stream_.size() < stream_.capacity()) return true;  // no reallocation needed, cheap path
  // Same trap as pushGlyph() in layoutPages(): plain doubling means one failed growth attempt
  // permanently blocks every later char in this batch (oom_ latches, and the doubled request
  // never gets smaller on its own) -- confirmed on a real device as the actual cause of "sparse"
  // pages surviving even after pushGlyph() was fixed, because the text never made it into the
  // stream for layoutPages() to place in the first place. Fall back to a small linear growth step
  // before giving up, so a later char (after some other allocation frees up) has a real chance.
  constexpr uint32_t SMALL_ALLOC_MARGIN = 8 * 1024;
  constexpr size_t LINEAR_GROWTH_STEP = 64;  // PendingChar elements; keeps stalled retries cheap

  const size_t doubledCapacity = stream_.capacity() == 0 ? 1 : stream_.capacity() * 2;
  const size_t doubledBytes = doubledCapacity * sizeof(PendingChar);
  if (ESP.getMaxAllocHeap() >= doubledBytes + SMALL_ALLOC_MARGIN) {
    stream_.reserve(doubledCapacity);
    return true;
  }

  const size_t linearCapacity = stream_.capacity() + LINEAR_GROWTH_STEP;
  const size_t linearBytes = linearCapacity * sizeof(PendingChar);
  if (ESP.getMaxAllocHeap() >= linearBytes + SMALL_ALLOC_MARGIN) {
    stream_.reserve(linearCapacity);
    return true;
  }

  LOG_ERR("VPT", "Low heap (%u bytes, need ~%u) while building vertical text stream; truncating batch",
          ESP.getMaxAllocHeap(), static_cast<unsigned>(linearBytes));
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
      if (paragraphBreaksBeforeIndex_.empty() || paragraphBreaksBeforeIndex_.back() != stream_.size()) {
        paragraphBreaksBeforeIndex_.push_back(stream_.size());
      }
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
    if (paragraphBreaksBeforeIndex_.empty() || paragraphBreaksBeforeIndex_.back() != stream_.size()) {
      paragraphBreaksBeforeIndex_.push_back(stream_.size());
    }
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
        const size_t absBreakIdx = stream_.size() + relIdx;  // relIdx is always 0 here
        if (paragraphBreaksBeforeIndex_.empty() || paragraphBreaksBeforeIndex_.back() != absBreakIdx) {
          paragraphBreaksBeforeIndex_.push_back(absBreakIdx);
        }
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
        for (size_t r = rubyStart; r < rubyEnd; r++) {
          const uint32_t rcp = rubyCps[r];
          if (rcp < 0x80) {
            slice.push_back(static_cast<char>(rcp));
          } else if (rcp < 0x800) {
            slice.push_back(static_cast<char>(0xC0 | (rcp >> 6)));
            slice.push_back(static_cast<char>(0x80 | (rcp & 0x3F)));
          } else if (rcp < 0x10000) {
            slice.push_back(static_cast<char>(0xE0 | (rcp >> 12)));
            slice.push_back(static_cast<char>(0x80 | ((rcp >> 6) & 0x3F)));
            slice.push_back(static_cast<char>(0x80 | (rcp & 0x3F)));
          } else {
            slice.push_back(static_cast<char>(0xF0 | (rcp >> 18)));
            slice.push_back(static_cast<char>(0x80 | ((rcp >> 12) & 0x3F)));
            slice.push_back(static_cast<char>(0x80 | ((rcp >> 6) & 0x3F)));
            slice.push_back(static_cast<char>(0x80 | (rcp & 0x3F)));
          }
        }
        if (!canPushStreamChar()) return;
        stream_.push_back(PendingChar{baseCps[k], paragraphIndex, static_cast<uint32_t>(baseOffsets[k]), run.style,
                                      run.emphasis, std::move(slice)});
      }
    }

    for (size_t relIdx : breakBeforeBaseIndex) {
      const size_t absBreakIdx = runStartStreamIndex + relIdx;
      if (paragraphBreaksBeforeIndex_.empty() || paragraphBreaksBeforeIndex_.back() != absBreakIdx) {
        paragraphBreaksBeforeIndex_.push_back(absBreakIdx);
      }
    }
  }
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

  const int cellPx = std::max(1, charAdvancePx());
  const int columnAdvancePx = cellPx + columnGapPx_;
  const int ascender = renderer_.getFontAscenderSize(fontId_);
  const int globalDownNudge = std::max(1, (cellPx * 3) / 8);
  const int bottomReservedPx = std::max(cellPx * 2, ascender + globalDownNudge + cellPx);
  const int usableHeightPx = std::max(cellPx, static_cast<int>(viewportHeight_) - bottomReservedPx);
  const uint16_t rowsPerColumn = static_cast<uint16_t>(std::max(1, usableHeightPx / cellPx));
  const int usableWidthPx = std::max(cellPx, static_cast<int>(viewportWidth_) - rightPaddingPx_);
  const uint16_t columnsPerPage = static_cast<uint16_t>(std::max(1, usableWidthPx / columnAdvancePx));

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
  boxGeomUsableWidthPx_ = usableWidthPx;
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
    if (ESP.getMaxAllocHeap() >= requestBytes + MIN_FREE_HEAP_FOR_RESERVE) {
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
    if (ESP.getMaxAllocHeap() >= requestBytes + MIN_FREE_HEAP_FOR_RESERVE) {
      p.glyphs.reserve(glyphsPerPage);
    } else {
      LOG_ERR("VPT", "Skipping page glyphs reserve (%u bytes doesn't fit, free=%u); growing incrementally",
              static_cast<unsigned>(requestBytes), ESP.getMaxAllocHeap());
    }
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
  auto pushGlyph = [this](std::vector<VerticalGlyph>& glyphs, const VerticalGlyph& g) {
    if (glyphs.size() < glyphs.capacity()) {
      glyphs.push_back(g);
      return true;
    }
    constexpr uint32_t SMALL_ALLOC_MARGIN = 8 * 1024;  // headroom for the rest of the app, not the reserve() margin
    constexpr size_t LINEAR_GROWTH_STEP = 16;          // elements; keeps a stalled page's retries cheap

    const size_t doubledCapacity = glyphs.capacity() == 0 ? 1 : glyphs.capacity() * 2;
    const size_t doubledBytes = doubledCapacity * sizeof(VerticalGlyph);
    if (ESP.getMaxAllocHeap() >= doubledBytes + SMALL_ALLOC_MARGIN) {
      glyphs.reserve(doubledCapacity);
      glyphs.push_back(g);
      return true;
    }

    const size_t linearCapacity = glyphs.capacity() + LINEAR_GROWTH_STEP;
    const size_t linearBytes = linearCapacity * sizeof(VerticalGlyph);
    if (ESP.getMaxAllocHeap() >= linearBytes + SMALL_ALLOC_MARGIN) {
      glyphs.reserve(linearCapacity);
      glyphs.push_back(g);
      return true;
    }

    LOG_ERR("VPT", "Low heap (%u bytes, need ~%u); dropping glyph", ESP.getMaxAllocHeap(),
            static_cast<unsigned>(linearBytes));
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

  auto columnLeftX = [&](uint16_t col) -> int { return usableWidthPx - cellPx - col * columnAdvancePx; };

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

  auto placeUprightAt = [&](const PendingChar& pc, uint16_t col, uint16_t rowIdx) {
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
      g.rubyText = pc.rubyText;
      pushGlyph(page.glyphs, g);
      return;
    }

    if (Kinsoku::isSmallKana(pc.codepoint)) {
      // Keep small kana in normal row flow to avoid overlap with neighboring
      // glyphs on fonts where "small" forms still have tall ink boxes.
      // Apply only a light rightward bias inside the cell.
      g.x = static_cast<uint16_t>(columnLeftX(col) + std::max(1, cellPx / 8));
      g.y = static_cast<uint16_t>(rowIdx * cellPx + ascender - std::max(1, cellPx / 8));
      g.renderKind = VerticalGlyph::Upright;
      g.rubyText = pc.rubyText;
      pushGlyph(page.glyphs, g);
      return;
    }

    int gx = columnLeftX(col);
    int gy = rowIdx * cellPx + ascender;
    if (pc.codepoint >= '0' && pc.codepoint <= '9') {
      int left = 0, width = 0, top = 0, height = 0;
      if (renderer_.getGlyphMetrics(fontId_, pc.codepoint, static_cast<EpdFontFamily::Style>(pc.style), &left, &width,
                                    &top, &height)) {
        gx = columnLeftX(col) + (cellPx - width) / 2 - left - 1;
      }
    }
    if (Kinsoku::verticalShiftType(pc.codepoint) == 1) {
      // Comma/period: bottom-left → upper-right of the cell.
      gx += cellPx / 2;
      gy -= cellPx / 2;
    }
    g.x = static_cast<uint16_t>(gx);
    g.y = static_cast<uint16_t>(gy);
    g.renderKind = VerticalGlyph::Upright;
    g.rubyText = pc.rubyText;
    pushGlyph(page.glyphs, g);
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
  auto encodeDigitUtf8 = [](uint32_t cp, std::string& out) {
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  };

  // Place a two-character tate-chu-yoko run (a 2-digit number like 26, or a
  // !?/!! pair) upright in a single cell, ink-centered on the column, and
  // advance row/column past it. Shared by the digit and punctuation-pair
  // branches in the loop below.
  auto placeTcyPairAt = [&](size_t i0) {
    std::string runUtf8;
    encodeDigitUtf8(stream_[i0].codepoint, runUtf8);
    encodeDigitUtf8(stream_[i0 + 1].codepoint, runUtf8);
    // Measure with the pair's ACTUAL style -- rendered with g.style, and bold digits are
    // wider, so an unstyled measurement mis-centers the pair in its cell.
    const auto tcyStyle = static_cast<EpdFontFamily::Style>(stream_[i0].style);
    // Ensure the digits AND the 中 reference glyph (used below to derive the column's CJK ink
    // center) are resident before measuring.
    std::string ensureUtf8 = runUtf8 + "\xe4\xb8\xad";
    renderer_.ensureSdCardFontReady(fontId_, ensureUtf8.c_str(), static_cast<uint8_t>(1u << (stream_[i0].style & 3)));
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
        encodeDigitUtf8(cpLast, lastUtf8);
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
          // and use its delta from the cell center, clamped so a metrics outlier can't fling
          // the pair off-axis. Falls back to plain ink centering when metrics are unavailable.
          int cjkDelta = 0;
          int kl = 0, kw = 0, kt = 0, kh = 0;
          if (renderer_.getGlyphMetrics(fontId_, 0x4E2D /* 中 */, tcyStyle, &kl, &kw, &kt, &kh) && kw > 0) {
            cjkDelta = (kl + kw / 2) - cellPx / 2;
            const int clampPx = std::max(2, cellPx / 8);
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
    g.y = static_cast<uint16_t>(row * cellPx + ascender);
    g.paragraphIndex = stream_[i0].paragraphIndex;
    g.byteOffset = stream_[i0].byteOffset;
    g.style = stream_[i0].style;
    g.renderKind = VerticalGlyph::UprightRun;
    g.rotatedRunText = runUtf8;
    pushGlyph(page.glyphs, g);

    row++;
    if (row >= rowsPerColumn) {
      column++;
      row = 0;
      finalizePageIfNeeded();
      row = columnStartRow(false);
    }
  };

  size_t idx = 0;
  while (idx < stream_.size()) {
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
          encodeDigitUtf8(stream_[i].codepoint, runUtf8);
        }

        // Render-truth measurement with the run's ACTUAL style: getRenderAdvanceX resolves
        // glyphs exactly as drawTextRotated90CCW will (the advance-table fast path priced
        // non-resident digits from a companion font and under-reported -- device photo:
        // a year-run measured ~half its drawn width, so 年 overprinted the digits).
        const auto runStyle = static_cast<EpdFontFamily::Style>(pc.style);
        renderer_.ensureSdCardFontReady(fontId_, runUtf8.c_str(), static_cast<uint8_t>(1u << (pc.style & 3)));
        const int runWidthPx = renderer_.getRenderAdvanceX(fontId_, runUtf8.c_str(), runStyle);
        const int fontPctRun = (cellPx > 0) ? (ascender * 100 / cellPx) : 100;
        const int runExtraNudge = (fontPctRun > 100) ? (cellPx * (fontPctRun - 100) / 30) : 0;
        const int numericRotatedDownNudge = std::max(8, (cellPx * 9) / 10) + runExtraNudge * 2;
        // Reserve rows for the run's FULL vertical span: the ink starts numericRotatedDownNudge
        // below the cell top and extends runWidthPx further down. Reserving only
        // ceil(runWidth/cell) let 4-digit years overrun their rows by most of a cell and
        // overprint the following character (device photo: 1914年 with 14 fused into 年).
        const uint16_t rowsNeeded = static_cast<uint16_t>(std::max(
            1, static_cast<int>(std::ceil(static_cast<double>(numericRotatedDownNudge + runWidthPx) / cellPx))));

        if (row != 0 && row + rowsNeeded > rowsPerColumn) {
          column++;
          row = 0;
          finalizePageIfNeeded();
          row = columnStartRow(false);
        }

        const int topY = row * cellPx;
        VerticalGlyph g;
        g.codepoint = 0;
        g.column = column;
        g.row = row;
        g.x = static_cast<uint16_t>(columnLeftX(column) + cellPx - ascender);
        g.y = static_cast<uint16_t>(topY + numericRotatedDownNudge);
        g.paragraphIndex = pc.paragraphIndex;
        g.byteOffset = pc.byteOffset;
        g.style = pc.style;
        g.renderKind = VerticalGlyph::RotatedRun;
        g.rotatedRunText = runUtf8;
        pushGlyph(page.glyphs, g);

        row = static_cast<uint16_t>(row + rowsNeeded);
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
        runUtf8 += encodeCp(stream_[runEnd].codepoint);
        runEnd++;
        if (runEnd - idx > 64) break;
      }

      // Split the run into chunks that fit in columns, breaking at spaces.
      // Measure with the run's ACTUAL style (drawn with g.style; bold is wider than an
      // unstyled measurement, which under-reserved rows and overprinted the next glyph).
      const auto runStyle = static_cast<EpdFontFamily::Style>(pc.style);
      renderer_.ensureSdCardFontReady(fontId_, runUtf8.c_str(), static_cast<uint8_t>(1u << (pc.style & 3)));
      const int maxColumnPx = rowsPerColumn * cellPx;
      // A rotated run drawn flush at its cell top starts inside the preceding upright
      // character's ink (device photo: ...デザイン bookwall with the ン touching the b).
      // Start it a third of a cell lower; the shift is included in every rows-needed
      // computation below so the run's tail can't creep into the FOLLOWING character.
      const int runDownNudge = std::max(4, cellPx / 2);
      std::string remaining = runUtf8;

      while (!remaining.empty()) {
        const int remWidthPx = renderer_.getRenderAdvanceX(fontId_, remaining.c_str(), runStyle);
        const uint16_t remRows = static_cast<uint16_t>(
            std::max(1, static_cast<int>(std::ceil(static_cast<double>(remWidthPx + runDownNudge) / cellPx))));
        const uint16_t availRows = rowsPerColumn - row;

        if (remRows <= availRows) {
          // Fits in the current column.
          const int topY = row * cellPx;
          VerticalGlyph g;
          g.codepoint = 0;
          g.column = column;
          g.row = row;
          g.x = static_cast<uint16_t>(columnLeftX(column) + cellPx - ascender);
          g.y = static_cast<uint16_t>(topY + runDownNudge);
          g.paragraphIndex = pc.paragraphIndex;
          g.byteOffset = pc.byteOffset;
          g.style = pc.style;
          g.renderKind = VerticalGlyph::RotatedRun;
          g.rotatedRunText = remaining;
          pushGlyph(page.glyphs, g);
          row = static_cast<uint16_t>(row + remRows);
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
          const uint16_t prefixRows = static_cast<uint16_t>(
              std::max(1, static_cast<int>(std::ceil(static_cast<double>(prefixPx + runDownNudge) / cellPx))));
          if (prefixRows <= availRows) {
            breakAt = sp;
            break;
          }
        }

        if (breakAt == std::string::npos) {
          // No space-break fits — move to a fresh column.
          if (row != 0) {
            column++;
            row = 0;
            finalizePageIfNeeded();
            row = columnStartRow(false);
          } else {
            // Already at top of column and still doesn't fit — force-place
            // the whole thing to avoid an infinite loop.
            VerticalGlyph g;
            g.codepoint = 0;
            g.column = column;
            g.row = 0;
            g.x = static_cast<uint16_t>(columnLeftX(column) + cellPx - ascender);
            g.y = static_cast<uint16_t>(runDownNudge);
            g.paragraphIndex = pc.paragraphIndex;
            g.byteOffset = pc.byteOffset;
            g.renderKind = VerticalGlyph::RotatedRun;
            g.rotatedRunText = remaining;
            pushGlyph(page.glyphs, g);
            row = std::min(remRows, rowsPerColumn);
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
        const int chunkPx = renderer_.getRenderAdvanceX(fontId_, chunk.c_str(), runStyle);
        const uint16_t chunkRows = static_cast<uint16_t>(
            std::max(1, static_cast<int>(std::ceil(static_cast<double>(chunkPx + runDownNudge) / cellPx))));

        const int topY = row * cellPx;
        VerticalGlyph g;
        g.codepoint = 0;
        g.column = column;
        g.row = row;
        g.x = static_cast<uint16_t>(columnLeftX(column) + cellPx - ascender);
        g.y = static_cast<uint16_t>(topY + runDownNudge);
        g.paragraphIndex = pc.paragraphIndex;
        g.byteOffset = pc.byteOffset;
        g.style = pc.style;
        g.renderKind = VerticalGlyph::RotatedRun;
        g.rotatedRunText = chunk;
        pushGlyph(page.glyphs, g);

        row = static_cast<uint16_t>(row + chunkRows);
        if (row >= rowsPerColumn) {
          column++;
          row = 0;
          finalizePageIfNeeded();
          row = columnStartRow(false);
        }

        // Skip the space and continue with the rest.
        remaining = remaining.substr(breakAt + 1);
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
    if (startingNewColumn && !paraBreakJustFired && Kinsoku::isLineStartProhibited(pc.codepoint)) {
      if (!page.glyphs.empty()) {
        // Oikomi (追い込み): pull this character back into the previous
        // column as an extra row.
        const VerticalGlyph& prev = page.glyphs.back();
        placeUprightAt(pc, prev.column, static_cast<uint16_t>(prev.row + 1));
        idx++;
        continue;
      } else if (!pages.empty()) {
        // Page just broke — pull back to the last column of the previous page.
        VerticalPage& prevPage = pages.back();
        // prevPage.glyphs was reserved for exactly one page's grid capacity when it was created;
        // this oikomi pull-back is the one place that can push a page over that reservation,
        // forcing libstdc++ to reallocate+relocate an already-near-full glyph array. Confirmed via
        // a real device crash inside this exact reallocation. If there's no reservation headroom
        // and heap is tight, skip the pull-back (the character starts the next page/column
        // normally instead) rather than risk it -- a minor formatting nicety, not correctness.
        const bool hasHeadroom = prevPage.glyphs.size() < prevPage.glyphs.capacity();
        if (!prevPage.glyphs.empty() && (hasHeadroom || ESP.getMaxAllocHeap() >= MIN_FREE_HEAP_FOR_RESERVE)) {
          const VerticalGlyph& prev = prevPage.glyphs.back();
          VerticalGlyph g;
          g.codepoint = pc.codepoint;
          g.column = prev.column;
          g.row = static_cast<uint16_t>(prev.row + 1);
          int gx = columnLeftX(prev.column);
          int gy = g.row * cellPx + ascender;
          if (Kinsoku::verticalShiftType(pc.codepoint) == 1) {
            gx += cellPx / 2;
            gy -= cellPx / 2;
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
          pushGlyph(prevPage.glyphs, g);
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
    row++;
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
  const int padX = std::max(2, gap / 2);
  const int padTop = std::max(2, boxGeomCellPx_ / 6);
  // The last row's glyph ink reaches ascender+descender past its cell top, plus the renderer's
  // global down-nudge -- and the reference rendering (Apple Books) leaves roughly a full
  // character of air below the last line. The bottom-reserved strip has the room.
  const int padBottom = std::max(6, (boxGeomCellPx_ * 5) / 4);
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
