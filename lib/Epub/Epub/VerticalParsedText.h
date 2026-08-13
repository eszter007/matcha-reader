#pragma once

#include <cstdint>
#include <string>
#include <vector>

class GfxRenderer;

// --- Shared cell metrics -------------------------------------------------------------------
// Layout and drawing MUST agree on every one of these -- the layout paginates with them, the
// draw positions ink with them -- or the mismatch accumulates down a column and the last row
// lands on the status bar. They live here, not in either caller, so there is one definition.

// A glyph's ink box relative to its origin, as GfxRenderer::getGlyphMetrics reports it: `top` is
// the ink top ABOVE the baseline, `left` the left side bearing. Bundled because vertical layout
// probes glyph ink constantly and four out-params per call buried the geometry in boilerplate.
struct GlyphInk {
  int left = 0;
  int width = 0;
  int top = 0;
  int height = 0;
};
// False when the font has no such glyph (or it is blank), in which case `out` is untouched --
// every caller has a metrics-free fallback, since an SD font may not be resident yet.
bool measureGlyphInk(const GfxRenderer& renderer, int fontId, uint32_t cp, uint8_t style, GlyphInk* out);

// The kihon-hanmen cell: one em, measured as the advance of the reference full-width glyph 漢
// (advanceY would include interline spacing on Latin-oriented fonts). JLREQ sets solid, so the
// cell IS the em -- inter-character air comes from the line-gap setting, never from padding here.
// Falls back to the line height when the font has no such advance.
// `measured` (optional) reports whether the font answered: a caller that caches the result must
// only cache a measured one, or a probe against a not-yet-resident SD font freezes its fallback in.
int verticalCellPx(const GfxRenderer& renderer, int fontId, bool* measured = nullptr);

// The gap a grid-adjacent pair of full-width characters leaves between their ink boxes, measured
// as the reference glyph's unused cell height. Used wherever something off-grid (a rotated Latin
// run, a bracket, an ellipsis) must clear the character after it: matching this keeps that
// spacing indistinguishable from the surrounding text instead of inventing a fraction of a cell.
int verticalNominalInkGapPx(const GfxRenderer& renderer, int fontId, int cellPx, bool* measured = nullptr);

// Where a glyph's baseline sits inside its cell, measured down from the cell's top: the offset
// that centres the reference CJK glyph's ink box (中 -- full-width, even bearings, the same
// reference the tate-chu-yoko centring uses). Falls back to the font ascender when metrics are
// unavailable, e.g. an SD font not yet resident.
int verticalCellBaselineOffset(const GfxRenderer& renderer, int fontId, int cellPx, bool* measured = nullptr);

// A single positioned glyph cell within a vertically-laid-out page.
// `paragraphIndex` + `byteOffset` identify exactly where this character
// came from in the source text -- this is the hook point for phase 2
// (tap-to-select word lookup against jisho.org): given a tap at logical
// (x, y), find the nearest VerticalGlyph, then walk byteOffset backwards/
// forwards to find word boundaries before firing off a lookup.
struct VerticalGlyph {
  // How this glyph is drawn in vertical layout:
  //   Upright          - normal CJK/kana, drawn at the baseline anchor (x,y).
  //   RotatedRun       - sideways Latin/number run rotated 90° CCW; x,y is the
  //                      rotation baseline anchor; rotatedRunText holds text.
  //   UprightRun       - short upright inline run (e.g. 2-digit tate-chu-yoko)
  //                      drawn unrotated; x,y is the baseline anchor.
  //   RotatedPunct     - a single bracket/dash rotated 90° CCW and aligned in
  //                      its cell using glyph metrics; x,y is the cell top-left
  //                      and codepoint holds the character.
  //   SmallKanaCorner  - small (yoon/sokuon) kana placed toward the cell's
  //                      top-right corner; x,y is the cell's top-left and
  //                      codepoint holds the character.
  enum RenderKind : uint8_t { Upright = 0, RotatedRun = 1, RotatedPunct = 2, SmallKanaCorner = 3, UprightRun = 4 };

