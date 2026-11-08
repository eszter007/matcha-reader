// The vertical layout engine: places the character stream that VerticalParsedText collects into
// columns and pages, applying the JLREQ placement rules. Each character is set at its own size;
// columns are as wide as their largest text.
#include <Arduino.h>
#include <Logging.h>
#include <Utf8.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "GfxRenderer.h"
#include "Kinsoku.h"
#include "VerticalLayoutSupport.h"
#include "VerticalParsedText.h"

namespace {

// Ink boxes for the handful of codepoints placed by quadrant. A miss is NOT stored: a probe
// against a not-yet-resident SD font fails transiently, and caching that would place every
// later occurrence by the fallback for the rest of the chapter.
struct InkMemo {
  static constexpr int CAPACITY = 24;
  uint64_t keys[CAPACITY] = {};  // codepoint | style << 24 | scale8 << 32; 0 = free
  GlyphInk inks[CAPACITY];
  int count = 0;
  int scale8 = 8;  // the size lookups and stores are for
  uint64_t keyOf(const uint32_t cp, const uint8_t style) const {
    return cp | (static_cast<uint64_t>(style) << 24) | (static_cast<uint64_t>(scale8) << 32);
  }
  bool lookup(const uint32_t cp, const uint8_t style, GlyphInk* out) const {
    const uint64_t k = keyOf(cp, style);
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

// Which half of its em a punctuation glyph's ink occupies: 0 none (full em), 1 first half,
// 2 second half, 3 centred. JLREQ 3.1.4 states pair spacing in terms of these halves.
int punctEmHalf(const uint32_t cp) {
  if (cp == 0x30FB || cp == 0x00B7) return 3;  // ・
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
}

// Halves of a pair JLREQ 3.1.10 never splits across columns: …… ‥‥ —— ――.
bool isInseparableMark(const uint32_t cp) { return cp == 0x2026 || cp == 0x2025 || cp == 0x2014 || cp == 0x2015; }

// Digits that are set SIDEWAYS in vertical text -- as a tate-chu-yoko pair, or as a rotated run.
// Halfwidth ASCII only. Fullwidth digits (U+FF10-U+FF19) are East Asian Wide: they are ideographic
// -width characters and stand upright in tategaki, one per cell, exactly like kanji. Gathering them
// into a rotated run laid １９３８年 on its side across four cells.
bool isAsciiDigit(const uint32_t cp) { return cp >= '0' && cp <= '9'; }

// Units and symbols written after a number (JLREQ cl-18 postfixed abbreviations, and the squared
// unit signs ㎏ ㍉): never split from it (3.1.10).
bool isPostfixUnit(const uint32_t cp) {
  return cp == 0xFF05 || cp == 0x2030 || cp == 0x2103 || cp == 0x00B0 || cp == 0x2032 || cp == 0x2033 || cp == 0x00A2 ||
         cp == 0xFFE0 || (cp >= 0x3300 && cp <= 0x33FF);
}

bool isAsciiAlnum(const uint32_t cp) {
  return (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z');
}

// Exclamation/question marks that books put in tate-chu-yoko pairs (!? / !!). Halfwidth only:
// fullwidth ！？ already render upright as normal CJK cells.
bool isBangOrQuestion(const uint32_t cp) { return cp == '!' || cp == '?'; }

void trimSpaces(std::string& s) {
  const size_t b = s.find_first_not_of(' ');
  if (b == std::string::npos) {
    s.clear();
    return;
  }
  const size_t e = s.find_last_not_of(' ');
  s.assign(s, b, e - b + 1);
}

// The body font at a CSS font-size, measured as it is drawn: the glyph bitmaps scaled by scale8/8
// (GfxRenderer::drawTextScaled). Same calls as GfxRenderer, so the placement rules read the same
// whatever the size; at 8 every call passes through unchanged.
struct ScaledFace {
  const GfxRenderer* renderer = nullptr;
  int scale8 = 8;
  int sc(const int v) const { return scale8 == 8 ? v : static_cast<int>(std::lround(v * scale8 / 8.0)); }
  void ensureSdCardFontReady(const int fontId, const char* text, const uint8_t styles) const {
    renderer->ensureSdCardFontReady(fontId, text, styles);
  }
  int getRenderAdvanceX(const int fontId, const char* text, const EpdFontFamily::Style style) const {
    return sc(renderer->getRenderAdvanceX(fontId, text, style));
  }
  bool getGlyphMetrics(const int fontId, const uint32_t cp, const EpdFontFamily::Style style, int* left, int* width,
                       int* top, int* height) const {
    if (!renderer->getGlyphMetrics(fontId, cp, style, left, width, top, height)) return false;
    *left = sc(*left);
    *width = sc(*width);
    *top = sc(*top);
    *height = sc(*height);
    return true;
  }
  bool verticalPunctInkBox(const int fontId, const uint32_t cp, const EpdFontFamily::Style style, const int cellTopY,
                           const int cellPx, const int shiftType, int* inkTop, int* inkHeight) const {
    if (scale8 == 8) {
      return renderer->verticalPunctInkBox(fontId, cp, style, cellTopY, cellPx, shiftType, inkTop, inkHeight);
    }
    return renderer->verticalPunctInkBox(fontId, cp, style, cellTopY, cellPx, shiftType, inkTop, inkHeight,
                                         static_cast<uint16_t>(scale8 * 32));
  }
};

bool measureGlyphInk(const ScaledFace& face, const int fontId, const uint32_t cp, const uint8_t style, GlyphInk* out) {
  int left = 0, width = 0, top = 0, height = 0;
  if (!face.getGlyphMetrics(fontId, cp, static_cast<EpdFontFamily::Style>(style), &left, &width, &top, &height)) {
    return false;
  }
  *out = GlyphInk{left, width, top, height};
  return true;
}

// The page grid for one layout pass: fixed once the font metrics and viewport are known, so the
// placement rules can be read (and moved out of the loop) without carrying a dozen captures.
struct ColumnGeometry {
  int cellPx = 1;            // one em, the kihon-hanmen cell
  int rowPitchPx = 1;        // cell + the character-spacing setting: the step from one row to the next
  int inkGapPx = 0;          // gap a normal adjacent pair leaves between ink boxes
  int baselineInCellPx = 0;  // baseline measured down from the cell top
  int ascenderPx = 0;
  int columnAdvancePx = 1;  // cell + 行間
  int usableWidthPx = 1;
  uint16_t rowsPerColumn = 1;
  uint16_t columnsPerPage = 1;

  // Top of a row's cell. Glyphs are still set in a one-em cell; only the step between cells moves.
  int rowTopPx(const int row) const { return row * rowPitchPx; }
  // The spacing between two cells (may be negative). The last row of a column carries none.
  int rowGapPx() const { return rowPitchPx - cellPx; }

  // Columns are anchored to the right edge and march leftwards. With every column one body em wide;
  // placement works against this and resolveColumnX() moves each column to where its width puts it.
  int columnLeftX(const uint16_t col) const { return usableWidthPx - cellPx - static_cast<int>(col) * columnAdvancePx; }
};

// The cell metrics of text set at scale8/8 of the body size. The page grid (columns, viewport) stays
// the body's; only what one character takes changes.
ColumnGeometry scaledGeometry(const ColumnGeometry& base, const int scale8, const int characterSpacingPx) {
  if (scale8 == 8 || scale8 == 0) return base;
  const auto sc = [scale8](const int v) { return static_cast<int>(std::lround(v * scale8 / 8.0)); };
  ColumnGeometry g = base;
  g.cellPx = std::max(1, sc(base.cellPx));
  g.inkGapPx = sc(base.inkGapPx);
  g.baselineInCellPx = sc(base.baselineInCellPx);
  g.ascenderPx = sc(base.ascenderPx);
  g.rowPitchPx = std::max((g.cellPx + 1) / 2, g.cellPx + characterSpacingPx);
  return g;
}

// Where a right-aligned mark's ink sits along the column.
enum class InkVAlign : uint8_t {
  HalfEmHead,  // 。、 -- centred in the FIRST half em, so the second half is free (JLREQ 3.1.4)
  Centre,      // small kana -- centred in the full em (JLREQ A.11)
  CentreCell,  // 。，、 in traditional Chinese -- centred in the em on BOTH axes, not right-aligned
};

// Places a mark's letter face against the RIGHT edge of its cell, from the glyph's own ink box:
// these characters are drawn toward the bottom-left of the em, so how far the face must travel is
// font-specific. False when metrics are unavailable, which every caller has a fallback for.
template <typename Face>
bool rightAlignedInk(const Face& renderer, const int fontId, InkMemo& memo, const ColumnGeometry& geom,
                     const uint32_t cp, const uint8_t style, const int colLeftX, const int cellTopY,
                     const InkVAlign vAlign, int* gxOut, int* gyOut, int* inkHeightOut = nullptr) {
  GlyphInk ink;
  if (!memo.lookup(cp, style, &ink)) {
    if (!measureGlyphInk(renderer, fontId, cp, style, &ink)) return false;
    memo.store(cp, style, ink);
  }
  if (ink.width <= 0 || ink.height <= 0) return false;
  // Draw resolves ink left as x + glyph->left, and ink top as (cellTop + baselineInCell) - top.
  const int inkTopInCell = vAlign == InkVAlign::HalfEmHead ? std::max(0, (geom.cellPx / 2 - ink.height) / 2)
                                                           : std::max(0, (geom.cellPx - ink.height) / 2);
  *gxOut = vAlign == InkVAlign::CentreCell ? colLeftX + (geom.cellPx - ink.width) / 2 - ink.left
                                           : colLeftX + geom.cellPx - ink.left - ink.width;
  *gyOut = std::max(0, cellTopY + inkTopInCell + ink.top - geom.baselineInCellPx);
  if (inkHeightOut) *inkHeightOut = inkTopInCell + ink.height;
  return true;
}

// getRenderAdvanceX ends at the pen, not at the ink: the last glyph's right side bearing is
// blank. Counting it made every run reserve up to a whole extra cell.
template <typename Face>
int runInkWidth(const Face& renderer, const int fontId, const std::string& s, const int advanceWidth,
                const EpdFontFamily::Style style) {
  if (s.empty()) return advanceWidth;
  size_t lastStart = s.size() - 1;
  while (lastStart > 0 && (static_cast<unsigned char>(s[lastStart]) & 0xC0) == 0x80) lastStart--;
  const std::string lastChar = s.substr(lastStart);
  // Full decode, not a 2-byte shortcut: a rotated run may end in a 3-byte codepoint (U+2122 (TM)
  // is in Kinsoku::isRotatedRunCharacter), and mis-decoding it measured the wrong glyph's ink,
  // fell back to the advance, and reserved an extra row.
  const auto* lastPtr = reinterpret_cast<const unsigned char*>(lastChar.c_str());
  const uint32_t lastCp = utf8NextCodepoint(&lastPtr);
  GlyphInk ink;
  if (!measureGlyphInk(renderer, fontId, lastCp, static_cast<uint8_t>(style), &ink) || ink.width <= 0) {
    return advanceWidth;
  }
  const int lastAdvance = renderer.getRenderAdvanceX(fontId, lastChar.c_str(), style);
  return advanceWidth - std::max(0, lastAdvance - (ink.left + ink.width));
}

// Rows a rotated run occupies: enough that the next character's ink clears the run's, no more.
uint16_t rotatedRunRows(const int rowPitchPx, const int startY, const int inkWidthPx, const uint16_t rowArg,
                        const int nextInkOffset) {
  const int intrusion = (nextInkOffset >= 0) ? nextInkOffset : 0;
  const int endRow = static_cast<int>(std::ceil(static_cast<double>(startY + inkWidthPx - intrusion) / rowPitchPx));
  return static_cast<uint16_t>(std::max(1, endRow - static_cast<int>(rowArg)));
}

}  // namespace

// Members are REFERENCES to layoutPages' own locals and share their names, so a rule moved in
// here reads exactly as it did in the loop; only the owner's members pick up an `o.` prefix.
struct VerticalParsedText::LayoutCursor {
  VerticalParsedText& o;
  const ColumnGeometry& base;  // the body text's cells and the page grid
  InkMemo& inkMemo;
  VerticalPage& page;
  std::vector<VerticalPage>& pages;
  uint16_t& column;
  uint16_t& row;
  int& rowY;         // top of the next cell in this column, before this column's slide
  int& lastCellTop;  // the last placed glyph's cell top, before its column's slide
  int& columnYShift;
  uint16_t& shiftColumn;
  const size_t glyphsPerPage;
  void* const ctx;
  const PageReadyCallback onPageReady;

  // = growForOnePush's LINEAR_GROWTH_STEP for glyphs: the room a page keeps before it must grow.
  static constexpr size_t PAGE_GLYPH_HEADROOM = 16;

  // Can one more glyph be appended without a reallocation that the heap cannot serve?
  bool pageVectorCanTakeMore() const {
    if (page.glyphs.size() + PAGE_GLYPH_HEADROOM <= page.glyphs.capacity()) return true;
    const size_t growBytes = (page.glyphs.capacity() + PAGE_GLYPH_HEADROOM) * sizeof(VerticalGlyph);
    return ESP.getMaxAllocHeap() >= growBytes;
  }

  void reservePageGlyphs(VerticalPage& p) const {
    // A full page plus pageVectorCanTakeMore()'s step twice: a page that fills never has to grow,
    // including the few that hold a cell or two more than glyphsPerPage (ruby, half-width pairs).
    const size_t fullCapacity = glyphsPerPage + 2 * PAGE_GLYPH_HEADROOM;
    if (p.glyphs.capacity() >= fullCapacity) return;  // a recycled page's buffer
    const size_t requestBytes = fullCapacity * sizeof(VerticalGlyph);
    if (heapCanAfford(requestBytes, MIN_FREE_HEAP_FOR_RESERVE)) {
      p.glyphs.reserve(fullCapacity);
      return;
    }
    // Partial fallback: reserve the largest fitting fraction of the page instead of nothing.
    // Starting from capacity 0 makes the vector double 1-2-4-...-256, and every doubling's
    // grow-copy (old + new buffer coexisting) is a fresh chance to land on a heap dip -- on
    // the X3's tighter heap that fired the emergency page split constantly, which the reader
    // sees as pages breaking at random fill levels. This runs only while no recycled buffer of
    // full size exists, so one medium block now prevents most of the risky growth steps later
    // at the dips.
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
    LOG_DBG("VPT", "Skipping page glyphs reserve (%u bytes doesn't fit, free=%u); growing incrementally",
            static_cast<unsigned>(requestBytes), ESP.getMaxAllocHeap());
  }

  // Row where a fresh column starts: 0 normally; inside a styled block, the block's start offset
  // (start-Xem), plus the hanging indent (h-indent-Xem) when the column continues a wrapped
  // paragraph line rather than beginning a new paragraph.
  // Appends a glyph, applying this column's slide and the heap growth ladder. False = dropped.
  bool pushGlyph(VerticalPage& pg, VerticalGlyph g, const std::string& text = std::string()) {
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
    // Tiers, most protective first.
    //   4K: a ~9216-byte linear request must still pass when maxAlloc dips to ~14324, as it does
    //       on a 450+ page chapter. An 8K margin misses, drops glyphs, and marks the section
    //       stale so it re-indexes on every open.
    //   0:  below this the margin protects only allocations that fail gracefully and retry (font
    //       advance tables, staging buffers, the next page's reserve), whereas a dropped glyph is
    //       content permanently missing from the page.
    static constexpr uint32_t MARGINS[] = {SMALL_ALLOC_MARGIN, 4 * 1024, 0};
    constexpr size_t LINEAR_GROWTH_STEP = 16;  // elements; keeps a stalled page's retries cheap
    if (growForOnePush(glyphs, MARGINS, LINEAR_GROWTH_STEP)) {
      g.scale8 = static_cast<uint8_t>(scale8);
      if (&pg == &page) noteWidth(g.column, geom.cellPx);
      glyphs.push_back(g);
      return true;
    }
    LOG_DBG("VPT", "Low heap (%u bytes); dropping glyph", ESP.getMaxAllocHeap());
    o.everDroppedForHeap_ = true;
    return false;
  }

  // JLREQ 3.1.4 pair spacing: two adjacent punctuation halves close up.
  void applyPunctPairSpacing(const uint32_t cp, const uint16_t columnArg) {
    if (page.glyphs.empty()) return;
    const VerticalGlyph& prev = page.glyphs.back();
    if (prev.column != columnArg || prev.codepoint == 0) return;
    const int prevHalf = punctEmHalf(prev.codepoint);
    const int thisHalf = punctEmHalf(cp);
    if (prevHalf == 0 || thisHalf == 0) return;

    // Every pair in the figures closes up by a half em: 、「 and 」「 from a full em to a half
    // (figures 3, 4); 。」, ）。, 「『, ）」 from a half em to solid (1, 2, 5, 6); 」・「 from three
    // quarters to a quarter each side of the dot (7).
    int reduction = geom.cellPx / 2;

    // 。and 、 carry their own trailing half em inside their em, so a following mark closes onto
    // it: an opening bracket keeps half an em between them (figure 3), a closing bracket or
    // another 。/、 is set solid (1, 2), a middle dot keeps a quarter.
    if (Kinsoku::verticalShiftType(prev.codepoint) == 1 && thisHalf == 3) reduction = geom.cellPx / 4;
    columnYShift += reduction;
    shiftColumn = columnArg;
  }

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

  // Where a rotated run may start in this column: on the grid unless the preceding glyph's
  // ink actually reaches into the cell.
  int rotatedRunStartY(const uint16_t columnArg, const int topYArg) const {
    if (page.glyphs.empty()) return topYArg;
    const VerticalGlyph& pg = page.glyphs.back();
    if (pg.column != columnArg) return topYArg;
    const int leadSpacePx = latinLeadSpacePx();
    // Start on the grid; the only reason to push further is a predecessor whose ink actually
    // reaches into this cell, which is measured below.
    int startY = topYArg;
    if (pg.renderKind == VerticalGlyph::RotatedPunct) {
      // Brackets are placed from their cell box and hang roughly a full cell lower than that
      // box (「 opens the character in the NEXT cell), so a run that only stepped one cell
      // started inside the bracket's ink (device photo, 「Furz」).
      int inkTop = 0, inkHeight = 0;
      if (face.verticalPunctInkBox(o.fontId_, pg.codepoint, static_cast<EpdFontFamily::Style>(pg.style),
                                   static_cast<int>(pg.y), geom.cellPx, Kinsoku::verticalShiftType(pg.codepoint),
                                   &inkTop, &inkHeight)) {
        startY = std::max(startY, inkTop + inkHeight + geom.inkGapPx);
      }
    } else if (pg.renderKind == VerticalGlyph::Upright && pg.codepoint != 0) {
      GlyphInk ink;
      if (measureGlyphInk(face, o.fontId_, pg.codepoint, pg.style, &ink) && ink.height > 0) {
        // pg.y already carries this column's slide-up; startY is computed on the raw grid and
        // gets the same slide applied at push time, so compare in raw space.
        const int pgRawY = static_cast<int>(pg.y) + ((pg.column == shiftColumn) ? columnYShift : 0);
        startY = std::max(startY, pgRawY + geom.baselineInCellPx - ink.top + ink.height);
      }
    }
    return startY + leadSpacePx;
  }

  // How far into its cell the next glyph's ink starts, or -1 when there is no next glyph in
  // this paragraph.
  int nextGlyphInkOffset(const size_t nextIdx, const uint32_t paragraphIndex) {
    if (nextIdx >= o.stream_.size() || o.stream_[nextIdx].paragraphIndex != paragraphIndex) return -1;
    const auto& next = o.stream_[nextIdx];
    const uint32_t key = (next.codepoint & 0x00FFFFFFu) | (static_cast<uint32_t>(next.style & 3) << 24);
    auto& slot = inkOffsetCache[key % INK_CACHE_SLOTS];
    if (slot.key == key) return slot.offset;

    const auto nextStyle = static_cast<EpdFontFamily::Style>(next.style);
    int offset = -1;
    if (Kinsoku::needsVerticalRotation(next.codepoint)) {
      int inkTop = 0, inkHeight = 0;
      if (face.verticalPunctInkBox(o.fontId_, next.codepoint, nextStyle, 0, geom.cellPx,
                                   Kinsoku::verticalShiftType(next.codepoint), &inkTop, &inkHeight)) {
        offset = inkTop;
      }
    } else {
      // Metrics for a glyph the SD font has not paged in yet come back empty, which silently
      // disabled the tail allowance; page the one character in first.
      char nextChar[5] = {};
      utf8EncodeCodepoint(next.codepoint, nextChar);
      face.ensureSdCardFontReady(o.fontId_, nextChar, static_cast<uint8_t>(1u << (next.style & 3)));
      GlyphInk ink;
      if (measureGlyphInk(face, o.fontId_, next.codepoint, next.style, &ink) && ink.height > 0) {
        // 。、 are drawn at the head of their cell (rightAlignedInk, HalfEmHead), not on the baseline;
        // their centred traditional-Chinese form (type 5) sits mid-cell.
        const int shift = Kinsoku::verticalShiftType(next.codepoint);
        offset = shift == 1   ? std::max(0, (geom.cellPx / 2 - ink.height) / 2)
                 : shift == 5 ? std::max(0, (geom.cellPx - ink.height) / 2)
                              : geom.baselineInCellPx - ink.top;
      }
    }
    // A miss is worth caching too -- it is the expensive case, and a glyph the font lacks
    // stays missing for the rest of the chapter.
    slot = {key, static_cast<int16_t>(offset)};
    return offset;
  }

  // JLREQ 3.2.6: a quarter em separates a Latin run / European numerals from the Japanese around
  // it. Every exception comes down to "somebody else already owns that space":
  //   line head / line end             nothing
  //   after 。、 or a closing bracket   they carry their own trailing space
  //   after an opening bracket         solid
  //   before 。、 or a closing bracket  solid
  //   before an opening bracket        it carries its own leading space
  // verticalShiftType is non-zero for exactly that punctuation, so one test covers each side.
  int latinLeadSpacePx() const {
    if (page.glyphs.empty()) return 0;
    const VerticalGlyph& pg = page.glyphs.back();
    if (pg.column != column) return 0;  // first thing in this column
    return Kinsoku::verticalShiftType(pg.codepoint) != 0 ? 0 : geom.cellPx / 4;
  }

  int latinTrailSpacePx(const size_t nextIdx, const uint32_t paragraphIndex) const {
    if (nextIdx >= o.stream_.size() || o.stream_[nextIdx].paragraphIndex != paragraphIndex) return 0;
    return Kinsoku::verticalShiftType(o.stream_[nextIdx].codepoint) != 0 ? 0 : geom.cellPx / 4;
  }

  // How many cells this column can still take. The grid says geom.rowsPerColumn, but every
  // tightening -- 。、 and closing brackets keeping a quarter em instead of a whole cell, a
  // rotated run's cell-rounding slack taken up -- slides this column's tail UP by columnYShift.
  // That reclaimed height has to be spent on more characters, or each column ends wherever its
  // punctuation happened to leave it. JLREQ sets every line to the same length; only hanging
  // punctuation passes it. A negative shift (ink pushed down, e.g. the low ellipsis stack)
  // correctly yields fewer rows.
  uint16_t rowsAvailable() const {
    const int shift = (column == shiftColumn) ? columnYShift : 0;
    // Inside a framed block the column stops short of the foot, leaving room for the bottom rule's
    // inset (boxFootReservePx_). Unframed blocks and columns outside a block run to the foot.
    const int reserve = o.inBox_ && o.activeBlock_.borderEdges != 0 ? o.boxFootReservePx_ : 0;
    const int rows = (static_cast<int>(o.viewportHeight_) - reserve + shift + geom.rowGapPx()) / geom.rowPitchPx;
    return static_cast<uint16_t>(std::clamp(rows, 1, static_cast<int>(UINT16_MAX)));
  }

  // JLREQ 3.8 spreading, the counterpart to oikomi below. A column that reclaimed slack inside
  // itself (a rotated run's cell rounding; the slide that keeps a low ellipsis stack clear of the
  // next character) ends short of the text area's foot by LESS than one cell. Nothing can be
  // pulled in to fill it -- oikomi has already run and the next character needs a whole em -- so
  // the leftover is returned to the column's own inter-character gaps and the column ends flush.
  //
  // Progressive, like the squeeze: each row moves by its share (leftover/lastRow), so corrections
  // already applied inside the column survive.
  //
  // Two cases are deliberately left ragged:
  //   - the column ends where its PARAGRAPH ended (shortfall >= one cell). Not a fitting problem;
  //     stretching it would space a two-character line down the whole page. A column oidashi
  //     pushed characters out of ends short mid-paragraph, so it is spread whatever the shortfall.
  //   - the last glyph HANGS past the foot (burasage), i.e. leftover is negative.
  static constexpr int MAX_SPREAD_PER_GAP_DIVISOR = 8;  // em / 8
  // Every column of a finished page at once (the per-column rule below): one pass finds each
  // column's last glyph, a second moves only the columns that spread.
  void spreadPageToFoot(VerticalPage& pg, const uint16_t columns) {
    const uint16_t n = std::min<uint16_t>(columns, MAX_PAGE_COLUMNS);
    if (n == 0) return;
    int16_t* lastRow = o.spreadLastRow_;
    uint16_t* lastY = o.spreadLastY_;
    uint8_t* lastScale = o.spreadLastScale_;
    int16_t* spread = o.spreadPx_;
    std::fill(lastRow, lastRow + n, static_cast<int16_t>(-1));
    for (const auto& g : pg.glyphs) {
      if (g.column >= n || g.row <= lastRow[g.column]) continue;
      lastRow[g.column] = g.row;
      lastY[g.column] = g.y;
      lastScale[g.column] = g.scale8 == 0 ? 8 : g.scale8;
    }
    bool any = false;
    for (uint16_t col = 0; col < n; col++) {
      spread[col] = 0;
      if (lastRow[col] < 1) continue;  // nothing to spread across
      const ColumnGeometry lastGeom =
          lastScale[col] == 8 ? base : scaledGeometry(base, lastScale[col], o.characterSpacingPx_);
      const int leftover = static_cast<int>(o.viewportHeight_) - (lastY[col] + lastGeom.cellPx);
      const bool pushedOut = col < 64 && (o.oidashiColumns_ >> col) & 1u;
      if (leftover <= 0 || (leftover >= lastGeom.rowPitchPx && !pushedOut)) continue;
      spread[col] =
          static_cast<int16_t>(std::min(leftover, lastRow[col] * lastGeom.cellPx / MAX_SPREAD_PER_GAP_DIVISOR));
      any = any || spread[col] != 0;
    }
    if (!any) return;
    for (auto& g : pg.glyphs) {
      if (g.column >= n || spread[g.column] == 0) continue;
      g.y =
          static_cast<uint16_t>(static_cast<int>(g.y) + spread[g.column] * static_cast<int>(g.row) / lastRow[g.column]);
    }
  }

  void spreadColumnToFoot(VerticalPage& pg, const uint16_t col) {
    int lastRow = -1;
    int lastY = 0;
    int lastScale = 8;
    for (const auto& g : pg.glyphs) {
      if (g.column == col && g.row > lastRow) {
        lastRow = g.row;
        lastY = g.y;
        lastScale = g.scale8 == 0 ? 8 : g.scale8;
      }
    }
    if (lastRow < 1) return;  // nothing to spread across
    const ColumnGeometry lastGeom = lastScale == 8 ? base : scaledGeometry(base, lastScale, o.characterSpacingPx_);
    const int leftover = static_cast<int>(o.viewportHeight_) - (lastY + lastGeom.cellPx);
    const bool pushedOut = col < 64 && (o.oidashiColumns_ >> col) & 1u;
    if (leftover <= 0 || (leftover >= lastGeom.rowPitchPx && !pushedOut)) return;
    // At most an eighth of an em between two characters: a column oidashi shortened by several
    // cells otherwise spreads into visibly loose type, so it ends short of the foot instead.
    const int spread = std::min(leftover, lastRow * lastGeom.cellPx / MAX_SPREAD_PER_GAP_DIVISOR);
    for (auto& g : pg.glyphs) {
      if (g.column != col) continue;
      g.y = static_cast<uint16_t>(static_cast<int>(g.y) + spread * static_cast<int>(g.row) / lastRow);
    }
  }

  // JLREQ 3.8 line adjustment, 追い込み (oikomi): recover one cell in column `col` so a character
  // that may not start a line can join it instead of being pushed to the next column.
  //
  // The reducible space is the half em that 。、 and brackets carry inside their own em; 3.8
  // reduces each by a QUARTER em, so four such marks buy one cell. The pull is progressive --
  // only the glyphs AFTER a reduced space move up -- which is why this cannot go through
  // columnYShift, that shifts a whole column uniformly.
  //
  // Two passes: measure first, and only touch the glyphs once a full cell is actually available.
  // A partial squeeze would tighten the column for nothing and still push the character out.
  // What line adjustment may take space from: the half em 。、 and brackets carry inside their own
  // em, and a paragraph's leading ideographic space. Aozora-derived books write that indent as a
  // U+3000 GLYPH rather than CSS, so it is a full em sitting in the column -- giving up half of it
  // still reads as an indent, and it is very often the only thing a short column has to offer.
  // Only 。、 and brackets (types 1-3): dashes and ー (type 4) and centred 。， (type 5) fill their
  // em, and taking from them pulls the next character into their ink.
  static bool isHalfEmMark(const uint32_t cp) {
    const int shift = Kinsoku::verticalShiftType(cp);
    return shift >= 1 && shift <= 3;
  }
  // An opening bracket is set in the second half of its em, so its white half is the one BEFORE
  // it: giving that up moves the bracket itself, not the character after it into its ink.
  static bool givesLeadingSpace(const uint32_t cp) { return Kinsoku::verticalShiftType(cp) == 3; }
  static bool isReducible(const uint32_t cp) { return isHalfEmMark(cp) || cp == 0x3000; }

  // How much room a character actually needs at the end of a column. A half-em mark (a closing
  // bracket, 。、) is set in one half of its em and the other half is white, so half a cell is
  // enough for it -- demanding a whole one is what kept a lone 」 off the column it belongs to.
  int spaceNeededFor(const uint32_t cp) const { return isHalfEmMark(cp) ? (geom.cellPx + 1) / 2 : geom.cellPx; }

  // Where a glyph's cell starts now. 。、 are positioned by their ink, so their y sits above
  // their cell top; measuring a squeeze from it counts that offset as white space still to give,
  // and a second squeeze then prints the next character over the mark.
  int cellTopOf(const VerticalGlyph& g) {
    if (g.renderKind != VerticalGlyph::Upright || Kinsoku::verticalShiftType(g.codepoint) != 1) return g.y;
    const int gScale = g.scale8 == 0 ? 8 : g.scale8;
    const ColumnGeometry gGeom = gScale == scale8 ? geom : geomOf(g);
    const ScaledFace gFace{&o.renderer_, gScale};
    // The grid top only decides whether the mark was clamped at the column head.
    const int gridTop = gGeom.rowTopPx(g.row);
    int qx = 0, placedY = 0;
    const int memoScale = inkMemo.scale8;
    inkMemo.scale8 = gScale;
    const bool measured = rightAlignedInk(gFace, o.fontId_, inkMemo, gGeom, g.codepoint, g.style,
                                          base.columnLeftX(g.column), gridTop, InkVAlign::HalfEmHead, &qx, &placedY);
    inkMemo.scale8 = memoScale;
    if (!measured) placedY = std::max(0, gridTop - gGeom.cellPx / 2);
    return static_cast<int>(g.y) + gridTop - placedY;
  }

  // Returns the pixels actually recovered (0 when the column has nothing to give). The caller
  // places its character `applied` px above the grid row, since the tail moved up by that much.
  //
  // A column can be squeezed more than once (……？ pulls in two characters that may not start a
  // line), so what a mark may still give is read from where its neighbours sit now: a mark keeps
  // at least half its em whatever earlier squeezes took, or the next character lands in its ink.
  int squeezeColumnForOneMore(const uint16_t col, const int neededPx) {
    size_t first = page.glyphs.size();
    while (first > 0 && page.glyphs[first - 1].column == col) first--;
    if (first == page.glyphs.size()) return 0;

    // A mark may give up to the half em it carries. Round that cap UP: on an odd cell, two floored
    // halves sum to one pixel less than the whole (14 + 14 against a 29px cell), which made every
    // two-mark column miss by a pixel and fail.
    const int maxPerMark = (geom.cellPx + 1) / 2;
    const int keep = geom.cellPx - maxPerMark;

    // The LAST glyph's trailing space is what the new character will occupy, so it is not itself
    // reducible. A column is at most a few dozen cells; more marks than this is not a real column.
    static constexpr size_t MAX_MARKS = 64;
    int16_t avail[MAX_MARKS];
    size_t marks = 0;
    int total = 0;
    for (size_t i = first; i + 1 < page.glyphs.size(); i++) {
      const uint32_t cp = page.glyphs[i].codepoint;
      if (!isReducible(cp)) continue;
      if (marks == MAX_MARKS) return 0;
      // The white half before an opening bracket sits between it and the previous glyph; every
      // other mark's white half is between it and the next one.
      int gap = geom.cellPx;
      if (!givesLeadingSpace(cp)) {
        gap = cellTopOf(page.glyphs[i + 1]) - cellTopOf(page.glyphs[i]);
      } else if (i > first) {
        gap = cellTopOf(page.glyphs[i]) - cellTopOf(page.glyphs[i - 1]);
      }
      const int a = std::clamp(gap - keep, 0, maxPerMark);
      avail[marks++] = static_cast<int16_t>(a);
      total += a;
    }
    if (total < neededPx) return 0;

    // Spread the space being recovered evenly: the smallest per-mark level that covers what is
    // needed, then trim the overshoot from the last marks at that level. With four or more marks
    // nobody gives more than the quarter em 3.8 prefers.
    int level = 0;
    int covered = 0;
    while (covered < neededPx) {
      level++;
      covered = 0;
      for (size_t k = 0; k < marks; k++) covered += std::min<int>(avail[k], level);
    }
    for (size_t k = 0; k < marks; k++) avail[k] = static_cast<int16_t>(std::min<int>(avail[k], level));
    for (size_t k = marks; k > 0 && covered > neededPx; k--) {
      if (avail[k - 1] == level) {
        avail[k - 1]--;
        covered--;
      }
    }

    int applied = 0;
    size_t k = 0;
    for (size_t i = first; i < page.glyphs.size(); i++) {
      const bool gives = i + 1 < page.glyphs.size() && isReducible(page.glyphs[i].codepoint);
      const int give = gives ? avail[k++] : 0;
      const bool leading = gives && givesLeadingSpace(page.glyphs[i].codepoint);
      if (leading) applied += give;  // the bracket rises into its own white half
      if (applied > 0) {
        page.glyphs[i].y = static_cast<uint16_t>(std::max(0, static_cast<int>(page.glyphs[i].y) - applied));
      }
      if (gives && !leading) applied += give;  // what follows rises into the mark's trailing half
    }
    return applied;
  }

  // Place a character in a cell the squeeze just freed: placeUprightAt works off the grid, so it
  // has to come up by the same amount the column's tail did.
  // The new character's cell starts one row below the last one's, wherever squeezes left that: the
  // last glyph has risen by this squeeze and by any before it in the column.
  void placeAfterSqueeze(const PendingChar& pc, const uint16_t col, const uint16_t rowIdx, const int rawTopY,
                         const int applied) {
    if (page.glyphs.empty()) return;
    const int cellTop = cellTopOf(page.glyphs.back()) + geom.rowPitchPx;
    const size_t before = page.glyphs.size();
    placeUprightAt(pc, col, rowIdx, rawTopY);
    if (page.glyphs.size() == before) return;
    VerticalGlyph& placed = page.glyphs.back();
    const int lift = applied > 0 ? cellTopOf(placed) - cellTop : 0;
    placed.y = static_cast<uint16_t>(std::max(0, static_cast<int>(placed.y) - lift));
  }

  // JLREQ 3.1.4: two adjacent half-em marks SHARE one em -- the first takes the first half, the
  // second the second. Covers 。」 and 、」 as well as 』」 and 」」, since a closing bracket is
  // itself a half-em glyph set in the first half of its cell. The half the first mark left free
  // is exactly where the second belongs: no new cell, and neither character leaves the column its
  // sentence ended in. A FULL-em character before the bracket (？」) genuinely needs 1.5 em, so
  // this declines and the caller falls back.
  bool placeHalfEmPair(const PendingChar& pc) {
    if (page.glyphs.empty()) return false;
    const VerticalGlyph& prev = page.glyphs.back();
    const int prevHalfEm = Kinsoku::verticalShiftType(prev.codepoint);
    if ((prevHalfEm != 1 && prevHalfEm != 2) || Kinsoku::verticalShiftType(pc.codepoint) != 2 ||
        !Kinsoku::needsVerticalRotation(pc.codepoint)) {
      return false;
    }
    VerticalGlyph g;
    g.codepoint = pc.codepoint;
    g.column = prev.column;
    g.row = prev.row;
    g.x = static_cast<uint16_t>(base.columnLeftX(prev.column));
    // Raw grid position: pushGlyph applies this column's slide, as every other path expects.
    g.y = static_cast<uint16_t>(lastCellTop + geom.cellPx / 2);
    g.renderKind = VerticalGlyph::RotatedPunct;
    g.paragraphIndex = pc.paragraphIndex;
    g.byteOffset = pc.byteOffset;
    g.style = pc.style;
    g.emphasis = pc.emphasis;
    pushGlyph(page, g, o.rubyOf(pc));  // shares the previous mark's cell: lastCellTop stays
    return true;
  }

  // Take up the cell-rounding leftover after a run: the rest of the column slides up by it, so
  // the next character follows at normal spacing instead of after a partly empty cell.
  void takeUpRunSlack(const int startY, const int inkWidthPx, const uint16_t rowAfter, const uint16_t columnArg,
                      const int nextInkOffset, const int trailSpacePx) {
    (void)rowAfter;  // == row: the run has just been advanced past
    if (nextInkOffset < 0 || rowsLeft() <= 0) return;
    const int nextGridInkTop = rowY + nextInkOffset;
    // Leave the run's trailing space (3.2.6), never less than the gap a normal pair leaves.
    const int slack = nextGridInkTop - (startY + inkWidthPx) - std::max(geom.inkGapPx, trailSpacePx);
    if (slack > 0) {
      columnYShift += slack;
      shiftColumn = columnArg;
    }
  }

  // Close the page when the columns run out: emit it, and open a fresh one.
  void finalizePageIfNeeded() {
    if (pageFull()) {
      forcePageBreak = false;
      // A box spanning the page boundary gets one rect per page: close it at this page's last
      // column and continue from column 0 on the next page.
      if (o.inBox_) {
        const uint16_t lastColumn = static_cast<uint16_t>(column > 0 ? column - 1 : 0);
        o.appendBoxRectToPage(page, o.boxStartCol_, lastColumn,
                              /*openLeft=*/true, /*openRight=*/o.boxContinuedFromPrevPage_);
        if (o.activeBlock_.aligns()) {
          o.alignBlockColumns(page, o.boxStartCol_, lastColumn);
        }
        o.boxStartCol_ = 0;
        o.boxContinuedFromPrevPage_ = true;
      }
      spreadPageToFoot(page, column);
      o.oidashiColumns_ = 0;
      o.resolveColumnX(page);
      pages.push_back(std::move(page));
      o.anyPageEverProduced_ = true;
      // Stream out everything except the single most-recently-completed page: the oikomi
      // pull-back check below only ever looks at pages.back() (the page immediately before the
      // one currently being started), so once THIS page is pushed, any earlier page in `pages`
      // can never be touched again and is safe to write out and free now.
      if (onPageReady) {
        while (pages.size() > 1) {
          onPageReady(ctx, pages.front());
          o.recyclePage(std::move(pages.front()));
          pages.erase(pages.begin());
        }
      }
      // Only the columns this page used hold widths.
      const size_t usedColumns = std::min<size_t>(static_cast<size_t>(column) + 1, MAX_PAGE_COLUMNS);
      o.prevPageColWidened_ = o.pendingColWidened_;
      if (o.pendingColWidened_) {
        std::copy(o.pendingColW_, o.pendingColW_ + usedColumns, o.prevPageColW_);
        std::fill(o.prevPageColW_ + usedColumns, std::end(o.prevPageColW_), 0);
      }
      std::fill(o.pendingColW_, o.pendingColW_ + usedColumns, 0);
      o.pendingColWidened_ = false;
      widthBeforeCol = UINT16_MAX;
      page = std::move(o.recycledPage_);
      page.clearKeepingCapacity();
      o.recycledPage_ = VerticalPage{};
      page.columnCount = base.columnsPerPage;
      page.rowsPerColumn = geom.rowsPerColumn;
      // A break decided while placing a character opens this page WITH that character on it, so
      // the page starts at its position. The per-character stamp cannot supply this: it runs
      // before the break, against the page being closed.
      page.visibleTextOffset = o.lastCharOffset_;
      reservePageGlyphs(page);
      column = 0;
      setRow(0);
    }
  }

  // Kinsoku at a PAGE boundary, decided before the page is handed over. A character that may not
  // start a line cannot be pulled back once this page is emitted -- the in-page rules all need the
  // previous glyph, and a fresh page has none -- so it ends up stranded at the top of the next
  // page, or alone on a page of its own. Resolve it here, while the closing column is still in
  // hand, in the same order the in-page rules use. `row` is the row just past the column's last
  // glyph. Returns true when the character was absorbed, and advances `idx` past it.
  bool absorbIntoClosingColumn(size_t& idx) {
    if (idx >= o.stream_.size() || page.glyphs.empty() || column == 0) return false;
    const PendingChar& next = o.stream_[idx];
    if (next.paragraphIndex != page.glyphs.back().paragraphIndex) return false;
    if (!Kinsoku::isLineStartProhibited(next.codepoint)) return false;
    const uint16_t prevColumn = static_cast<uint16_t>(column - 1);
    if (Kinsoku::verticalShiftType(next.codepoint) == 1) {
      // 。/、 hang past the column end (JLREQ 3.1.9). Only these may.
      placeUprightAt(next, prevColumn, row, rowY);
    } else if (placeHalfEmPair(next)) {
      // A closing bracket after a half-em mark shares its cell -- no room needed at all.
    } else if (const int applied = squeezeColumnForOneMore(prevColumn, spaceNeededFor(next.codepoint))) {
      // 3.8 line adjustment freed room in the closing column.
      placeAfterSqueeze(next, prevColumn, row, rowY, applied);
    } else {
      return false;
    }
    idx++;
    return true;
  }

  // Place one upright character in a given cell, applying the JLREQ quadrant/half-em rules.
  // `cellTopY` is the top of its cell before the column's slide (pushGlyph applies that).
  void placeUprightAt(const PendingChar& pc, uint16_t col, uint16_t rowIdx, const int cellTopY) {
    applyPunctPairSpacing(pc.codepoint, col);
    lastCellTop = cellTopY;
    const int colLeftX = base.columnLeftX(col);
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
      g.x = static_cast<uint16_t>(colLeftX);
      g.y = static_cast<uint16_t>(cellTopY);
      g.renderKind = VerticalGlyph::RotatedPunct;
      // JLREQ: an opening bracket at the LINE HEAD is set flush to it (tentsuki), the half em
      // before it deleted. "Line head" is the first glyph of THIS column, not row 0 -- a styled
      // block (CSS indent, hanging indent) opens its column further down, and dialogue paragraphs
      // are typically indented. Checked before the push, when the bracket is not yet the last.
      const bool atLineHead = page.glyphs.empty() || page.glyphs.back().column != col;
      const bool flushOpeningBracket = atLineHead && Kinsoku::verticalShiftType(pc.codepoint) == 3;
      if (flushOpeningBracket) g.lineHeadFlush = 1;
      pushGlyph(page, g, o.rubyOf(pc));
      if (flushOpeningBracket) {
        // The half em it no longer needs comes off the rest of the column.
        columnYShift += geom.cellPx / 2;
        shiftColumn = col;
      }
      return;
    }

    if (Kinsoku::isSmallKana(pc.codepoint)) {
      // JLREQ A.11: in vertical writing the letter face of a small kana (cl-11: ぁぃぅ ァィゥ っゃゅょ)
      // is centred VERTICALLY in the frame and set right of its horizontal centre -- not against
      // the frame's top edge, which is where the horizontal-mode intuition puts it.
      int qx = 0, qy = 0;
      if (rightAlignedInk(face, o.fontId_, inkMemo, geom, pc.codepoint, pc.style, colLeftX, cellTopY, InkVAlign::Centre,
                          &qx, &qy)) {
        g.x = static_cast<uint16_t>(qx);
        g.y = static_cast<uint16_t>(qy);
      } else {
        g.x = static_cast<uint16_t>(colLeftX + std::max(1, geom.cellPx / 8));
        g.y = static_cast<uint16_t>(cellTopY);
      }
      g.renderKind = VerticalGlyph::Upright;
      pushGlyph(page, g, o.rubyOf(pc));
      return;
    }

    int gx = colLeftX;
    int gy = cellTopY;
    if (pc.codepoint >= '0' && pc.codepoint <= '9') {
      GlyphInk ink;
      if (measureGlyphInk(face, o.fontId_, pc.codepoint, pc.style, &ink)) {
        gx = colLeftX + (geom.cellPx - ink.width) / 2 - ink.left;
      }
    }
    const int shiftType = Kinsoku::verticalShiftType(pc.codepoint);
    if (shiftType == 1) {
      // 。and 、 occupy a FULL em like every other character -- only their ink sits in one half
      // of it, at the cell's head. Charging them just their ink plus a half em looks tighter per
      // mark but takes a quarter em off the grid at every sentence, so no two columns end at the
      // same depth. Closing up against a neighbour is the pair rules' job (3.1.4), not the
      // mark's own advance. (JLREQ 3.8 does reduce this, but per line and only to make one fit.)
      int qx = 0, qy = 0;
      if (rightAlignedInk(face, o.fontId_, inkMemo, geom, pc.codepoint, pc.style, colLeftX, cellTopY,
                          InkVAlign::HalfEmHead, &qx, &qy)) {
        gx = qx;
        gy = qy;
      } else {
        gx += geom.cellPx / 2;
        gy = std::max(0, gy - geom.cellPx / 2);
      }
    } else if (shiftType == 5) {
      // Traditional Chinese: the mark sits in the middle of its em (CNS 13180 / W3C clreq).
      int qx = 0, qy = 0;
      if (rightAlignedInk(face, o.fontId_, inkMemo, geom, pc.codepoint, pc.style, colLeftX, cellTopY,
                          InkVAlign::CentreCell, &qx, &qy)) {
        gx = qx;
        gy = qy;
      } else {
        gx += geom.cellPx / 4;
        gy = std::max(0, gy - geom.cellPx / 4);
      }
    }
    g.x = static_cast<uint16_t>(gx);
    g.y = static_cast<uint16_t>(gy);
    g.renderKind = VerticalGlyph::Upright;
    pushGlyph(page, g, o.rubyOf(pc));
  }

  // Place a two-character tate-chu-yoko run (a 2-digit number, or a !?/!! pair) upright in one
  // cell, ink-centred on the column, and advance row/column past it.
  void placeTcyPairAt(size_t i0) {
    std::string runUtf8;
    utf8AppendCodepoint(o.stream_[i0].codepoint, runUtf8);
    utf8AppendCodepoint(o.stream_[i0 + 1].codepoint, runUtf8);
    // Measure with the pair's ACTUAL style -- rendered with g.style, and bold digits are
    // wider, so an unstyled measurement mis-centers the pair in its cell.
    const auto tcyStyle = static_cast<EpdFontFamily::Style>(o.stream_[i0].style);
    const auto tcyStyleBit = static_cast<uint8_t>(1u << (o.stream_[i0].style & 3));
    face.ensureSdCardFontReady(o.fontId_, runUtf8.c_str(), tcyStyleBit);
    const int runWidthPx = face.getRenderAdvanceX(o.fontId_, runUtf8.c_str(), tcyStyle);

    // Center the run on its INK box, not its advance width. drawText puts ink at
    // pen + firstGlyph.left, and digit advances carry trailing whitespace, so advance-based
    // centering sat the run visibly right of the column's kanji (device photo, 「築26年」).
    // Mirrors the single-digit ink centering in placeUprightAt.
    int runX = base.columnLeftX(column) + std::max(0, (geom.cellPx - runWidthPx) / 2);
    {
      const uint32_t cpFirst = o.stream_[i0].codepoint;
      const uint32_t cpLast = o.stream_[i0 + 1].codepoint;
      int l1 = 0, w1 = 0, t1 = 0, h1 = 0, lN = 0, wN = 0, tN = 0, hN = 0;
      if (face.getGlyphMetrics(o.fontId_, cpFirst, tcyStyle, &l1, &w1, &t1, &h1) &&
          face.getGlyphMetrics(o.fontId_, cpLast, tcyStyle, &lN, &wN, &tN, &hN)) {
        std::string lastUtf8;
        utf8AppendCodepoint(cpLast, lastUtf8);
        const int lastAdvance = face.getRenderAdvanceX(o.fontId_, lastUtf8.c_str(), tcyStyle);
        // Ink spans pen+l1 .. pen+(runWidthPx-lastAdvance)+lN+wN.
        const int inkWidth = (runWidthPx - lastAdvance + lN + wN) - l1;
        if (inkWidth > 0 && inkWidth <= geom.cellPx) {
          // Align the pair's ink center with the ink center of the column's CJK glyphs rather
          // than the geometric cell center: CJK glyphs carry uneven side bearings, so pure cell
          // centering reads shifted next to them. The previous fixed nudge of
          // -max(4, geom.cellPx/4) was tuned on one photo/font (Kyokasho, 「築26年」) and
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
          face.ensureSdCardFontReady(o.fontId_, "\xe4\xb8\xad", tcyStyleBit);
          if (face.getGlyphMetrics(o.fontId_, 0x4E2D /* 中 */, tcyStyle, &kl, &kw, &kt, &kh) && kw > 0) {
            cjkDelta = (kl + kw / 2) - geom.cellPx / 2;
            const int clampPx = std::max(1, (geom.cellPx - kw) / 2);
            if (cjkDelta > clampPx) cjkDelta = clampPx;
            if (cjkDelta < -clampPx) cjkDelta = -clampPx;
          }
          runX = base.columnLeftX(column) + (geom.cellPx - inkWidth) / 2 - l1 + cjkDelta;
        }
      }
    }

    VerticalGlyph g;
    g.codepoint = 0;
    g.column = column;
    g.row = row;
    g.x = static_cast<uint16_t>(std::max(0, runX));
    g.y = static_cast<uint16_t>(rowY);
    g.paragraphIndex = o.stream_[i0].paragraphIndex;
    g.byteOffset = o.stream_[i0].byteOffset;
    g.style = o.stream_[i0].style;
    g.renderKind = VerticalGlyph::UprightRun;
    lastCellTop = rowY;
    pushGlyph(page, g, runUtf8);

    advanceRows(1);
    if (rowsLeft() <= 0) {
      column++;
      setRow(0);
      finalizePageIfNeeded();
      setRow(columnStartRow(false));
    }
  }

  void placeUpright(const PendingChar& pc) { placeUprightAt(pc, column, row, rowY); }

  uint16_t columnStartRow(const bool paragraphStart) const {
    if (!o.inBox_) return 0;
    const int startRows = static_cast<int>(o.activeBlock_.startEm + 0.5f);
    const int hangRows = paragraphStart ? 0 : static_cast<int>(o.activeBlock_.hangEm + 0.5f);
    // Bounded by rowsAvailable(), not the grid's geom.rowsPerColumn: a boxed column is shorter
    // (the foot reserve, and any negative columnYShift), so the grid bound would let a large
    // block indent start a glyph inside the strip the bottom rule occupies.
    return static_cast<uint16_t>(std::min<int>(rowsAvailable() - 1, std::max(0, startRows + hangRows)));
  }
  // Cells of the current size that still fit below rowY in this column.
  int rowsLeft() const {
    const int shift = (column == shiftColumn) ? columnYShift : 0;
    const int reserve = o.inBox_ && o.activeBlock_.borderEdges != 0 ? o.boxFootReservePx_ : 0;
    return (static_cast<int>(o.viewportHeight_) - reserve - (rowY - shift) + geom.rowPitchPx - geom.cellPx) /
           geom.rowPitchPx;
  }
  // How many cells of the current size fit from `rawTopY` down, with this column's slide.
  int cellsFitFrom(const int rawTopY) const {
    const int shift = (column == shiftColumn) ? columnYShift : 0;
    const int reserve = o.inBox_ && o.activeBlock_.borderEdges != 0 ? o.boxFootReservePx_ : 0;
    return (static_cast<int>(o.viewportHeight_) - reserve - (rawTopY - shift) + geom.rowPitchPx - geom.cellPx) /
           geom.rowPitchPx;
  }
  void setRow(const uint16_t r) {
    row = r;
    rowY = static_cast<int>(r) * geom.rowPitchPx;
  }
  void advanceRows(const int n) {
    row = static_cast<uint16_t>(row + n);
    rowY += n * geom.rowPitchPx;
  }
  // --- The size of the text being placed -----------------------------------------------------
  ColumnGeometry geom{};  // the current character's cells: base, or scaled by its CSS font-size
  ScaledFace face{};
  int scale8 = 8;
  // Sets the cells and measurements to the size of the text about to be placed.
  void useScale(int s8) {
    if (s8 == 0) s8 = 8;
    if (s8 == scale8 && face.renderer) return;
    scale8 = s8;
    geom = scaledGeometry(base, s8, o.characterSpacingPx_);
    face = ScaledFace{&o.renderer_, s8};
    inkMemo.scale8 = s8;
  }
  ColumnGeometry geomOf(const VerticalGlyph& g) const {
    return scaledGeometry(base, g.scale8 == 0 ? 8 : g.scale8, o.characterSpacingPx_);
  }

  // --- Column widths on the page being filled ---------------------------------------------------
  // A column is as wide as the largest text in it; one with nothing measured yet is a body em.
  int widthOf(const uint16_t col) const {
    return col < MAX_PAGE_COLUMNS && o.pendingColW_[col] != 0 ? o.pendingColW_[col] : base.cellPx;
  }
  void noteWidth(const uint16_t col, const int px) {
    if (col >= MAX_PAGE_COLUMNS || px <= o.pendingColW_[col]) return;
    o.pendingColW_[col] = static_cast<uint16_t>(px);
    if (px != base.cellPx) o.pendingColWidened_ = true;
    if (col < widthBeforeCol) widthBeforeCol = UINT16_MAX;  // a column it summed got wider
  }
  // Width taken by the columns right of `col`, each with its gap. Asked for the current column on
  // nearly every character, so the last sum is kept until a column it covers changes.
  mutable uint16_t widthBeforeCol = UINT16_MAX;
  mutable int widthBeforePx = 0;
  int widthBefore(const uint16_t col) const {
    if (col != widthBeforeCol) {
      int used = 0;
      for (uint16_t c = 0; c < col; c++) used += widthOf(c) + o.columnGapPx_;
      widthBeforeCol = col;
      widthBeforePx = used;
    }
    return widthBeforePx;
  }
  bool forcePageBreak = false;  // close the page at the next finalizePageIfNeeded(), however full
  // True when not even a body-width column fits left of the ones already set.
  bool pageFull() const { return forcePageBreak || widthBefore(column) + base.cellPx > base.usableWidthPx; }
  // Text larger than the body at the head of a column that its width no longer fits starts the next
  // page instead of running off the left edge.
  void makeRoomForWidth() {
    if (geom.cellPx <= widthOf(column) || column == 0) return;
    const bool columnEmpty = page.glyphs.empty() || page.glyphs.back().column < column;
    if (!columnEmpty || widthBefore(column) + geom.cellPx <= base.usableWidthPx) return;
    forcePageBreak = true;
    finalizePageIfNeeded();
    setRow(columnStartRow(false));
  }

  // --- Driver state for one layoutPages() pass ------------------------------------------------
  bool isFinalFlush = true;
  size_t nextParagraphBreakIdx = 0;
  size_t nextBoxStartIdx = 0;
  size_t nextBoxEndIdx = 0;
  size_t suppressKinsokuUntilIdx = 0;  // watchdog escape hatch: place plainly up to here

  // Ends the current column; a new page opens when this one has no column left.
  void breakColumn() {
    column++;
    setRow(0);
    finalizePageIfNeeded();
  }
  // Ends the current column and opens the next one at its start row.
  void nextColumn() {
    breakColumn();
    setRow(columnStartRow(false));
  }
  // Opens a fresh column unless the current one is still empty.
  void ensureFreshColumn() {
    if (row != 0) breakColumn();
  }

  // Where the next paragraph begins, or the batch's end: runs are gathered only up to it.
  size_t boundaryLimit() const {
    return nextParagraphBreakIdx < o.paragraphBreaksBeforeIndex_.size()
               ? o.paragraphBreaksBeforeIndex_[nextParagraphBreakIdx]
               : o.stream_.size();
  }

  // Box END before the paragraph break: the box's content ended with the previous glyph, so its
  // rect closes at the CURRENT column -- the break below then advances to a fresh one.
  void closeBlocksAt(const size_t idx) {
    while (nextBoxEndIdx < o.boxEndsBeforeIndex_.size() && idx == o.boxEndsBeforeIndex_[nextBoxEndIdx]) {
      if (o.inBox_) {
        o.appendBoxRectToPage(page, o.boxStartCol_, column, /*openLeft=*/false,
                              /*openRight=*/o.boxContinuedFromPrevPage_);
        if (o.activeBlock_.aligns()) o.alignBlockColumns(page, o.boxStartCol_, column);
        // A BORDERED block always takes the gap, whatever margin-after the CSS asks for: its
        // LEFT rule sits in the column gap that the next column's furigana occupies (furigana is
        // set to the left of its own column in tategaki), so anything placed immediately after
        // the box collides with the rule. Only a whole blank column clears it.
        const bool wantAfterGap = o.activeBlock_.afterEm >= 0.75f || o.activeBlock_.borderEdges != 0;
        o.inBox_ = false;
        o.boxContinuedFromPrevPage_ = false;
        // Content after the block must not share its last column.
        ensureFreshColumn();
        // m-after-Xem approximation: one blank column of extra separation.
        if (wantAfterGap && column != 0) breakColumn();
      }
      nextBoxEndIdx++;
    }
  }

  // Forces a fresh column at the start of every paragraph after the first, the same way
  // horizontal layout starts a new line per paragraph. True when a break fired here: kinsoku
  // governs WRAPPED line starts, not author-intended paragraph openings, so a paragraph beginning
  // with prohibited punctuation (……でも) keeps its fresh column.
  bool startParagraphsAt(const size_t idx) {
    bool fired = false;
    while (nextParagraphBreakIdx < o.paragraphBreaksBeforeIndex_.size() &&
           idx == o.paragraphBreaksBeforeIndex_[nextParagraphBreakIdx]) {
      fired = true;
      ensureFreshColumn();
      setRow(columnStartRow(true));
      // The book's text-indent: the paragraph's first column starts that many cells down.
      if (idx < o.stream_.size() && o.stream_[idx].indentQuarters > 0) {
        const int indentRows = (o.stream_[idx].indentQuarters + 2) / 4;
        setRow(static_cast<uint16_t>(std::min<int>(rowsAvailable() - 1, row + indentRows)));
      }
      nextParagraphBreakIdx++;
    }
    return fired;
  }

  // Box START after the paragraph break has advanced to the box's first (fresh) column.
  void openBlocksAt(const size_t idx) {
    while (nextBoxStartIdx < o.boxStartsBeforeIndex_.size() && idx == o.boxStartsBeforeIndex_[nextBoxStartIdx]) {
      // Consume this marker's params unconditionally -- markers and queue entries are recorded
      // 1:1, and skipping a pop (e.g. a start while already in a block) would desync every
      // later block's params.
      VerticalBlockParams params;
      if (!o.blockParamsQueue_.empty()) {
        params = o.blockParamsQueue_.front();
        o.blockParamsQueue_.erase(o.blockParamsQueue_.begin());
      }
      if (!o.inBox_) {
        // Defensive: if no break fired (block element without a <p>), still start fresh.
        ensureFreshColumn();
        // m-before-Xem approximation: one blank column of extra separation. A bordered block
        // always takes it -- the mirror of the after-gap above, since its RIGHT rule sits in the
        // gap belonging to the preceding column and collides with that column's furigana.
        if ((params.beforeEm >= 0.75f || params.borderEdges != 0) && column != 0) breakColumn();
        o.activeBlock_ = params;
        o.inBox_ = true;
        o.boxStartCol_ = column;
        setRow(columnStartRow(true));
        LOG_DBG("VPT", "block active at col=%u row=%u start=%.1f hang=%.1f edges=0x%X", column, row,
                o.activeBlock_.startEm, o.activeBlock_.hangEm, o.activeBlock_.borderEdges);
      }
      nextBoxStartIdx++;
    }
  }

  // An inline image: its own columns, all on one page.
  bool placeInlineImage(size_t& idx) {
    const PendingChar& pc = o.stream_[idx];
    if (!isImageMarker(pc.codepoint)) return false;
    const uint32_t id = pc.codepoint - IMAGE_MARKER_BASE;
    if (id < o.inlineImages_.size()) {
      const uint16_t span = std::max<uint16_t>(1, std::min<uint16_t>(o.inlineImages_[id].columns, base.columnsPerPage));
      ensureFreshColumn();
      // All of its columns on one page: start the next page when the rest of this one is short.
      if (column != 0 &&
          widthBefore(column) + span * (base.cellPx + o.columnGapPx_) - o.columnGapPx_ > base.usableWidthPx) {
        forcePageBreak = true;
        finalizePageIfNeeded();
      }
      VerticalGlyph g;
      g.codepoint = pc.codepoint;
      g.column = static_cast<uint16_t>(column + span - 1);  // leftmost column it covers
      g.row = 0;
      // Flush with the right edge of its first (rightmost) column, extending left over the rest.
      const int rightEdge = base.columnLeftX(static_cast<uint16_t>(column)) + geom.cellPx;
      g.x = static_cast<uint16_t>(std::max(0, rightEdge - o.inlineImages_[id].widthPx));
      g.y = 0;
      g.paragraphIndex = pc.paragraphIndex;
      g.byteOffset = pc.byteOffset;
      g.style = pc.style;
      g.renderKind = VerticalGlyph::Upright;
      lastCellTop = 0;
      pushGlyph(page, g, o.inlineImages_[id].info);
      column += span;
      setRow(0);
      finalizePageIfNeeded();
      setRow(columnStartRow(true));
    }
    idx++;
    return true;
  }

  // Tate-chu-yoko for !?/!! pairs: JP books mark these with a tcy span, but like the digit path
  // they are detected directly so books without the class benefit too. Only a run of exactly two
  // marks qualifies, and only when no ASCII letter/digit adjoins it -- "Hello!?" stays part of the
  // rotated latin run.
  bool placeBangPair(size_t& idx, const size_t limit) {
    if (!isBangOrQuestion(o.stream_[idx].codepoint)) return false;
    const bool prevAlnum = idx > 0 && isAsciiAlnum(o.stream_[idx - 1].codepoint);
    size_t markEnd = idx;
    while (markEnd < limit && isBangOrQuestion(o.stream_[markEnd].codepoint)) markEnd++;
    const bool nextAlnum = markEnd < o.stream_.size() && isAsciiAlnum(o.stream_[markEnd].codepoint);
    if (markEnd - idx != 2 || prevAlnum || nextAlnum) return false;
    placeTcyPairAt(idx);
    idx = markEnd;
    return true;
  }

  // Places a sideways run glyph at the current column and row, starting at startY.
  void pushRotatedRun(const PendingChar& pc, const int startY, const std::string& text) {
    lastCellTop = rowY;
    VerticalGlyph g;
    g.codepoint = 0;
    g.column = column;
    g.row = row;
    g.x = static_cast<uint16_t>(base.columnLeftX(column));
    g.y = static_cast<uint16_t>(startY);
    g.paragraphIndex = pc.paragraphIndex;
    g.byteOffset = pc.byteOffset;
    g.style = pc.style;
    g.renderKind = VerticalGlyph::RotatedRun;
    pushGlyph(page, g, text);
  }

  // Halfwidth digits: a pair is set as tate-chu-yoko, a longer number sideways, a single digit
  // upright. A number written into a Latin word or with a Latin unit (10km, 50%, A4) is left to the
  // rotated-run path, set sideways with it as one run, so the two never part (3.1.10).
  bool placeNumber(size_t& idx, const size_t limit) {
    const PendingChar& pc = o.stream_[idx];
    if (!isAsciiDigit(pc.codepoint)) return false;
    size_t digitEnd = idx;
    while (digitEnd < limit && isAsciiDigit(o.stream_[digitEnd].codepoint)) digitEnd++;
    const uint32_t after = digitEnd < limit ? o.stream_[digitEnd].codepoint : 0;
    const bool numberInWord = digitEnd < limit && o.stream_[digitEnd].paragraphIndex == pc.paragraphIndex &&
                              ((after >= 'A' && after <= 'Z') || (after >= 'a' && after <= 'z') || after == '%');
    if (numberInWord) return false;
    const size_t digitCount = digitEnd - idx;

    // 3.1.10: a number keeps its unit. One the rest of the column cannot take with the unit after
    // it starts the next column (a longer run checks its own length below).
    const int unitRows = digitEnd < limit && isPostfixUnit(o.stream_[digitEnd].codepoint) ? 1 : 0;
    if (digitCount <= 2 && unitRows && row > columnStartRow(false) && 1 + unitRows > rowsLeft()) {
      if (column < 64) o.oidashiColumns_ |= uint64_t{1} << column;  // spread to its foot
      nextColumn();
    }

    if (digitCount == 2) {
      placeTcyPairAt(idx);
      idx = digitEnd;
      return true;
    }

    if (digitCount > 2) {
      std::string runUtf8;
      for (size_t i = idx; i < digitEnd; i++) utf8AppendCodepoint(o.stream_[i].codepoint, runUtf8);
      // Render-truth measurement with the run's ACTUAL style: getRenderAdvanceX resolves glyphs
      // exactly as drawTextRotated90CCW will (the advance-table fast path priced non-resident
      // digits from a companion font and under-reported -- device photo: a year-run measured
      // ~half its drawn width, so 年 overprinted the digits).
      const auto runStyle = static_cast<EpdFontFamily::Style>(pc.style);
      face.ensureSdCardFontReady(o.fontId_, runUtf8.c_str(), static_cast<uint8_t>(1u << (pc.style & 3)));
      const int runWidthPx = face.getRenderAdvanceX(o.fontId_, runUtf8.c_str(), runStyle);
      // Same measured placement as an embedded Latin word: start under the previous glyph's real
      // ink, reserve only what the digits' ink needs. The old fixed 9/10-cell drop was a blunt
      // guard against 4-digit years overprinting the following character (1914年) and left most
      // of a cell empty behind the number (device photo, SCP－1305 だ。).
      const int digitInkWidth = runInkWidth(face, o.fontId_, runUtf8, runWidthPx, runStyle);
      const int nextInkOffset = nextGlyphInkOffset(digitEnd, pc.paragraphIndex);
      int startY = rotatedRunStartY(column, rowY);
      uint16_t rowsNeeded = rotatedRunRows(geom.rowPitchPx, startY, digitInkWidth, row, nextInkOffset);
      if (row != 0 && rowsNeeded + unitRows > rowsLeft()) {
        nextColumn();
        startY = rotatedRunStartY(column, rowY);
        rowsNeeded = rotatedRunRows(geom.rowPitchPx, startY, digitInkWidth, row, nextInkOffset);
      }
      pushRotatedRun(pc, startY, runUtf8);
      const uint16_t digitColumn = column;
      advanceRows(rowsNeeded);
      takeUpRunSlack(startY, digitInkWidth, row, digitColumn, nextInkOffset,
                     latinTrailSpacePx(digitEnd, pc.paragraphIndex));
      if (rowsLeft() <= 0) nextColumn();
      idx = digitEnd;
      return true;
    }

    // A single digit stands upright, centred in its cell.
    placeUprightAt(pc, column, row, rowY);
    advanceRows(1);
    if (rowsLeft() <= 0) nextColumn();
    idx++;
    return true;
  }

  // An embedded Latin word or phrase (with any numbers in it), set sideways as one block and split
  // at spaces where it does not fit the column.
  bool placeRotatedRun(size_t& idx, const size_t limit) {
    const PendingChar& pc = o.stream_[idx];
    if (!Kinsoku::isRotatedRunCharacter(pc.codepoint) && !isAsciiDigit(pc.codepoint)) return false;
    size_t runEnd = idx;
    std::string runUtf8;
    while (runEnd < limit &&
           (Kinsoku::isRotatedRunCharacter(o.stream_[runEnd].codepoint) || isAsciiDigit(o.stream_[runEnd].codepoint)) &&
           o.stream_[runEnd].paragraphIndex == pc.paragraphIndex) {
      utf8AppendCodepoint(o.stream_[runEnd].codepoint, runUtf8);
      runEnd++;
      if (runEnd - idx > 64) break;
    }

    // A run that reaches the END of a non-final batch is not this batch's to place: the rest of
    // the word is in the next one, and the gatherer only ever looks within one batch, so placing
    // it now renders "authority" as "au", a blank cell, then "thority". Hold its characters back
    // instead -- the next batch prepends them and gathers the whole word. isFinalFlush means
    // nothing follows, so there the run really is complete.
    if (!isFinalFlush && runEnd == o.stream_.size()) {
      o.carriedRunTail_.assign(std::make_move_iterator(o.stream_.begin() + static_cast<long>(idx)),
                               std::make_move_iterator(o.stream_.end()));
      // rubyPool_ is cleared with the batch: keep the carried characters' ruby apart.
      o.carriedRubyPool_.clear();
      for (auto& c : o.carriedRunTail_) {
        if (!c.rubyLen) continue;
        const uint32_t at = static_cast<uint32_t>(o.carriedRubyPool_.size());
        o.carriedRubyPool_.append(o.rubyPool_, c.rubyOffset, c.rubyLen);
        c.rubyOffset = at;
      }
      idx = runEnd;
      return true;
    }

    // Measure with the run's ACTUAL style (drawn with g.style; bold is wider than an unstyled
    // measurement, which under-reserved rows and overprinted the next glyph).
    const auto runStyle = static_cast<EpdFontFamily::Style>(pc.style);
    face.ensureSdCardFontReady(o.fontId_, runUtf8.c_str(), static_cast<uint8_t>(1u << (pc.style & 3)));
    // JP sources separate embedded Latin from kana with ASCII spaces (それは Germinal や). Drawn
    // verbatim, the leading space pushes the first letter deep into the run's first cell and the
    // trailing space inflates the reserved rows -- the word floats low with a dead cell after it
    // (device photo, Vita Sexualis). The measured start position plus the cell raster already
    // provide the visual separation, so trim boundary spaces and keep only the inner ones (word
    // gaps and break points). A source space before the word (哲学と Sokrates) does not take a
    // cell either: JLREQ 3.2.6's quarter em separates them.
    const int nextInkOffset = nextGlyphInkOffset(runEnd, pc.paragraphIndex);
    std::string remaining = runUtf8;
    trimSpaces(remaining);
    if (remaining.empty()) {
      idx = runEnd;
      return true;
    }
    const auto inkOf = [&](const std::string& s) {
      return runInkWidth(face, o.fontId_, s, face.getRenderAdvanceX(o.fontId_, s.c_str(), runStyle), runStyle);
    };

    // Where the NEXT chunk of this run continues, when a split leaves it in the same column: one
    // word space after the previous chunk's ink, not the next cell boundary -- rounding a word gap
    // up to a whole cell is what made "au thority" read as two words.
    const int wordSpacePx = face.getRenderAdvanceX(o.fontId_, " ", runStyle);
    int continueY = -1;

    while (!remaining.empty()) {
      const int startY = continueY >= 0 ? continueY : rotatedRunStartY(column, rowY);
      const uint16_t remRows = rotatedRunRows(geom.rowPitchPx, startY, inkOf(remaining), row, nextInkOffset);
      const uint16_t availRows = static_cast<uint16_t>(std::max(0, rowsLeft()));

      if (remRows <= availRows) {
        // Fits in the current column.
        pushRotatedRun(pc, startY, remaining);
        const uint16_t runColumn = column;
        advanceRows(remRows);
        takeUpRunSlack(startY, inkOf(remaining), row, runColumn, nextInkOffset,
                       latinTrailSpacePx(runEnd, pc.paragraphIndex));
        if (rowsLeft() <= 0) nextColumn();
        break;
      }

      // Doesn't fit -- find the longest prefix ending at a space that fits within availRows.
      size_t breakAt = std::string::npos;
      for (size_t sp = remaining.rfind(' '); sp != std::string::npos;
           sp = (sp == 0) ? std::string::npos : remaining.rfind(' ', sp - 1)) {
        if (rotatedRunRows(geom.rowPitchPx, startY, inkOf(remaining.substr(0, sp)), row, nextInkOffset) <= availRows) {
          breakAt = sp;
          break;
        }
      }

      if (breakAt == std::string::npos) {
        // No space-break fits. Retry from a fresh column only if the current row is deeper than
        // where a fresh column starts; otherwise force-place.
        //
        // Compare against columnStartRow(false), NEVER 0. Inside a styled block (start-Xem
        // margins such as Aozora/EBPAJ div.mtN, seen up to 22em) fresh columns begin at a
        // non-zero row, so comparing with 0 makes every retry re-seed the same row, leaves the
        // force-place arm unreachable, and loops forever without consuming input. With the
        // correct comparison, one retry leaves row == columnStartRow(false) and force-place is
        // guaranteed on the next pass: termination is structural.
        if (row > columnStartRow(false)) {
          nextColumn();
          continueY = -1;
          continue;
        }
        // At (or above) a fresh column's start row and still doesn't fit -- force-place the whole
        // thing at the CURRENT row to guarantee progress. It may overrun the column bottom (the
        // renderer clips); an unbreakable over-long run has no better placement.
        pushRotatedRun(pc, startY, remaining);
        setRow(static_cast<uint16_t>(row + std::min<int>(remRows, std::max(0, rowsLeft()))));
        if (rowsLeft() <= 0) nextColumn();
        break;
      }

      // Place the prefix chunk.
      std::string chunk = remaining.substr(0, breakAt);
      trimSpaces(chunk);
      if (chunk.empty()) {
        // Nothing but spaces before the break point (double spaces in the source); consume them
        // and retry with the remainder instead of pushing an empty glyph.
        remaining = remaining.substr(breakAt + 1);
        trimSpaces(remaining);
        continue;
      }
      const int chunkInk = inkOf(chunk);
      const uint16_t chunkRows = rotatedRunRows(geom.rowPitchPx, startY, chunkInk, row, nextInkOffset);
      pushRotatedRun(pc, startY, chunk);
      advanceRows(chunkRows);
      if (rowsLeft() <= 0) {
        nextColumn();
        continueY = -1;
      } else {
        continueY = startY + chunkInk + wordSpacePx;
      }
      // Skip the space and continue with the rest.
      remaining = remaining.substr(breakAt + 1);
      trimSpaces(remaining);
    }
    idx = runEnd;
    return true;
  }

  // Ruby (JLREQ 3.3): a group stays in one column, and a reading longer than its base plus what its
  // neighbours may take spaces the base out to it. A column it does not fit ends early and is
  // spread to its foot, like one oidashi pushed a character out of. Anything longer than a whole
  // column still breaks. False when the group needs no spacing: it is then placed character by
  // character like any other text.
  bool placeRubyGroup(size_t& idx, const size_t limit) {
    constexpr uint8_t CONT = EpdFontFamily::RUBY_CONTINUE;
    const PendingChar& pc = o.stream_[idx];
    if (idx < suppressKinsokuUntilIdx || (pc.style & CONT) != 0) return false;
    if (pc.rubyLen == 0 && !(idx + 1 < limit && (o.stream_[idx + 1].style & CONT) != 0)) return false;

    size_t groupEnd = idx + 1;
    while (groupEnd < limit && (o.stream_[groupEnd].style & CONT) != 0) groupEnd++;
    const int n = static_cast<int>(groupEnd - idx);
    const int gapPx = geom.rowPitchPx - geom.cellPx;
    const int basePx = n * geom.cellPx + (n - 1) * gapPx;
    // Ruby is half the body size and set solid (VerticalTextBlock::render draws it so).
    const int rubyLineH = (geom.cellPx + 1) / 2;
    int rubyChars = 0;
    for (size_t k = idx; k < groupEnd; k++) {
      const auto& c = o.stream_[k];
      for (uint32_t b = 0; b < c.rubyLen; b++) {
        if ((static_cast<uint8_t>(o.rubyPool_[c.rubyOffset + b]) & 0xC0) != 0x80) rubyChars++;
      }
    }
    const int rubyPx = o.rubyShown_ ? rubyChars * rubyLineH : 0;
    const auto hasRuby = [&](const size_t at) {
      return o.stream_[at].rubyLen > 0 || (o.stream_[at].style & CONT) != 0 ||
             (at + 1 < o.stream_.size() && (o.stream_[at + 1].style & CONT) != 0);
    };
    // How far the reading may hang over a neighbour (3.3.8): one ruby character over kana, or over
    // the blank half of a closing bracket or 、。 before it and an opening bracket after it.
    const auto hangOver = [&](const size_t at, const bool before) {
      if (at >= limit || o.stream_[at].paragraphIndex != pc.paragraphIndex || hasRuby(at)) return 0;
      const uint32_t cp = o.stream_[at].codepoint;
      const bool kana = (cp >= 0x3041 && cp <= 0x309F) || (cp >= 0x30A1 && cp <= 0x30FA);
      const int shift = Kinsoku::verticalShiftType(cp);
      return kana || (before ? (shift == 1 || shift == 2) : shift == 3) ? rubyLineH : 0;
    };
    const int startRow = columnStartRow(false);
    // The base's length with the reading set against it; whole rows it takes.
    const auto fit = [&](const bool atHead, const bool atFoot, int& spanPx) {
      const int before = atHead || idx == 0 ? 0 : hangOver(idx - 1, true);
      const int after = atFoot ? 0 : hangOver(groupEnd, false);
      spanPx = std::max(basePx, rubyPx - before - after);
      return (spanPx + gapPx + geom.rowPitchPx - 1) / geom.rowPitchPx;
    };
    bool atHead = page.glyphs.empty() || page.glyphs.back().column < column;
    int spanPx = basePx;
    int rows = fit(atHead, false, spanPx);
    if (rows >= rowsLeft()) rows = fit(atHead, true, spanPx);  // ends at the foot
    int freshSpan = basePx;
    if (!atHead && row > startRow && rows > rowsLeft() && startRow + fit(true, false, freshSpan) <= rowsAvailable()) {
      // An opening bracket just before the group may not end the column it leaves: it moves too.
      // Only this batch's own stream entries, as for the oidashi below.
      static constexpr size_t MAX_CARRIED = 2;
      size_t carried = 0;
      while (carried < MAX_CARRIED && idx > carried && page.glyphs.size() > carried) {
        const VerticalGlyph& g = page.glyphs[page.glyphs.size() - 1 - carried];
        const PendingChar& entry = o.stream_[idx - 1 - carried];
        if (g.column != column || !Kinsoku::isLineEndProhibited(g.codepoint) || g.byteOffset != entry.byteOffset ||
            g.paragraphIndex != entry.paragraphIndex) {
          break;
        }
        carried++;
      }
      if (carried > 0 && page.glyphs.size() > carried &&
          page.glyphs[page.glyphs.size() - 1 - carried].column != column) {
        carried = 0;  // the brackets open the column: moving them would leave it empty
      }
      page.glyphs.resize(page.glyphs.size() - carried);
      if (column < 64) o.oidashiColumns_ |= uint64_t{1} << column;
      nextColumn();
      if (carried > 0 && page.glyphs.empty()) page.visibleTextOffset = o.stream_[idx - carried].visibleTextOffset;
      for (size_t k = carried; k > 0; k--) {
        placeUprightAt(o.stream_[idx - k], column, row, rowY);
        advanceRows(1);
      }
      atHead = carried == 0;
      rows = fit(atHead, false, spanPx);
      if (rows >= rowsLeft()) rows = fit(atHead, true, spanPx);
    }
    if (spanPx <= basePx || rows > rowsLeft()) return false;

    // Spaced 1:2:1: half as much at each end as between two characters. The base stays centred on
    // its reading, so at a column head the reading starts with the column (3.3.9).
    const int extra = spanPx - basePx;
    const int unit = extra / (2 * n);
    int offset = unit;
    for (size_t k = idx; k < groupEnd; k++) {
      const uint16_t r = static_cast<uint16_t>(row + offset / geom.rowPitchPx);
      const int gridTop = rowY + (r - row) * geom.rowPitchPx;
      const size_t before = page.glyphs.size();
      placeUprightAt(o.stream_[k], column, r, gridTop);
      if (page.glyphs.size() > before) {
        VerticalGlyph& placed = page.glyphs.back();
        placed.y =
            static_cast<uint16_t>(std::max(0, static_cast<int>(placed.y) + offset - (r - row) * geom.rowPitchPx));
        lastCellTop = rowY + offset;
      }
      offset += geom.rowPitchPx + 2 * unit;
    }
    advanceRows(rows);
    // The rows were rounded up: the rest of the column slides back up by the difference.
    const int slack = rows * geom.rowPitchPx - (spanPx + gapPx);
    if (slack > 0) {
      if (shiftColumn != column) {
        columnYShift = 0;
        shiftColumn = column;
      }
      columnYShift += slack;
    }
    if (rowsLeft() <= 0) nextColumn();
    idx = groupEnd;
    return true;
  }

  // The ellipsis dot stack is drawn low enough to leave its own cell (it has to, or it floats
  // above the rhythm of the upright glyphs around it). Reserve that overflow by sliding the rest of
  // the column down, or the next character prints into the dots (device photo, book 2).
  void clearEllipsisDots(const PendingChar& dots, const int dotsCellTop, const size_t nextIdx,
                         const uint32_t paragraphIndex) {
    const int nextInkOffset = nextGlyphInkOffset(nextIdx, paragraphIndex);
    int inkTop = 0, inkHeight = 0;
    if (nextInkOffset < 0 ||
        !face.verticalPunctInkBox(o.fontId_, dots.codepoint, static_cast<EpdFontFamily::Style>(dots.style), dotsCellTop,
                                  geom.cellPx, Kinsoku::verticalShiftType(dots.codepoint), &inkTop, &inkHeight)) {
      return;
    }
    // The dots sit low in their cell, so close the resulting overlap down to the same ink gap a
    // normal pair of characters leaves.
    const int deficit = (inkTop + inkHeight + geom.inkGapPx) - (rowY + nextInkOffset);
    if (deficit > 0) {
      columnYShift -= deficit;
      shiftColumn = column;
    }
  }

  // A character that may not start a column, arriving at a column head: put it back on the
  // previous column (oikomi, line adjustment, a shared half em, burasage), or move the character
  // before it forward with it (oidashi). True when it was placed; false leaves it for the normal
  // path, possibly after the oidashi already moved its partner into this column.
  bool keepOffColumnHead(size_t& idx) {
    const PendingChar& pc = o.stream_[idx];
    if (!page.glyphs.empty()) {
      const VerticalGlyph& prev = page.glyphs.back();
      // Oikomi (追い込み): put the character on the previous column after all. Free when that column
      // has a row spare; otherwise 3.8 line adjustment can recover one by tightening the
      // punctuation already in it, which keeps 一 with しょ instead of splitting them.
      const int nextTop = lastCellTop + geom.rowPitchPx;  // the cell after it, before the slide
      if (cellsFitFrom(nextTop) >= 1) {
        placeUprightAt(pc, prev.column, static_cast<uint16_t>(prev.row + 1), nextTop);
        idx++;
        return true;
      }
      const uint16_t oikomiCol = prev.column;
      const uint16_t oikomiRow = static_cast<uint16_t>(prev.row + 1);
      if (const int applied = squeezeColumnForOneMore(oikomiCol, spaceNeededFor(pc.codepoint))) {
        placeAfterSqueeze(pc, oikomiCol, oikomiRow, nextTop, applied);
        idx++;
        return true;
      }
      if (placeHalfEmPair(pc)) {
        idx++;
        return true;
      }
      // Burasage (ぶら下げ): 。/、 hang past the end of a full column rather than evict the character
      // before them. They are half-em marks whose ink sits in the top half of the cell, so the
      // overhang is small and falls in the bottom margin -- the same margin ruby uses beside the
      // text (JLREQ Fig 2.37). Without this, oidashi pulled the preceding character along and left
      // a column holding just those two.
      if (Kinsoku::verticalShiftType(pc.codepoint) == 1 && cellsFitFrom(nextTop) == 0) {
        placeUprightAt(pc, prev.column, static_cast<uint16_t>(prev.row + 1), nextTop);
        idx++;
        return true;
      }
      pushOutWithPartner(idx, prev);
    }
    if (page.glyphs.empty() && !pages.empty()) return keepOffPageHead(idx);
    return false;
  }

  // Oidashi (追い出し): move the character BEFORE this one forward, so the pair starts this column
  // together (。」 must not be split across columns).
  //
  // Move it EXPLICITLY into this column. Popping it and rewinding idx does not work: the normal
  // path re-places it in the very cell that was just freed, so the bracket is back at a column head
  // and asks for the same rewind -- forever. That was a live-locked build holding the render lock,
  // i.e. a frozen reader.
  //
  // Only safe when that glyph is this batch's previous stream entry: streamed layout can leave
  // glyphs from an earlier batch on the page, which are not ours to re-place. And only when the
  // pair actually fits, so a short column cannot strand the bracket instead.
  //
  // The character before may itself be the second half of an inseparable pair (…… ‥‥ ——, JLREQ
  // 3.1.11): then both halves move, or ？ after …… would split the dots across columns. The same
  // holds whenever the group's head may not start a line itself (the first … of a …… that only half
  // fits, or the 。 before it): the character before joins, until the group opens on one that may
  // head a column -- た。…… moves as one.
  void pushOutWithPartner(const size_t idx, const VerticalGlyph& prev) {
    const PendingChar& pc = o.stream_[idx];
    const auto isStreamEntry = [&](const VerticalGlyph& g, const size_t at) {
      return g.byteOffset == o.stream_[at].byteOffset && g.paragraphIndex == o.stream_[at].paragraphIndex;
    };
    static constexpr size_t MAX_MOVING = 4;
    size_t moving = 1;
    while (moving < MAX_MOVING && idx > moving && page.glyphs.size() > moving) {
      const VerticalGlyph& head = page.glyphs[page.glyphs.size() - moving];
      const VerticalGlyph& before = page.glyphs[page.glyphs.size() - moving - 1];
      const bool samePair = isInseparableMark(head.codepoint) && before.codepoint == head.codepoint;
      if (!samePair && !Kinsoku::isLineStartProhibited(head.codepoint)) break;
      if (before.column != prev.column || !isStreamEntry(before, idx - moving - 1)) break;
      moving++;
    }
    if (idx < moving || static_cast<int>(moving) >= rowsLeft() || !isStreamEntry(prev, idx - 1)) return;
    if (prev.column < 64) o.oidashiColumns_ |= uint64_t{1} << prev.column;
    page.glyphs.resize(page.glyphs.size() - moving);
    for (size_t k = moving; k > 0; k--) {
      placeUprightAt(o.stream_[idx - k], column, row, rowY);
      advanceRows(1);
    }
    // A moved ellipsis needs the same clearance the normal path gives one: its dots sit low in
    // their cell, and this character would otherwise print into them.
    const PendingChar& moved = o.stream_[idx - 1];
    if (moved.codepoint == 0x2026 || moved.codepoint == 0x2025) {
      clearEllipsisDots(moved, (rowY - geom.rowPitchPx), idx, pc.paragraphIndex);
    }
  }

  // The page just broke before a character that may not start one: pull it back to the last column
  // of the previous page, or move that page's last character over with it.
  bool keepOffPageHead(size_t& idx) {
    const PendingChar& pc = o.stream_[idx];
    VerticalPage& prevPage = pages.back();
    // prevPage.glyphs was reserved for exactly one page's grid capacity when it was created; this
    // oikomi pull-back is the one place that can push a page over that reservation, forcing
    // libstdc++ to reallocate+relocate an already-near-full glyph array. Confirmed via a real
    // device crash inside this exact reallocation. If there's no reservation headroom and heap is
    // tight, skip the pull-back (the character starts the next page/column normally instead)
    // rather than risk it -- a minor formatting nicety, not correctness.
    const bool hasHeadroom = prevPage.glyphs.size() < prevPage.glyphs.capacity();
    // Same bound as the in-page pull-back: only while that column has a row spare.
    // The previous page's last glyph is still the last one placed.
    const int nextTop = lastCellTop + geom.rowPitchPx;
    const bool prevColumnHasRoom = !prevPage.glyphs.empty() && cellsFitFrom(nextTop) >= 1;
    if (prevColumnHasRoom && (hasHeadroom || ESP.getMaxAllocHeap() >= MIN_FREE_HEAP_FOR_RESERVE)) {
      const VerticalGlyph& prev = prevPage.glyphs.back();
      VerticalGlyph g;
      g.codepoint = pc.codepoint;
      g.column = prev.column;
      g.row = static_cast<uint16_t>(prev.row + 1);
      int gx = base.columnLeftX(prev.column);
      int gy = nextTop;
      const int pullShift = Kinsoku::verticalShiftType(pc.codepoint);
      if (pullShift == 1 || pullShift == 5) {
        int qx = 0, qy = 0;
        if (rightAlignedInk(face, o.fontId_, inkMemo, geom, pc.codepoint, pc.style, gx, nextTop,
                            pullShift == 1 ? InkVAlign::HalfEmHead : InkVAlign::CentreCell, &qx, &qy)) {
          gx = qx;
          gy = qy;
        } else if (pullShift == 1) {
          gx += geom.cellPx / 2;
          gy = std::max(0, gy - geom.cellPx / 2);
        }
      }
      g.x = static_cast<uint16_t>(gx);
      g.y = static_cast<uint16_t>(gy);
      g.renderKind = VerticalGlyph::Upright;
      if (Kinsoku::needsVerticalRotation(pc.codepoint)) {
        g.x = static_cast<uint16_t>(base.columnLeftX(prev.column));
        g.y = static_cast<uint16_t>(nextTop);
        g.renderKind = VerticalGlyph::RotatedPunct;
      }
      g.paragraphIndex = pc.paragraphIndex;
      g.byteOffset = pc.byteOffset;
      g.scale8 = static_cast<uint8_t>(scale8);
      if (o.prevPageColWidened_) g.x = static_cast<uint16_t>(std::max(0, g.x + o.columnXDelta(o.prevPageColW_, g)));
      lastCellTop = nextTop;
      pushGlyph(prevPage, g, o.rubyOf(pc));
      idx++;
      return true;
    }
    // Oidashi across the page break: the character that ended the previous page moves to the head
    // of this one, so the pair opens it together. Only one that may start a column, and only this
    // batch's previous stream entry (an earlier batch's glyph is not ours to re-place).
    const VerticalGlyph* last = prevPage.glyphs.empty() ? nullptr : &prevPage.glyphs.back();
    if (!prevColumnHasRoom && last && idx > 0 && last->codepoint != 0 &&
        last->renderKind != VerticalGlyph::RotatedRun && last->renderKind != VerticalGlyph::UprightRun &&
        !isImageMarker(last->codepoint) && !Kinsoku::isLineStartProhibited(last->codepoint) &&
        last->byteOffset == o.stream_[idx - 1].byteOffset &&
        last->paragraphIndex == o.stream_[idx - 1].paragraphIndex && 1 < rowsLeft()) {
      const uint16_t lastColumn = last->column;
      prevPage.glyphs.pop_back();
      // The column it leaves ends short of the foot: spread like any column oidashi shortened.
      if (lastColumn < 64) o.oidashiColumns_ |= uint64_t{1} << lastColumn;
      spreadColumnToFoot(prevPage, lastColumn);
      o.oidashiColumns_ = 0;
      page.visibleTextOffset = o.stream_[idx - 1].visibleTextOffset;
      placeUprightAt(o.stream_[idx - 1], column, row, rowY);
      advanceRows(1);
      // Fall through: this character is placed normally, now one row below its partner.
    }
    return false;
  }

  // One upright character, with the kinsoku rules that decide which column it belongs to.
  void placeCharacter(size_t& idx, const bool paragraphJustStarted) {
    const PendingChar& pc = o.stream_[idx];
    // A column start is NOT always row 0: styled blocks (start-Xem, hanging indents) open their
    // columns further down, which used to dodge this check and let 。/、 head an indented column.
    // "Nothing placed in this column yet" is the real condition -- glyphs are appended in layout
    // order, so the last glyph sitting in an earlier column (or an empty page) means this
    // character would be the column's first.
    const bool startingNewColumn = page.glyphs.empty() || page.glyphs.back().column < column;
    // The second half of a dash or leader pair is held to the first like a character that may not
    // start a column.
    const bool pairsWithPrevious = isInseparableMark(pc.codepoint) && idx > 0 &&
                                   o.stream_[idx - 1].codepoint == pc.codepoint &&
                                   o.stream_[idx - 1].paragraphIndex == pc.paragraphIndex;
    if (startingNewColumn && !paragraphJustStarted && idx >= suppressKinsokuUntilIdx &&
        (Kinsoku::isLineStartProhibited(pc.codepoint) || pairsWithPrevious) && keepOffColumnHead(idx)) {
      return;
    }

    if (rowsLeft() == 1 && Kinsoku::isLineEndProhibited(pc.codepoint)) {
      // Oidashi (追い出し): push this character forward into a fresh column instead of letting it
      // end the current one.
      nextColumn();
    }

    placeUpright(pc);
    const uint16_t placedRow = row;
    advanceRows(1);
    // Applied once per ellipsis GROUP (…… is two codepoints): the dots keep their own spacing,
    // only what follows the group moves.
    const bool endsEllipsisGroup = (pc.codepoint == 0x2026 || pc.codepoint == 0x2025) &&
                                   (idx + 1 >= o.stream_.size() ||
                                    (o.stream_[idx + 1].codepoint != 0x2026 && o.stream_[idx + 1].codepoint != 0x2025));
    if (endsEllipsisGroup && rowsLeft() > 0) {
      clearEllipsisDots(pc, geom.rowTopPx(placedRow), idx + 1, pc.paragraphIndex);
    }
    if (rowsLeft() <= 0) {
      column++;
      if (pageFull()) {
        size_t nextIdx = idx + 1;
        if (absorbIntoClosingColumn(nextIdx)) idx = nextIdx - 1;  // the idx++ below steps past it
      }
      setRow(0);
      finalizePageIfNeeded();
      setRow(columnStartRow(false));
    }
    idx++;
  }
};

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
  // A carry with no following batch (the sink finalizes without adding more text) would be lost.
  // The stream is empty here, so there are no recorded break indices to shift.
  if (!carriedRunTail_.empty() && stream_.empty()) {
    for (auto& carried : carriedRunTail_) {
      if (!canPushStreamChar()) break;
      pushCarried(carried);
    }
    carriedRunTail_.clear();
    carriedRubyPool_.clear();
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
  // The character-spacing setting widens or narrows the step down the column, as it does the
  // step along a horizontal line. Never below half an em, whatever a caller passes.
  const int rowPitchPx = std::max((cellPx + 1) / 2, cellPx + characterSpacingPx_);
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
  // N rows occupy N*pitch - spacing: the last one needs no trailing gap.
  const uint16_t rowsPerColumn =
      static_cast<uint16_t>(std::clamp((static_cast<int>(viewportHeight_) + rowPitchPx - cellPx) / rowPitchPx, 1,
                                       static_cast<int>(VerticalGlyph::MAX_GRID_CELLS)));
  const int usableWidthPx = std::max(cellPx, static_cast<int>(viewportWidth_) - rightPaddingPx_);
  // N columns occupy N*advance - gap, not N*advance: the last one needs no trailing 行間, so
  // dividing the width by the advance drops a column whenever the remainder is a cell or more.
  const uint16_t columnsPerPage = static_cast<uint16_t>(
      std::clamp((usableWidthPx + columnGapPx_) / columnAdvancePx, 1, static_cast<int>(VerticalGlyph::MAX_GRID_CELLS)));
  // The width that does not divide evenly into whole columns is left where it falls: on the LEFT,
  // because tategaki starts at the right margin and marches leftwards. It must not be spread into
  // the gaps -- 行間 is set in quarter ems by the line-spacing setting (and by whether furigana
  // needs room in it), so widening it here overrides the rule that chose it; measurably, a 21px
  // gap became 25px against a 29px em. Nor split between the margins: that pushes the first
  // column off the right edge the text is supposed to start at.

  // One bundle for the helpers that were lambdas purely to reach these values.
  const ColumnGeometry geom{.cellPx = cellPx,
                            .rowPitchPx = rowPitchPx,
                            .inkGapPx = inkGapPx,
                            .baselineInCellPx = baselineInCellPx,
                            .ascenderPx = ascender,
                            .columnAdvancePx = columnAdvancePx,
                            .usableWidthPx = usableWidthPx,
                            .rowsPerColumn = rowsPerColumn,
                            .columnsPerPage = columnsPerPage};
  InkMemo inkMemo;

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
  boxGeomRowPitchPx_ = rowPitchPx;
  boxGeomColumnAdvancePx_ = columnAdvancePx;
  boxGeomUsableWidthPx_ = usableWidthPx;
  boxFootReservePx_ = boxPadPx();
  boxGeomRowsInBox_ = static_cast<uint16_t>(
      std::max(1, (static_cast<int>(viewportHeight_) - boxFootReservePx_ + geom.rowGapPx()) / rowPitchPx));
  boxGeomRowsFull_ =
      static_cast<uint16_t>(std::max(1, (static_cast<int>(viewportHeight_) + geom.rowGapPx()) / rowPitchPx));

  // Re-record box markers carried across a batch boundary (see reset()) at index 0.
  if (boxEndCarry_) {
    boxEndsBeforeIndex_.insert(boxEndsBeforeIndex_.begin(), 0);
    boxEndCarry_ = false;
  }
  if (boxStartCarry_) {
    boxStartsBeforeIndex_.insert(boxStartsBeforeIndex_.begin(), 0);
    boxStartCarry_ = false;
  }

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
      LOG_DBG("VPT", "Skipping pages reserve (%u bytes doesn't fit, free=%u); growing incrementally",
              static_cast<unsigned>(requestBytes), ESP.getMaxAllocHeap());
    }
  }

