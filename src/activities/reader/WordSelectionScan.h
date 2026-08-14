#pragma once

#include <Epub/VerticalParsedText.h>
#include <WordLookup.h>

#include <cstdint>
#include <string>
#include <vector>

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
  };

  // Populate allGlyphs from a page and reset the state machine. Vertical (tategaki) mode.
  void initFromVerticalPage(const VerticalPage& page);
  // Horizontal (yokogaki) mode: flattens the page's lines into one continuous character stream.
  void initFromPage(const Page& page);
  // Manga mode: a plain UTF-8 text blob (panel or combined page text). Newlines are dropped.
  void initFromUtf8Text(const std::string& text);

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
  // True when the cell at `glyphIndex` has been segmented. Exact for any walk order, which
  // comparing against scannedGlyphs() cannot be once the walk is re-aimed.
  bool isGlyphMapped(size_t glyphIndex) const {
    if (phase == Phase::Done) return true;
    if (glyphIndex >= allGlyphs.size() || scannedBits.empty()) return false;
    return (scannedBits[glyphIndex >> 3] >> (glyphIndex & 7)) & 1;
  }

  // Shared helpers, also used by EpubReaderWordLookupActivity's runtime lookups.
  static constexpr int kMaxLookupChars = 8;
  static void encodeUtf8(uint32_t cp, std::string& out);
  static bool isLookupableChar(uint32_t cp);
  static bool isKatakana(uint32_t cp);
  static bool isHiragana(uint32_t cp);
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
  // The display filter (bare particles, conjugation fragments), applied to a matched position
  // before it is added to selectableGlyphs.
  bool passesDisplayFilter(size_t allIdx) const;
};