  uint32_t codepoint = 0;
  uint16_t column = 0;  // 0 = rightmost column on the page
  uint16_t row = 0;     // 0 = topmost cell in the column
  uint16_t x = 0;       // logical screen-space draw position
  uint16_t y = 0;       // logical screen-space draw position
  uint32_t paragraphIndex = 0;
  uint32_t byteOffset = 0;  // UTF-8 byte offset into that paragraph's text
  uint8_t renderKind = Upright;
  uint8_t style = 0;      // EpdFontFamily::Style flags (BOLD, ITALIC, etc.)
  bool emphasis = false;  // text-emphasis (sesame dots beside character)
  // Opening bracket starting its column: set flush to the line head (tentsuki), the half em
  // before it deleted. Decided in layout, which knows where columns start (indents included).
  uint8_t lineHeadFlush = 0;
  // Index into the owning VerticalPage's texts pool: the run string for
  // RotatedRun/UprightRun glyphs, the furigana/ruby annotation (UTF-8) for
  // every other kind. NO_TEXT = none. An id instead of inline std::strings
  // keeps the glyph a ~28-byte POD -- two always-present strings cost ~48
  // bytes per glyph even when empty, tripling every page buffer, reserve and
  // grow-copy on the heap-tight X3. The id travels WITH the glyph through
  // oikomi moves and drop paths, so there is no side table to desync.
  static constexpr uint16_t NO_TEXT = 0xFFFF;
  uint16_t textId = NO_TEXT;
};

// One screen's worth of vertically laid out text, ready to hand to
// VerticalTextBlock::render(). Glyph records are fixed-size; the variable-length
// ruby/run strings live in the page's texts pool (length-prefixed on disk, after
// the glyph array -- see VerticalSection's writePage/readPage).
// Pixel-space border rectangle for a boxed (kakomi) block on this page, in the same logical
// coordinate space as VerticalGlyph::x/y (the renderer adds its offsets identically).
struct VerticalBoxRect {
  // `edges` says which border lines to DRAW (a kakomi box has all four; a .k-solid-top
  // separator only TOP) plus extension flags: a box spanning a page boundary renders
  // HALF-OPEN, like print typesetting -- the open side omits its vertical line and runs the
  // horizontal lines through to the screen edge (tategaki flows right-to-left, so "continues
  // to next page" = open LEFT edge, and a continuation page comes in from the RIGHT edge).
  static constexpr uint8_t DRAW_TOP = 1 << 0;
  static constexpr uint8_t DRAW_RIGHT = 1 << 1;
  static constexpr uint8_t DRAW_BOTTOM = 1 << 2;
  static constexpr uint8_t DRAW_LEFT = 1 << 3;
  static constexpr uint8_t EXTEND_LEFT = 1 << 4;   // horizontal lines run to the left screen edge
  static constexpr uint8_t EXTEND_RIGHT = 1 << 5;  // horizontal lines run to the right screen edge
  int16_t x = 0;
  int16_t y = 0;
  int16_t w = 0;
  int16_t h = 0;
  uint8_t edges = 0;
};

// Distilled block-level layout parameters for the vertical engine (from CSS via
// CssParser::collectVerticalStyles; plain floats here to avoid a CssParser include).
struct VerticalBlockParams {
  float startEm = 0;   // every column of the block starts this many cells down
  float beforeEm = 0;  // extra gap before the block's first column
  float afterEm = 0;   // extra gap after the block's last column
  float hangEm = 0;    // hanging indent: wrapped (non-paragraph-start) columns start this much lower
  bool alignCenter = false;
  uint8_t borderEdges = 0;  // CSS physical TOP/RIGHT/BOTTOM/LEFT bits (match DRAW_* order)
};

struct VerticalPage {
  // Codepoint index, within the chapter's visible character data, of this page's first
  // character -- the vertical counterpart of Page::visibleTextOffset, counted by the same rule
  // so the two are directly comparable. This is what makes a vertical/horizontal switch land on
  // the page holding the same sentence rather than on a proportional guess.
  uint32_t visibleTextOffset = 0;
  std::vector<VerticalGlyph> glyphs;
  std::vector<VerticalBoxRect> boxes;
  // Variable-length glyph texts (ruby annotations, rotated/upright run strings), referenced
  // by VerticalGlyph::textId. Pooled per page so the glyph array stays fixed-size POD.
  std::vector<std::string> texts;
  const char* glyphText(const VerticalGlyph& g) const { return g.textId < texts.size() ? texts[g.textId].c_str() : ""; }
  const std::string& glyphTextStr(const VerticalGlyph& g) const {
    static const std::string kEmpty;
    return g.textId < texts.size() ? texts[g.textId] : kEmpty;
  }
  // Appends `s` to the pool and returns its id; NO_TEXT for an empty string or a full pool.
  uint16_t internText(std::string s) {
    if (s.empty() || texts.size() >= VerticalGlyph::NO_TEXT) return VerticalGlyph::NO_TEXT;
    texts.push_back(std::move(s));
    return static_cast<uint16_t>(texts.size() - 1);
  }
  uint16_t columnCount = 0;
  uint16_t rowsPerColumn = 0;
  // If non-empty, this page is an image page — render the image instead of glyphs.
  std::string imagePath;
  // Book-internal href the image was extracted from. Kept so a render can re-extract when
  // imagePath is missing: the build extracts eagerly, and that extraction needs one contiguous
  // 32KB inflate window at the exact moment the layout has the heap at its tightest (measured
  // 11.7KB free / 10.7KB largest block on a dense vertical chapter). Without the href, a failure
  // there was permanent — the layout persisted pointing at a file that no longer existed and
  // nothing could ever recreate it. A render has the heap back (~95KB), so it can simply retry.
  std::string imageSrcPath;
  int16_t imageWidth = 0;
  int16_t imageHeight = 0;
  bool imageRotated = false;  // true = landscape image rotated 90° CW to fill portrait screen
  bool isImagePage() const { return !imagePath.empty(); }
};