  // Every reserve() below this point is a single contiguous allocation that aborts the whole
  // process on failure under -fno-exceptions -- confirmed via a real device crash inside this
  // exact reserve, immediately after the *previous* page was pushed (which can itself burst
  // memory use during its own relocation). Check the request against free heap every time.

  // Guards one push_back when the bulk reserve was skipped. Skipping the reserve is only safe if
  // each push is guarded too: incremental growth still reallocates, and that reallocation aborts
  // under -fno-exceptions. Dropping one glyph is preferable to killing the firmware.
  //
  // Three rules, each load-bearing:
  //
  //   1. Check only when a reallocation is imminent (size == capacity). Otherwise free.
  //
  //   2. Test the ACTUAL next allocation size, never a flat margin. Growth roughly doubles, so a
  //      push from empty needs ~50 bytes while a push near a full page needs nearly the whole bulk
  //      reserve. A flat 32KB margin drops every glyph whenever free heap is below 32KB -- easily
  //      survivable for a 50-byte push -- and silently blanks whole pages.
  //
  //   3. Fall back to LINEAR growth once doubling would be too large. With x2 growth a single
  //      failed doubling leaves capacity unchanged, so every later push requests the same large
  //      block and fails identically, dropping the rest of the page after one transient dip. A
  //      small constant step keeps each retry cheap enough to succeed once memory frees up.
  // An embedded Latin run reserves whole cells, leaving up to a cell of dead space after it.
  // The whole remainder of the column slides up by that amount, not just the next character --
  // moving one character only relocates the gap. Spacing stays even and this column's tail sits
  // slightly higher than its neighbours'.
  // Carried across batches with the row: a resumed batch continues the same column.
  int& columnYShift = pendingColumnYShift_;
  uint16_t& shiftColumn = pendingShiftColumn_;

