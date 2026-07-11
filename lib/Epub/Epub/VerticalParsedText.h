#pragma once

#include <cstdint>
#include <string>
#include <vector>

class GfxRenderer;

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
  uint16_t column = 0;      // 0 = rightmost column on the page
  uint16_t row = 0;          // 0 = topmost cell in the column
  uint16_t x = 0;            // logical screen-space draw position
  uint16_t y = 0;            // logical screen-space draw position
  uint32_t paragraphIndex = 0;
  uint32_t byteOffset = 0;   // UTF-8 byte offset into that paragraph's text
  uint8_t renderKind = Upright;
  uint8_t style = 0;           // EpdFontFamily::Style flags (BOLD, ITALIC, etc.)
  bool emphasis = false;       // text-emphasis (sesame dots beside character)
  // Populated for run render kinds (RotatedRun/UprightRun).
  std::string rotatedRunText;
  // Furigana/ruby annotation for this glyph (UTF-8). Rendered in a smaller
  // font to the right of the base character in vertical layout.
  std::string rubyText;
};

// One screen's worth of vertically laid out text, ready to hand to
// VerticalTextBlock::render(). Most fields are fixed-size and serialize
// trivially; the only variable-length piece is VerticalGlyph::rotatedRunText
// on entries where rotated == true, which needs a length-prefixed write/read
// (see docs/vertical-text-design.md for the proposed vsections/*.bin layout)
// rather than a flat memcpy of the vector.
struct VerticalPage {
  std::vector<VerticalGlyph> glyphs;
  uint16_t columnCount = 0;
  uint16_t rowsPerColumn = 0;
  // If non-empty, this page is an image page — render the image instead of glyphs.
  std::string imagePath;
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
    stream_.clear();
    paragraphBreaksBeforeIndex_.clear();
    oom_ = false;
  }
  // Whether ANY char/glyph was dropped for lack of heap across the whole build. Unlike oom_
  // this is never cleared by reset(): the section build reads it at the end, because a build
  // that dropped content produced sparse pages and must not be persisted as a VALID cache --
  // that makes the truncation permanent. Fresh object per build, so no explicit clear needed.
  bool everDroppedForHeap() const { return everDroppedForHeap_; }
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

  // The page currently being filled by layoutPages(), plus its fill position -- persists ACROSS
  // layoutPages() calls (when isFinalFlush=false) so a batch boundary landing mid-page continues
  // the same page on the next call instead of finalizing it early and starting a fresh one. See
  // layoutPages()'s isFinalFlush doc comment for the full rationale.
  VerticalPage pendingPage_;
  // A paragraph break recorded at exactly the end of a batch's stream (trailing newline) --
  // carried across reset() and re-recorded at the start of the next batch. See layoutPages().
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

  // Call before every stream_.push_back(). Only checks free heap when the vector is actually
  // about to reallocate (size == capacity) -- cheap in the common case where capacity headroom
  // from reserve() already covers this push. Returns false (and latches oom_) if a reallocation
  // would be needed and heap is too tight to risk it; the caller should skip this element.
  bool canPushStreamChar();

  // Codepoint-estimating, request-size-aware reserve for stream_ (see .cpp for the full story --
  // both the byte-count-as-slot-count over-request and the unchecked-request-size reserve have
  // crashed a real device).
  void reserveStreamFor(size_t utf8Bytes);

  int charAdvancePx() const;
};