// Lays out one or more paragraphs of Japanese (or any CJK) text into
// right-to-left, top-to-bottom columns, following simplified kinsoku shori
// rules (see Kinsoku.h) and batching embedded Latin/number runs into
// sideways-rotated blocks.
//
// Deliberately does NOT attempt word-wrap, hyphenation, or the
// Knuth-Plass-style "badness" minimization that ParsedText uses for
// horizontal Latin script -- none of that applies to CJK text, where the
// unit of layout is the individual character, not the word. This makes the
// vertical engine considerably simpler than ParsedText despite doing a
// conceptually similar job.
//
// v1 scope / known limitations (see docs/vertical-text-design.md):
//   - Operates on plain paragraph text; does not currently consume
//     per-run bold/italic/underline styling or inline images.
//   - Punctuation (、 。 etc.) is shifted toward the upper-right of its cell
//     for tategaki, but the offset is an approximation (half cellPx).
class VerticalParsedText {
 public:
  VerticalParsedText(const GfxRenderer& renderer, int fontId, uint16_t viewportWidth, uint16_t viewportHeight);

  // Adds one paragraph's worth of text (UTF-8). Call once per <p> (or
  // equivalent block) in source order; paragraphIndex is just this call's
  // ordinal position and is what VerticalGlyph::paragraphIndex refers back
  // to, so the caller is responsible for keeping its own paragraph-index
  // -> original-text mapping if it needs to resolve lookups later.
  void addParagraph(const std::string& utf8Text);

  // A single run of base text optionally annotated with ruby (furigana).
  // For <ruby>漢<rt>かん</rt>字<rt>じ</rt></ruby>, this produces two
  // RubyRun entries: {"漢", "かん"} and {"字", "じ"}.
  // Unannotated text has empty ruby.
  struct RubyRun {
    std::string baseText;
    std::string rubyText;
    uint8_t style = 0;
    bool emphasis = false;
    // Codepoint index, within the chapter's visible character data, of this run's FIRST base
    // character. Counted by the extractor over the same rule the horizontal parser uses
    // (VisibleTextUtils::isNonVisibleElement), so the number means the same thing in both
    // layouts -- that shared meaning is the whole point: it is what lets a reading position
    // survive a vertical/horizontal switch. See VerticalPage::visibleTextOffset.
    uint32_t visibleTextOffset = 0;
  };

  // continuesPreviousParagraph: pass true when this call carries the NEXT CHUNK of a paragraph
  // whose earlier chunks were already added (VerticalSection chunks large paragraphs to bound
  // stream_ memory) -- no paragraph break is recorded, so after an intervening flush/reset the
  // text continues seamlessly mid-column. Pass false (default) for a genuinely new paragraph so
  // layoutPages() forces the fresh column even when a batch boundary lands exactly here (the
  // old code recorded a break either way and then unconditionally skipped the batch's first
  // break, silently merging a real paragraph into the previous one's column whenever a flush
  // coincided with a paragraph boundary -- always the case right after an inline image).
  void addAnnotatedParagraph(const std::vector<RubyRun>& runs, bool continuesPreviousParagraph = false);

  // Called for a page as soon as it's confirmed safe to write -- i.e. one page has already been
  // completed after it, so the "oikomi" pull-back check (which only ever looks at the single most
  // recently completed page) can no longer touch it. ctx is caller-supplied context.
  using PageReadyCallback = void (*)(void* ctx, VerticalPage&& page);

