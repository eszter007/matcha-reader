#pragma once

#include <Epub/RubyGlossary.h>
#include <Epub/VerticalParsedText.h>
#include <WordLookup.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class GfxRenderer;

class Page;

// The Word Lookup page pre-scan, extracted from EpubReaderWordLookupActivity into a RESUMABLE
// state machine so EpubReaderActivity can run it in small slices during idle loop() ticks while
// the user reads the page. The scan segments the page's text with thousands of dictionary
// lookups (~2.5-4s of SD reads even with the sparse index), which is far too long to block on
// when the user taps Word Lookup -- but is invisible when it happens during the seconds the user
// spends reading the page. The activity receives the (usually finished) scan and completes any
// remainder synchronously.
//
// The scan is strictly sequential over the page's positions and each discovered entry is
// display-filtered (noise particles, conjugation fragments) at the moment it is found, so the
// selectable list only ever GROWS -- a caller can show partial results while stepping continues
// (progressive Word Lookup open) without entries later disappearing under the cursor.
class WordSelectionScan {
 public:
  struct GlyphRef {
    uint16_t x;
    uint16_t y;
    uint16_t column;
    uint16_t row;
    uint32_t codepoint;
    uint32_t paragraphIndex;
    // Cells this entry's match covers, leading digits included (15人 = 3). Only meaningful in
    // selectableGlyphs, where it is what lets a caller draw a highlight box over the word ON the
    // page without a dictionary read per cursor move -- the scan already knew the length and
    // used to discard it. 0 in allGlyphs; treat 0 as "one cell".
    uint8_t matchLen;
    // Advance in pixels, for a page laid out in lines, where glyphs are not one cell wide. 0 in
    // a vertical page: its cells are the caller's cellPx.
    uint8_t width;
    // Index into the scan's furigana list for the ruby printed on this glyph (a group reading sits
    // on its first glyph), kRubyCovered for a glyph under another glyph's ruby, or kNoRuby.
    uint16_t ruby = 0xFFFF;
  };
  static constexpr uint16_t kNoRuby = 0xFFFF;
  static constexpr uint16_t kRubyCovered = 0xFFFE;
  // Longer furigana is not a word reading; it is not kept.
  static constexpr size_t kMaxRubyBytes = 48;
  // The furigana printed on allGlyphs[i], or nullptr.
  const std::string* rubyAt(size_t i) const {
    return i < allGlyphs.size() && allGlyphs[i].ruby < rubies_.size() && allGlyphs[i].ruby != kRubyCovered
               ? &rubies_[allGlyphs[i].ruby]
               : nullptr;
  }

  // Where a horizontal page's characters are drawn, one entry per codepoint of every word in
  // page order. Measured by the reader while its fonts are still resident (measurePage): the
  // scan itself is built after they are released, when an advance can only be read back off
  // the card one glyph at a time.
  struct GlyphBox {
    uint16_t x;
    uint16_t y;
    uint8_t width;
  };
  struct LineGeometry {
    std::unique_ptr<GlyphBox[]> boxes;
    size_t count = 0;
    bool valid() const { return boxes != nullptr; }
  };
  // suppressRuby: ruby is hidden (shown ruby pushes the base text down its line). Leaves `out`
  // invalid when the page has no text or the table does not fit in memory.
  static void measurePage(const Page& page, const GfxRenderer& renderer, int fontId, bool suppressRuby,
                          LineGeometry& out);

  // Populate allGlyphs from a page and reset the state machine. Vertical (tategaki) mode.
  // Each glyph gets its cell's origin (see verticalCellTop), cellPx being the reader's cell size.
  void initFromVerticalPage(const VerticalPage& page, int cellPx);
  // Horizontal (yokogaki) mode: flattens the page's lines into one continuous character stream.
  // With a geometry each glyph also gets its place on the page -- x/y as drawn, column = its
  // line, row = its x, so "the next column" and "the closest row" mean the next line and the
  // glyph straight below, exactly as they do for a vertical page turned on its side.
  void initFromPage(const Page& page, const LineGeometry* geometry = nullptr);
  // Manga mode: a plain UTF-8 text blob (panel or combined page text). Newlines are dropped.
  void initFromUtf8Text(const std::string& text);