  LayoutCursor cur{*this,          geom,          inkMemo,      pendingPage_,        pages,
                   pendingColumn_, pendingRow_,   pendingRowY_, pendingLastCellTop_, columnYShift,
                   shiftColumn,    glyphsPerPage, ctx,          onPageReady};

  // First call ever (or first since the last isFinalFlush=true call): start a fresh page. A
  // resumed call (pendingPageValid_ already true) picks up exactly where the previous non-final
  // call left off -- same page object, same column/row -- so a batch boundary never truncates a
  // page that isn't actually full.
  if (!pendingPageValid_) {
    pendingPage_ = VerticalPage{};
    pendingPage_.columnCount = columnsPerPage;
    pendingPage_.rowsPerColumn = rowsPerColumn;
    cur.reservePageGlyphs(pendingPage_);
    // The second of the two page buffers that circulate (see recycledPage_), taken now while the
    // heap is at its chapter-start best rather than at the first page break.
    cur.reservePageGlyphs(recycledPage_);
    pendingColumn_ = 0;
    pendingRow_ = 0;
    pendingRowY_ = 0;
    pendingLastCellTop_ = 0;
    pendingColumnYShift_ = 0;
    pendingShiftColumn_ = UINT16_MAX;
    pendingPageValid_ = true;
  }
  VerticalPage& page = pendingPage_;
  uint16_t& column = pendingColumn_;
  uint16_t& row = pendingRow_;