  // Runs the column-fill layout algorithm over everything added since the last layoutPages() call
  // (i.e. since construction, or since the most recent call) and returns one VerticalPage per
  // screen's worth of content.
  //
  // When onPageReady is non-null, completed pages are streamed out through it as soon as they're
  // safe to detach (see PageReadyCallback), rather than all being held in the returned vector for
  // the whole call -- confirmed on a real device as a real peak-memory problem: for a large batch
  // (several pages), every page's glyph buffer (13KB+ each) stayed resident simultaneously until
  // the whole function returned, on top of the stream_ buffer still being alive at the same time.
  // With a callback, at most ~2 pages' worth of glyph buffers are ever resident at once.
  //
  // isFinalFlush controls what happens to the trailing, possibly-not-yet-full page (the one still
  // being filled when this call's input runs out):
  //   - true (the default -- correct for a one-shot "lay out this whole chapter" caller, and the
  //     only behavior this function had before batched callers existed): the trailing page is
  //     finalized as-is and included in the return value, exactly as before.
  //   - false: a caller that's batching a long chapter across MULTIPLE layoutPages() calls (to
  //     bound peak memory -- see LayoutPageSink::onParagraph()/flushText() in VerticalSection.cpp)
  //     passes false for every call except the chapter's true last one. The trailing page is then
  //     held internally (NOT returned/streamed) and CONTINUED by the next layoutPages() call
  //     instead of being finalized early. Without this, a batch boundary landing mid-page (which,
  //     with a small enough batch size, is the common case) would finalize a page that's only
  //     partially filled and start a fresh one for the remainder -- confirmed on a real device via
  //     screenshot as pages missing their left-hand columns, i.e. never actually cut short in the
  //     source, just artificially split into two half-empty pages by the batch boundary.
  // The return value only contains page(s) actually finalized by this call (0-2: the trailing page
  // from the PREVIOUS call if it just got completed by this call's input, plus this call's own
  // trailing page if isFinalFlush) -- callers must still write those, same as before.
  std::vector<VerticalPage> layoutPages(void* ctx = nullptr, PageReadyCallback onPageReady = nullptr,
                                        bool isFinalFlush = true);

  // Detach and return the in-progress page (the one held across isFinalFlush=false calls) so a
  // caller can splice a standalone page -- an image -- into the page sequence in document order.
  // Returns false (and emits nothing) when there is no pending page or it has no glyphs, so an
  // image at a chapter/batch start never produces a spurious blank page. Deliberately NOT the
  // same as a layoutPages(isFinalFlush=true) call: that path also resets anyPageEverProduced_
  // (end-of-chapter bookkeeping), which mid-chapter would let a later genuinely-final flush emit
  // a stray blank page in image-heavy chapters. Call only after the pending stream is laid out
  // (i.e. right after a layoutPages() call / when pendingCount() == 0).
  bool finalizePendingPage(VerticalPage& out);