  // Append the start of the NEXT page as lookup context, so a word split across the page boundary
  // can still be segmented and looked up. Call after an initFrom*(); paragraphIndex is the next
  // page's first paragraph, which makes the check free: the text builder already stops at a
  // paragraph change, so context from a DIFFERENT paragraph is ignored without a special case.
  //
  // Context only. These glyphs carry no page position and never become selectable -- the reader
  // cannot put a cursor on a character that is not on screen -- so they extend what a word at the
  // page end can MATCH without adding anywhere new to point at. A word beginning on the last
  // on-page character reaches at most WordLookup::MAX_CONJUGATION_CHARS - 1 further.
  void appendLookupContext(const std::string& utf8, uint32_t paragraphIndex);
  // Number of leading allGlyphs entries that are really on the page. Anything reading a glyph's
  // geometry (column/row/x/y) must stop here: context cells carry no position, and 0/0 is a valid
  // vertical cell. Text building may read past it -- that is what the context is for.
  size_t onPageGlyphCount() const { return contextStart; }
  // Lets a name the book annotated (緋倉美津子) be selected whole when the dictionary only knows
  // its parts. Call before tryLoadCache: the glossary size is part of the cache fingerprint.
  void setBookGlossary(const std::string& bookCachePath);
  // Characters of the longest glossary base `text` starts with, or 0. RAM only; a hash hit is
  // confirmed against the file by RubyGlossary::longestPrefix when the word is looked up.
  int glossarySpanAt(const std::string& text) const;
  static constexpr int kLookupContextChars = WordLookup::MAX_CONJUGATION_CHARS - 1;

  // Run scan/filter work for up to maxMillis (pass UINT32_MAX to run to completion).
  // Returns true when the scan is fully done.
  bool step(uint32_t maxMillis);
  bool isDone() const { return phase == Phase::Done; }

  // True when initFrom*() or step() abandoned work because the heap couldn't grow a vector, so
  // the selectable list is a partial (often tiny) result. Callers that can free heap (release
  // font caches) then restartStepScan() to recover the full result -- see restartStepScan().
  bool wasTruncated() const { return scanTruncated; }

  // Re-run the step scan over the already-built allGlyphs after the caller has freed heap. Clears
  // the partial selectable map, the truncation flag, and the scan cursor, but KEEPS allGlyphs
  // (the step()-time abort never touches it), so a single low-heap rescan recovers every word the
  // fragmented first pass dropped. No-op-safe if allGlyphs is empty.
  void restartStepScan();

  // Single-slot per-book result cache: the completed selectable map for ONE (spine, page),
  // self-validated by a hash of allGlyphs (so a section rebuild with different layout/content
  // invalidates it) and the main dictionary's file size (so a dictionary swap does too). Also
  // carries the cursor position the user was last at, so re-opening the same page resumes where
  // they left off instead of restarting at the first word.
  // tryLoadCache: call after initFrom*(); on a hit fills the selectable map and marks the scan
  // Done -- the multi-second page scan is skipped entirely -- and sets restoredCursorIndex.
  // saveCache: call once isDone(), with the cursor position to remember for next time.
  bool tryLoadCache(const std::string& path, uint16_t spineIndex, uint16_t pageIndex);
  bool saveCache(const std::string& path, uint16_t spineIndex, uint16_t pageIndex, uint16_t cursorIndex) const;

  // Set by a successful tryLoadCache() to the cursor position at last exit; kNoRestoredCursor
  // (the reset() default) means no cache hit occurred, so the caller should start at word one.
  static constexpr uint16_t kNoRestoredCursor = 0xFFFF;
  uint16_t restoredCursorIndex = kNoRestoredCursor;

  std::vector<GlyphRef> allGlyphs;         // Full glyph list for building lookup text
  std::vector<GlyphRef> selectableGlyphs;  // Positions with a dictionary match
  std::vector<size_t> selectToAllIdx;      // Maps selectableGlyphs index -> allGlyphs index
  // First index in allGlyphs that is off-page lookup context (see appendLookupContext).
  // allGlyphs.size() when there is none, so it is always a valid "end of the page" bound.
  size_t contextStart = 0;

  // Where the walk currently sits in allGlyphs. Every cell's page position is known from
  // initFrom*(), but only segmented cells can be selected.
  // NOT a "everything below this is mapped" frontier, and not a processed count: the walk is
  // re-aimed on demand (aimAtGlyph), so it can sit anywhere and cells on either side of it may
  // already be done. Ask isGlyphMapped() for mapping decisions; this is for progress logging.
  size_t scannedGlyphs() const { return scanPos; }