  cur.useScale(8);
  cur.isFinalFlush = isFinalFlush;
  cur.nextParagraphBreakIdx = nextParagraphBreakIdx;
  size_t idx = 0;
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
      cur.suppressKinsokuUntilIdx = highWaterIdx + 1;
    }
    // Emergency page split: the page's glyph vector is effectively full and the heap has no
    // block for the grow-copy. Close the page at this character boundary and continue on a
    // fresh one (whose vector starts small and grows in fragment-sized steps) -- an early
    // page break the reader barely notices, instead of the silent character loss that
    // followed once pushGlyph's growth ladder was exhausted mid-page.
    if (!cur.pageFull() && !page.glyphs.empty() && !cur.pageVectorCanTakeMore()) {
      LOG_INF("VPT", "Page glyph buffer cannot grow (%u glyphs, maxAlloc=%u); early page break",
              static_cast<unsigned>(page.glyphs.size()), ESP.getMaxAllocHeap());
      everSplitForHeap_ = true;
      cur.forcePageBreak = true;
      cur.finalizePageIfNeeded();
      cur.setRow(cur.columnStartRow(false));
    }

    // Stamp the page with its first character's position. "No glyphs yet" stands in for "nothing
    // placed yet", so a page continuing across a batch boundary keeps the stamp it opened with.
    // A page opened by a break later in this iteration is seeded from lastCharOffset_ instead --
    // see finalizePageIfNeeded().
    lastCharOffset_ = stream_[idx].visibleTextOffset;
    if (page.glyphs.empty()) page.visibleTextOffset = stream_[idx].visibleTextOffset;

    // Each step below is entered only when its case applies: almost every character is plain text,
    // and the checks cost less than the calls they guard.
    const PendingChar& pc = stream_[idx];
    if (pc.scale8 != cur.scale8 && !(pc.scale8 == 0 && cur.scale8 == 8)) cur.useScale(pc.scale8);
    if (cur.geom.cellPx > geom.cellPx) cur.makeRoomForWidth();
    if (cur.nextBoxEndIdx < boxEndsBeforeIndex_.size() && idx == boxEndsBeforeIndex_[cur.nextBoxEndIdx]) {
      cur.closeBlocksAt(idx);
    }
    bool paragraphJustStarted = false;
    if (cur.nextParagraphBreakIdx < paragraphBreaksBeforeIndex_.size() &&
        idx == paragraphBreaksBeforeIndex_[cur.nextParagraphBreakIdx]) {
      paragraphJustStarted = cur.startParagraphsAt(idx);
    }
    if (cur.nextBoxStartIdx < boxStartsBeforeIndex_.size() && idx == boxStartsBeforeIndex_[cur.nextBoxStartIdx]) {
      cur.openBlocksAt(idx);
    }

    const size_t limit = cur.boundaryLimit();
    const uint32_t cp = pc.codepoint;
    constexpr uint8_t CONT = EpdFontFamily::RUBY_CONTINUE;
    const bool rubyHead =
        (pc.style & CONT) == 0 && (pc.rubyLen > 0 || (idx + 1 < limit && (stream_[idx + 1].style & CONT) != 0));
    if ((isImageMarker(cp) && cur.placeInlineImage(idx)) || (isBangOrQuestion(cp) && cur.placeBangPair(idx, limit)) ||
        (isAsciiDigit(cp) && cur.placeNumber(idx, limit)) ||
        ((isAsciiDigit(cp) || Kinsoku::isRotatedRunCharacter(cp)) && cur.placeRotatedRun(idx, limit)) ||
        (rubyHead && cur.placeRubyGroup(idx, limit))) {
      continue;
    }
    cur.placeCharacter(idx, paragraphJustStarted);
  }

  if (isFinalFlush) {
    // A block still open here ends with the chapter: its end marker sits at the stream's end, which the
    // loop never visits. Close it as finalizePendingPage() does, or it loses its rect and alignment.
    if (inBox_) {
      const uint16_t lastCol = lastBlockColumn(page, boxStartCol_);
      appendBoxRectToPage(page, boxStartCol_, lastCol, /*openLeft=*/false, /*openRight=*/boxContinuedFromPrevPage_);
      if (activeBlock_.aligns()) alignBlockColumns(page, boxStartCol_, lastCol);
      inBox_ = false;
      boxContinuedFromPrevPage_ = false;
    }
    cur.spreadPageToFoot(page, static_cast<uint16_t>(column + 1));
    resolveColumnX(page);
    clearColumnWidths();
    oidashiColumns_ = 0;
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
// column in tategaki order) and append it to the page. Inset 0.25em from the text on all four
// sides; at the foot the box takes whatever is left down to the text area's bottom edge.
void VerticalParsedText::appendBoxRectToPage(VerticalPage& p, const uint16_t startCol, const uint16_t endCol,
                                             const bool openLeft, const bool openRight) const {
  if (boxGeomCellPx_ <= 0 || activeBlock_.borderEdges == 0) return;
  // A uniform 0.25em of air between the text and the rule on all four sides. At least 1px, so
  // the rule never lands on the glyphs at small font sizes.
  const int pad = boxPadPx();
  const int padX = pad;
  const int padTop = pad;
  // The foot lands on the text area's bottom edge, taking whatever the boxed column left below
  // its last character -- never into the margin band beyond it. boxFootReservePx_ (see
  // rowsAvailable) is what guarantees that leftover is at least the inset.
  // Extending the rule through the margin band to the status bar was tried and looked worse.
  const int textBottom =
      static_cast<int>(boxGeomRowsInBox_) * boxGeomRowPitchPx_ - (boxGeomRowPitchPx_ - boxGeomCellPx_);
  const int padBottom = std::max(pad, static_cast<int>(viewportHeight_) - textBottom);
  const int right = columnLeftPx(pendingColW_, startCol) + columnWidthPx(pendingColW_, startCol) + padX;
  const int left = columnLeftPx(pendingColW_, endCol) - padX;
  VerticalBoxRect r;
  r.x = static_cast<int16_t>(left);
  r.y = static_cast<int16_t>(-padTop);
  r.w = static_cast<int16_t>(right - left);
  r.h = static_cast<int16_t>(textBottom + padTop + padBottom);
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

// Place the glyphs of columns [startCol..endCol] along the column height once their block closes on a
// page: text-align center / end moves each column on its own; auto head/foot margins move the block as
// a whole, so its columns keep starting together (a title page centred as one block). Text only moves
// down, by whole rows (rows stay true for selection), into the space below its own last glyph.
void VerticalParsedText::alignBlockColumns(VerticalPage& p, const uint16_t startCol, const uint16_t endCol) const {
  // A framed block's columns stop short of the foot for its bottom rule: align within THAT height. A
  // block without a frame aligns to the real foot, like the text around it.
  if (boxGeomCellPx_ <= 0 || boxGeomRowsInBox_ == 0 || boxGeomRowPitchPx_ <= 0) return;
  const int rows = activeBlock_.borderEdges != 0 ? boxGeomRowsInBox_ : boxGeomRowsFull_;
  const int footPx = (rows - 1) * boxGeomRowPitchPx_ + boxGeomCellPx_;
  // Lowest ink edge of the columns, -1 when empty or holding an image (which is never moved). A
  // sideways Latin word or number is one glyph at its first row but runs down for its whole length.
  const auto bottomOf = [&](const uint16_t from, const uint16_t to) {
    int bottom = -1;
    for (const auto& g : p.glyphs) {
      if (g.column < from || g.column > to) continue;
      if (isImageMarker(g.codepoint)) return -1;
      const int scale8 = g.scale8 == 0 ? 8 : g.scale8;
      const auto atScale = [scale8](const int v) {
        return scale8 == 8 ? v : static_cast<int>(std::lround(v * scale8 / 8.0));
      };
      int height = atScale(boxGeomCellPx_);
      if (g.renderKind == VerticalGlyph::RotatedRun && g.textId != VerticalGlyph::NO_TEXT) {
        height =
            atScale(renderer_.getRenderAdvanceX(fontId_, p.glyphText(g), static_cast<EpdFontFamily::Style>(g.style)));
      }
      bottom = std::max(bottom, static_cast<int>(g.y) + height);
    }
    return bottom;
  };
  const auto shift = [&](const uint16_t from, const uint16_t to, const bool toFoot) {
    const int bottom = bottomOf(from, to);
    if (bottom < 0) return;
    // A column whose last mark hangs below the foot (burasage) has no free row.
    const int freeRows = std::max(0, footPx - bottom) / boxGeomRowPitchPx_;
    const int rowsDown = toFoot ? freeRows : freeRows / 2;
    if (rowsDown == 0) return;
    for (auto& g : p.glyphs) {
      if (g.column < from || g.column > to) continue;
      g.y = static_cast<uint16_t>(g.y + rowsDown * boxGeomRowPitchPx_);
      g.row = static_cast<uint8_t>(std::min(255, g.row + rowsDown));
    }
  };
  if (activeBlock_.blockAlign != VerticalBlockParams::BlockAlign::None) {
    shift(startCol, endCol, activeBlock_.blockAlign == VerticalBlockParams::BlockAlign::End);
    return;
  }
  for (uint16_t c = startCol; c <= endCol; c++) shift(c, c, activeBlock_.alignEnd);
}

// The leftmost column [startCol..] that holds a glyph on page p (an image's glyph sits on the last column it
// covers), or startCol when none does. A block's closing frame ends there, not at a cursor already moved on.
uint16_t VerticalParsedText::lastBlockColumn(const VerticalPage& p, const uint16_t startCol) {
  uint16_t last = startCol;
  for (const auto& g : p.glyphs) {
    if (g.column >= startCol) last = std::max<uint16_t>(last, g.column);
  }
  return last;
}

bool VerticalParsedText::finalizePendingPage(VerticalPage& out) {
  if (!pendingPageValid_) return false;
  pendingPageValid_ = false;  // next layoutPages() call starts a fresh page either way
  if (pendingPage_.glyphs.empty()) return false;
  // Chapter ended while inside a box (end marker carried past the last batch, or a truncated
  // chapter): close the rect on this final page so the border isn't silently dropped.
  if (inBox_) {
    const uint16_t lastCol = lastBlockColumn(pendingPage_, boxStartCol_);
    appendBoxRectToPage(pendingPage_, boxStartCol_, lastCol, /*openLeft=*/false,
                        /*openRight=*/boxContinuedFromPrevPage_);
    if (activeBlock_.aligns()) alignBlockColumns(pendingPage_, boxStartCol_, lastCol);
    inBox_ = false;
    boxContinuedFromPrevPage_ = false;
  }
  resolveColumnX(pendingPage_);
  clearColumnWidths();
  out = std::move(pendingPage_);
  anyPageEverProduced_ = true;  // set, NOT reset -- see the header doc comment
  return true;
}

int VerticalParsedText::columnWidthPx(const uint16_t (&widths)[MAX_PAGE_COLUMNS], const uint16_t col) const {
  return col < MAX_PAGE_COLUMNS && widths[col] != 0 ? widths[col] : boxGeomCellPx_;
}

int VerticalParsedText::columnLeftPx(const uint16_t (&widths)[MAX_PAGE_COLUMNS], const uint16_t col) const {
  const int gap = boxGeomColumnAdvancePx_ - boxGeomCellPx_;
  int right = boxGeomUsableWidthPx_;
  for (uint16_t c = 0; c < col; c++) right -= columnWidthPx(widths, c) + gap;
  return right - columnWidthPx(widths, col);
}

int VerticalParsedText::columnXDelta(const uint16_t (&widths)[MAX_PAGE_COLUMNS], const VerticalGlyph& g) const {
  const int width = columnWidthPx(widths, g.column);
  const int provisionalLeft =
      boxGeomUsableWidthPx_ - boxGeomCellPx_ - static_cast<int>(g.column) * boxGeomColumnAdvancePx_;
  // An image keeps its own width; text is centred on the column's axis in its own cell.
  const int scale8 = g.scale8 == 0 ? 8 : g.scale8;
  const int cell = isImageMarker(g.codepoint) || scale8 == 8
                       ? boxGeomCellPx_
                       : static_cast<int>(std::lround(boxGeomCellPx_ * scale8 / 8.0));
  return columnLeftPx(widths, g.column) + (width - cell) / 2 - provisionalLeft;
}

void VerticalParsedText::clearColumnWidths() {
  std::fill(std::begin(pendingColW_), std::end(pendingColW_), 0);
  pendingColWidened_ = false;
}

void VerticalParsedText::resolveColumnX(VerticalPage& p) const {
  // Every column one body em wide: the glyphs are already where they belong.
  if (!pendingColWidened_) return;
  for (auto& g : p.glyphs) g.x = static_cast<uint16_t>(std::max(0, g.x + columnXDelta(pendingColW_, g)));
}