  // Column-to-column gap in pixels, added on top of the character cell
  // size when advancing to a new column. Mirrors the role
  // SETTINGS.lineCompression plays for horizontal text; exposed as a
  // setter so EpubReaderActivity can wire it to a reader setting instead
  // of a hardcoded constant.
  // Also clears oom_: that flag means "the reallocation attempted right then didn't fit", not
  // "this chapter is unbuildable" -- latching it across batches would silently truncate every
  // batch after the first transient low-memory moment, for the rest of the chapter.
  void reset() {
    // Carry box markers sitting exactly at the batch boundary into the next batch: they were
    // recorded for content that hasn't been added yet, so the layout loop (idx < stream size)
    // never visited them. layoutPages() re-records a carried marker at index 0.
    boxStartCarry_ =
        boxStartCarry_ || (!boxStartsBeforeIndex_.empty() && boxStartsBeforeIndex_.back() == stream_.size());
    boxEndCarry_ = boxEndCarry_ || (!boxEndsBeforeIndex_.empty() && boxEndsBeforeIndex_.back() == stream_.size());
    stream_.clear();
    paragraphBreaksBeforeIndex_.clear();
    boxStartsBeforeIndex_.clear();
    boxEndsBeforeIndex_.clear();
    // blockParamsQueue_ is NOT cleared: entries are consumed by the layout loop in marker
    // order, and a carried start marker still owns its queued params.
    oom_ = false;
  }
  // Styled-block boundaries, called by the extraction sink in stream order. A styled block wraps
  // whole columns: the layout forces a column break at each boundary, applies the params to its
  // columns (start offset, hanging indent, centering), and records a border rect per page when
  // borderEdges is set; a block spanning pages gets one rect on each page.
  void markBlockStart(const VerticalBlockParams& params) {
    boxStartsBeforeIndex_.push_back(stream_.size());
    blockParamsQueue_.push_back(params);
  }
  void markBlockEnd() { boxEndsBeforeIndex_.push_back(stream_.size()); }
  // Whether ANY char/glyph was dropped for lack of heap across the whole build. Unlike oom_
  // this is never cleared by reset(): the section build reads it at the end, because a build
  // that dropped content produced sparse pages and must not be persisted as a VALID cache --
  // that makes the truncation permanent. Fresh object per build, so no explicit clear needed.
  bool everDroppedForHeap() const { return everDroppedForHeap_; }
  // True when any page was closed early by the emergency split (glyph vector could not grow on
  // a heap dip). No content is lost, but the pagination is degraded -- pages end at arbitrary
  // fill levels -- so the build path treats it like a drop: usable now, rebuilt next open.
  bool everSplitForHeap() const { return everSplitForHeap_; }
  // Pin stream_'s backing store once at build start, while the heap is freshest. Mid-build
  // growth (alloc-copy-free every few dozen entries) interleaved with ruby-string churn walks
  // the buffer through the heap and shreds the largest contiguous block -- observed on a real
  // device collapsing maxAlloc 59K -> 4K over one furigana-dense chapter until an unrelated
  // allocation aborted. With the batch cadence (BATCH_CHARS=160) and run slicing
  // (RUN_SLICE_CHARS=170, exact char counting) 512 entries are never exceeded, so after this
  // call stream_ never reallocates for the whole build. clear() in reset() keeps capacity.
  void preallocateStream();
  // Number of characters currently buffered for layout. Callers batching a long chapter use this
  // to flush layoutPages()+reset() periodically so stream_ stays O(batch) instead of O(chapter)
  // -- a whole chapter's worth of PendingChars (32 bytes each) cannot fit in RAM on-device.
  size_t pendingCount() const { return stream_.size(); }
  void setColumnGapPx(int gapPx) { columnGapPx_ = gapPx; }
  // Extra right-side padding (in pixels) reserved for vertical ruby so it
  // doesn't clip against the right edge.
  void setRightPaddingPx(int padPx) { rightPaddingPx_ = (padPx < 0) ? 0 : padPx; }

 private:
  const GfxRenderer& renderer_;
  int fontId_;
  uint16_t viewportWidth_;
  uint16_t viewportHeight_;
  int columnGapPx_ = 0;
  int rightPaddingPx_ = 0;

  struct PendingChar {
    uint32_t codepoint;
    uint32_t paragraphIndex;
    uint32_t byteOffset;
    uint8_t style;
    bool emphasis;
    std::string rubyText;
    // See RubyRun::visibleTextOffset. Carried per character so the page that a character
    // opens can be stamped with it; NOT carried on VerticalGlyph, which is deliberately a
    // fixed-size POD (4 bytes x ~500 glyphs/page is a page buffer this device cannot spare).
    uint32_t visibleTextOffset = 0;
  };

  // Flattened, paragraph-tagged codepoint stream built up by addParagraph()
  // and consumed by layoutPages(). Paragraph boundaries are recorded as a
  // forced column break (a new paragraph always starts at the top of a
  // fresh column, matching how horizontal layout starts a new line).
  std::vector<PendingChar> stream_;
  std::vector<size_t> paragraphBreaksBeforeIndex_;

  // Set once free heap drops critically low; remaining characters/paragraphs for this chapter
  // are silently dropped instead of risking an OOM abort inside stream_'s reallocation (see
  // canPushStreamChar()).
  bool oom_ = false;
  bool everDroppedForHeap_ = false;  // see everDroppedForHeap()
  bool everSplitForHeap_ = false;    // see everSplitForHeap()