  // Point the walk at `glyphIndex`. The walk is free to start anywhere and to be re-aimed at any
  // time: every segmented cell is recorded in a per-glyph bitmap, so positions are never scanned
  // twice and the order they are visited in does not matter. Vertical select mode aims it at the
  // middle column on open (the cursor opens there) and re-aims it at whatever column the reader
  // jumps to, so a jump segments the ~20 cells asked for instead of everything in between.
  //
  // Scanning starts up to kMaxLookupChars BEFORE the target so a word overlapping the target is
  // segmented with its real context; those earlier cells are read but not recorded, leaving them
  // for whoever scans them properly. selectableGlyphs therefore grows out of reading order --
  // callers stepping word by word must order by selectToAllIdx.
  void aimAtGlyph(size_t glyphIndex);
  // Selectable word covering `glyphIndex`, else the first one after it with every cell in between
  // segmented; -1 while the walk has not got that far.
  int wordAtOrAfter(size_t glyphIndex) const;
  // True when the cell at `glyphIndex` has been segmented. Exact for any walk order, which
  // comparing against scannedGlyphs() cannot be once the walk is re-aimed.
  bool isGlyphMapped(size_t glyphIndex) const {
    if (phase == Phase::Done) return true;
    if (glyphIndex >= allGlyphs.size() || scannedBits.empty()) return false;
    return (scannedBits[glyphIndex >> 3] >> (glyphIndex & 7)) & 1;
  }

  // Whether the scan segments by weighed whole-run splits (Chinese). The runtime lookup then
  // takes the scan's word length instead of the longest match from the cursor.
  bool usesRunSegmentation() const { return chineseMode_; }
  // Characters the runtime lookup may take from selectable word `selectableIdx`: in Chinese mode
  // exactly the word the run segmentation chose (和 before 尚未, not 和尚), unless that word was
  // cut at the page edge and its span is only the on-page part of a longer match; otherwise the
  // full window, for the longest-match path.
  int lookupCharLimit(size_t selectableIdx) const {
    if (selectableIdx >= selectableGlyphs.size() || selectableIdx >= selectToAllIdx.size()) return kMaxLookupChars;
    const uint8_t span = selectableGlyphs[selectableIdx].matchLen;
    const bool cutAtPageEdge = span > 0 && selectToAllIdx[selectableIdx] + span >= contextStart;
    if (!chineseMode_) {
      // A span the page edge cut is shorter than the word: read the longest match. A conjugation
      // the scan matched past the window keeps its own length (WordLookup::MAX_CONJUGATION_CHARS).
      if (cutAtPageEdge) return kMaxMatchChars;
      return span > kMaxLookupChars ? span : kMaxLookupChars;
    }
    if (span == 0 || cutAtPageEdge) return kMaxLookupChars;
    return span < kMaxLookupChars ? span : kMaxLookupChars;
  }

  // Shared helpers, also used by EpubReaderWordLookupActivity's runtime lookups.
  static constexpr int kMaxLookupChars = 8;
  // The longest match a word can have: a conjugation may run past the lookup window.
  static constexpr int kMaxMatchChars = WordLookup::MAX_CONJUGATION_CHARS;
  // A word beginning on the last on-page character reaches at most kMaxMatchChars - 1 further.
  static_assert(kLookupContextChars >= kMaxMatchChars - 1, "context must cover the longest match");
  static void encodeUtf8(uint32_t cp, std::string& out);
  static bool isLookupableChar(uint32_t cp);
  static bool isKatakana(uint32_t cp);
  static bool isCJK(uint32_t cp);
  static bool isDigitCp(uint32_t cp);
  // A katakana run of >=2 chars immediately followed by an honorific (さん/くん/ちゃん/さま/様/氏)
  // is almost always a proper name being addressed, so it should stay one unit even when no
  // dictionary covers it -- otherwise a fictional name fragments into unrelated real sub-words
  // (ヘムレンさん -> ヘム=heme + レン). Returns the katakana run length IN CHARS, or 0 if `text`
  // (from byte 0) doesn't start with such a name+honorific pattern. Shared by the scan (to group
  // the selectable unit) and the runtime lookup (to display the whole name).
  static size_t katakanaNameRunBeforeHonorific(const std::string& text);
  // A greedy dictionary match can absorb a trailing case-particle onto a kanji stem (東の, 私は)
  // and land on a junk entry. When the match is [kanji...][particle] and the stem alone is a
  // valid word, shorten `result` to the stem.
  static bool stripTrailingParticle(const std::string& text, WordLookupResult& result, bool needDefinition = true);