  // Boxed-block state. inBox_/boxStartCol_ persist across batches (a box can span many flushes);
  // the marker vectors are per-batch (cleared in reset(), with boundary carry flags above).
  std::vector<size_t> boxStartsBeforeIndex_;
  std::vector<size_t> boxEndsBeforeIndex_;
  bool boxStartCarry_ = false;
  bool boxEndCarry_ = false;
  bool inBox_ = false;
  uint16_t boxStartCol_ = 0;
  std::vector<VerticalBlockParams> blockParamsQueue_;  // FIFO, one entry per markBlockStart
  VerticalBlockParams activeBlock_;                    // valid while inBox_
  bool blockContinuationColumn_ = false;               // current column wraps mid-paragraph
  // Geometry snapshot from the last layoutPages() call so finalizePendingPage() (called outside
  // layoutPages) can still close an open box rect on the final page.
  int boxGeomCellPx_ = 0;
  int boxGeomColumnAdvancePx_ = 0;
  int boxGeomUsableWidthPx_ = 0;
  // Rows a column INSIDE a box may use: short of a full column by boxFootReservePx_, so the
  // bottom rule has its inset below the last character without crossing the text area's foot.
  uint16_t boxGeomRowsInBox_ = 0;
  int boxFootReservePx_ = 0;
  // 0.25em: the inset between the text and the rule, the same on all four sides (1px floor).
  // Open-coded rather than std::max so this header needs no <algorithm>.
  [[nodiscard]] int boxPadPx() const { return boxGeomCellPx_ / 4 > 1 ? boxGeomCellPx_ / 4 : 1; }
  bool boxContinuedFromPrevPage_ = false;
  void appendBoxRectToPage(VerticalPage& p, uint16_t startCol, uint16_t endCol, bool openLeft, bool openRight) const;
  void centerBlockColumns(VerticalPage& p, uint16_t startCol, uint16_t endCol) const;

  // The page currently being filled by layoutPages(), plus its fill position -- persists ACROSS
  // layoutPages() calls (when isFinalFlush=false) so a batch boundary landing mid-page continues
  // the same page on the next call instead of finalizing it early and starting a fresh one. See
  // layoutPages()'s isFinalFlush doc comment for the full rationale.
  VerticalPage pendingPage_;
  // A paragraph break recorded at exactly the end of a batch's stream (trailing newline) --
  // carried across reset() and re-recorded at the start of the next batch. See layoutPages().
  // A rotated Latin run that reaches the end of a non-final batch is NOT placed: the layout
  // gathers a run only within one batch, so placing it would render "authority" as "au", a blank
  // cell, then "thority". Its characters are held here and prepended to the next batch, where the
  // rest of the word joins them. Survives reset(), which clears the stream itself.
  std::vector<PendingChar> carriedRunTail_;
  bool pendingTrailingBreak_ = false;
  uint16_t pendingColumn_ = 0;
  uint16_t pendingRow_ = 0;
  bool pendingPageValid_ = false;  // false until the first layoutPages() call initializes pendingPage_
  // True once any page (streamed via callback or returned) has been produced across the whole
  // chapter, i.e. across every layoutPages() call since construction. Used only at the final flush
  // to decide whether an otherwise-empty trailing page still needs to be emitted so a genuinely
  // empty chapter gets one (possibly blank) page rather than zero -- checking the local `pages`
  // vector alone isn't enough once pages can be streamed out mid-call and earlier batches may
  // already have produced real pages.
  bool anyPageEverProduced_ = false;
  // Source position of the character being placed. finalizePageIfNeeded() seeds a page opened
  // mid-character from it, which the empty-page stamp cannot reach. See
  // VerticalPage::visibleTextOffset.
  uint32_t lastCharOffset_ = 0;

  // Call before every stream_.push_back(). Only checks free heap when the vector is actually
  // about to reallocate (size == capacity) -- cheap in the common case where capacity headroom
  // from reserve() already covers this push. Returns false (and latches oom_) if a reallocation
  // would be needed and heap is too tight to risk it; the caller should skip this element.
  bool canPushStreamChar();

  // Mutable page-building state for one layoutPages() pass, with the placement rules that act on it.
  // Defined in the .cpp: it exists to give those rules a named home and an explicit set of state.
  // Nested so it can reach this class's private stream/box state directly.
  struct LayoutCursor;

  void reserveStreamFor(size_t utf8Bytes);

  // Font metrics for this object's fontId, measured once and reused for every paragraph of the chapter.
  // Each probe pages its reference glyph (漢/中) in from the SD font, whose on-demand slot table is
  // small, so re-measuring per paragraph evicts real text glyphs -- it accounted for ~27% of all SD
  // glyph loads during a chapter build.
  //
  // Cache only a SUCCESSFUL measurement: a probe against a font that is not resident yet returns a
  // fallback, and storing that freezes the fallback in for the whole chapter.
  int cellPxMemo_ = 0;
  int inkGapPxMemo_ = -1;
  int baselineInCellMemo_ = -1;

  void recordParagraphBreakAt(size_t idx);
};