  // Heap-aware growth helpers -- bare vector reserve()/push_back() aborts the whole device on
  // OOM under -fno-exceptions (see CLAUDE.md and the .cpp for the rationale).
  static void reserveGlyphsSafe(std::vector<GlyphRef>& vec, size_t count);
  static bool pushGlyphSafe(std::vector<GlyphRef>& vec, const GlyphRef& g);

 private:
  // Furigana strings of this page, indexed by GlyphRef::ruby. Small: one per annotated base.
  std::vector<std::string> rubies_;
  uint16_t keepRuby(const std::string& ruby);
  enum class Phase : uint8_t { Scan, Done };
  Phase phase = Phase::Scan;
  size_t scanPos = 0;  // next allGlyphs index the scan will examine
  // One bit per allGlyphs entry: set once that cell has been segmented. This is what lets the
  // walk be re-aimed freely -- it replaces "everything below scanPos is done", which only held
  // while the walk was a single sequential pass. 219 cells is 28 bytes.
  std::vector<uint8_t> scannedBits;
  // Cells before this are read for context only: their matches are not recorded and their bits
  // are not set, so the walk that properly owns them still scans them.
  size_t recordFrom = 0;
  void markScanned(size_t glyphIndex) {
    if (glyphIndex < allGlyphs.size() && !scannedBits.empty()) scannedBits[glyphIndex >> 3] |= (1u << (glyphIndex & 7));
  }
  // Off-page context cells are never walked, so they would stay unmapped forever: mark them
  // segmented so nextUnscanned() and the select-mode "closest unmapped cell" search skip them
  // instead of repeatedly aiming the walk at work it will never do.
  void markContextScanned() {
    for (size_t i = contextStart; i < allGlyphs.size(); i++) markScanned(i);
  }
  // Next cell at or after `from` that has not been segmented; allGlyphs.size() when none remain.
  size_t nextUnscanned(size_t from) const;
  // Size the per-cell bitmap for the glyph list just built. Nothrow: on OOM the bitmap stays
  // empty, isGlyphMapped() then reports nothing as mapped, and the walk still completes -- it
  // just cannot skip work it has already done.
  void allocScannedBits();
  size_t skipUntil = 0;  // characters inside an already-matched word are skipped
  // Set true when initFrom*() or step() had to abandon work because the heap couldn't grow a
  // vector (dropped glyphs / partial scan). A truncated scan finds too few (often zero)
  // selectable words; persisting that result to the on-SD cache would poison every later open
  // of this page -- even after the heap recovers -- so saveCache() refuses when this is set.
  bool scanTruncated = false;

  void reset();
  uint32_t glyphContentHash() const;
  void scanOnePosition();
  // Chinese: segment the run of hanzi starting at allGlyphs[from] as a whole, choosing the
  // split with the best total word priority (the dictionary's frequency rank) rather than the
  // longest match at each step, and record every word in it. Returns false when the cell does not
  // start a hanzi run, so the caller falls through to the per-position path.
  bool scanChineseRun(size_t from);
  // True while the dictionary folder is Chinese: text with no conjugation, where the kana
  // heuristics of scanOnePosition() never fire and a crossing ambiguity (结婚的/和尚/未) is best
  // settled by comparing whole-run splits.
  bool chineseMode_ = false;
  std::vector<RubyGlossary::SpanKey> glossaryKeys_;  // sorted by hash
  uint16_t glossaryCount_ = 0;
  uint32_t segmentationFingerprint() const;
  // Longest hanzi run the segmenter weighs at once. Chinese clauses rarely run longer between
  // punctuation marks, and the run can always continue with the next call.
  static constexpr size_t kRunMax = 24;
  // Longest window probed per cell: the full lookup window, so seven- and eight-character set
  // phrases and names (一年之计在于春, 中华人民共和国) are found. lookupAll() stops at the first
  // prefix no headword starts with, so most cells cost two or three probes whatever the limit.
  static constexpr int kChineseMaxWindow = kMaxLookupChars;
  // Per-cell candidate priorities for the run being segmented (index: cell, window length - 1)
  // and the dynamic-programming tables. Members, not locals: ~250 bytes is past the stack budget.
  uint8_t runPriority_[kRunMax][kMaxLookupChars] = {};
  uint8_t runFound_[kRunMax] = {};
  int16_t runBest_[kRunMax + 1] = {};
  uint8_t runBestLen_[kRunMax + 1] = {};
  // The display filter (bare particles, conjugation fragments), applied to a matched position
  // before it is added to selectableGlyphs.
  bool passesDisplayFilter(size_t allIdx, int matchChars, const std::string& lookupText, size_t matchBytes) const;
};
