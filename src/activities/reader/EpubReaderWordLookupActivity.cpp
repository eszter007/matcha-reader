#include "EpubReaderWordLookupActivity.h"

#include <Arduino.h>
#include <DictIndex.h>
#include <Epub/RubyGlossary.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <SdCardFontSystem.h>
#include <WordLookup.h>

#include <algorithm>
#include <cassert>
#include <climits>
#include <cstdlib>
#include <cstring>

#include "CrossPointSettings.h"
#include "DefinitionTextRenderer.h"
#include "Epub/Page.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "components/DictionaryPanel.h"
#include "components/UITheme.h"
#include "fontIds.h"

EpubReaderWordLookupActivity::EpubReaderWordLookupActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                           const VerticalPage& page, std::string scanCachePath,
                                                           const uint16_t spineIndex, const uint16_t pageIndex,
                                                           const VerticalSelectContext& selectContext)
    : Activity("WordLookup", renderer, mappedInput),
      selectCtx(selectContext),
      scanCachePath(std::move(scanCachePath)),
      scanSpine(spineIndex),
      scanPage(pageIndex) {
  const size_t slash = this->scanCachePath.find_last_of('/');
  if (slash != std::string::npos) bookCachePath = this->scanCachePath.substr(0, slash);
  if (selectCtx.valid()) {
    mode = Mode::Select;
    // Nothing has to be painted for the first frame when the reader's page is still on screen:
    // the cursor is two XOR-ed rectangles over pixels that are already there.
    selectPageDrawn = selectCtx.pageOnScreen;
  }
  reclaimFontHeap();  // BEFORE building the scan -- see reclaimFontHeap()
  scan.initFromVerticalPage(page);
  initScanFromCacheOrBurst("vertical");
}

EpubReaderWordLookupActivity::EpubReaderWordLookupActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                           const Page& page, std::string scanCachePath,
                                                           const uint16_t spineIndex, const uint16_t pageIndex)
    : Activity("WordLookup", renderer, mappedInput),
      scanCachePath(std::move(scanCachePath)),
      scanSpine(spineIndex),
      scanPage(pageIndex) {
  const size_t slash = this->scanCachePath.find_last_of('/');
  if (slash != std::string::npos) bookCachePath = this->scanCachePath.substr(0, slash);
  reclaimFontHeap();  // BEFORE building the scan -- see reclaimFontHeap()
  scan.initFromPage(page);
  initScanFromCacheOrBurst("horizontal");
}

// Self-heal fragmentation BEFORE the scan builds its glyph vectors. Two reasons this must run
// first, not after initFrom*():
//   1) Building allGlyphs on a fragmented heap truncates it (pushGlyphSafe can't grow), so the
//      scan finds too few/zero selectable words. Coalescing first gives it room to complete.
//   2) Device telemetry showed maxAlloc degrading monotonically across open/close cycles
//      (28.7K -> 22.5K -> 19.4K ...) while total free fully recovered: font hot-group/slab
//      buffers regrown while RENDERING definitions persist past onExit and split the large
//      block the dict caches vacate. Left unchecked this ends in an allocation abort() a few
//      pages later (confirmed crash_report).
// Threshold is 40K (not the historical 28K): on the X3 (wider 528px viewport) the reader's
// resident font slab is larger, so the dict caches can fail to find contiguous space even above
// the old floor, surfacing as an empty scan ("no matches found"). Matches EpubReaderActivity's
// RESUME_HEAP_FLOOR so the tight X3-resume path (huge CSS book, maxAlloc bottoming near 7K)
// reliably reclaims before the scan runs. Fonts reload lazily; the reader re-warms on return.
void EpubReaderWordLookupActivity::reclaimFontHeap() {
  if (ESP.getMaxAllocHeap() < 40 * 1024) {
    LOG_INF("WLA", "Low contiguous heap (maxAlloc=%u); releasing font caches", ESP.getMaxAllocHeap());
    if (auto* fcm = renderer.getFontCacheManager()) {
      // This runs on the main task; the render task may be mid-render with glyph
      // pointers into the font cache (it holds the render lock for the whole
      // render()). Freeing under the lock waits that render out -- releasing
      // without it is a cross-task use-after-free (confirmed crash_report:
      // renderCharImpl faulted while this path freed the cache).
      RenderLock lock;
      fcm->releaseAllFontMemory();
      LOG_INF("WLA", "After font release: maxAlloc=%u", ESP.getMaxAllocHeap());
    }
  }
}

// A persisted scan for this exact page skips all scanning; otherwise start progressively.
void EpubReaderWordLookupActivity::initScanFromCacheOrBurst(const char* label) {
  if (!scanCachePath.empty() && scan.tryLoadCache(scanCachePath, scanSpine, scanPage)) {
    return;
  }
  runInitialBurst(label);
}

// Progressive open: scan only far enough to find the FIRST selectable word so the panel can show
// a definition within a few hundred ms. The rest of the page is mapped incrementally from loop()
// while the user reads (see there); moveCursor() scans further on demand if the user outruns it.
// The cap bounds the open even on a pathological page with no early match.
void EpubReaderWordLookupActivity::runInitialBurst(const char* label) {
  const uint32_t scanStart = millis();
  LOG_INF("WLA", "progressive scan (%s): %u characters", label, static_cast<unsigned>(scan.allGlyphs.size()));
  // Tategaki select mode opens its cursor mid-page, so segment the half the reader is looking at
  // FIRST: the walk starts at the middle column and wraps to the head region afterwards. The
  // burst below then stops at the first word as usual -- and that word is already a middle one,
  // so the cursor lands in place with no hop and no extra work at open.
  if (mode == Mode::Select) {
    uint16_t midColumn = 0;
    uint16_t midRow = 0;
    size_t firstGlyph = 0;
    if (middleTarget(midColumn, midRow, firstGlyph)) scan.aimAtGlyph(firstGlyph);
  }
  while (!scan.isDone() && scan.selectableGlyphs.empty() && millis() - scanStart < 1500) {
    stepScan(50);
  }
  // Walk POSITION, not a processed count: a wrapped walk starts mid-page, so this is where the
  // frontier sits in allGlyphs, not how much of the page has been segmented.
  LOG_INF("WLA", "progressive scan (%s): ready after %u ms (walk at %u/%u)", label, millis() - scanStart,
          static_cast<unsigned>(scan.scannedGlyphs()), static_cast<unsigned>(scan.allGlyphs.size()));
}

// See the header: heal a low-heap-truncated scan once by freeing fonts and re-walking the intact
// glyph list. cursorIndex is intentionally left alone -- the rebuilt selectable list only grows,
// and every caller already guards against an out-of-range cursor while it refills, so the user's
// position resumes naturally once the rescan passes it again.
bool EpubReaderWordLookupActivity::stepScan(uint32_t budgetMs) {
  // Definition rendering leaves compressed-font groups resident. Reclaim before the next scan
  // slice needs to grow a vector; waiting until that growth fails discards progress and rescans
  // the whole page. This is the same recovery used below, just before damage instead of after it.
  if (ESP.getMaxAllocHeap() < 20 * 1024) {
    RenderLock lock;
    if (auto* fcm = renderer.getFontCacheManager()) {
      fcm->releaseAllFontMemory();
      LOG_INF("WLA", "Reclaimed fonts before scan: maxAlloc=%u", ESP.getMaxAllocHeap());
    }
  }
  const bool done = scan.step(budgetMs);
  if (scan.wasTruncated() && !scanHealAttempted && !scan.allGlyphs.empty()) {
    scanHealAttempted = true;
    LOG_INF("WLA", "Scan truncated by low heap; releasing fonts and rescanning (maxAlloc=%u)", ESP.getMaxAllocHeap());
    // Heal under the render lock: this runs on the main task (loop()), and the
    // render task may be mid-render, drawing definition text from font-cache
    // glyphs and reading scan.selectableGlyphs. Freeing the cache / resetting the
    // scan without the lock is a cross-task use-after-free (confirmed
    // crash_report: renderCharImpl faulted at this exact moment).
    RenderLock lock;
    if (auto* fcm = renderer.getFontCacheManager()) {
      fcm->releaseAllFontMemory();
      LOG_INF("WLA", "After font release: maxAlloc=%u", ESP.getMaxAllocHeap());
    }
    scan.restartStepScan();
    return false;  // not done -- caller keeps stepping over the freshly-reset scan
  }
  return done;
}

void EpubReaderWordLookupActivity::onEnter() {
  Activity::onEnter();
  // Heap telemetry for the word-lookup OOM crash hunt (crash_report showed abort() on a tiny
  // string allocation inside performLookupImpl -- heap exhausted, cause unknown). Logged at
  // enter AND exit so a leak per open/close cycle shows as a declining series.
  LOG_INF("WLA", "onEnter heap: free=%u maxAlloc=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  // A scan-cache hit remembers the position the user was last at on this exact page -- resume
  // there instead of making them click back through every entry they've already seen.
  const bool restored = scan.restoredCursorIndex != WordSelectionScan::kNoRestoredCursor &&
                        scan.restoredCursorIndex < scan.selectableGlyphs.size();
  if (restored) cursorIndex = scan.restoredCursorIndex;

  // Select mode shows the page, not a definition, so opening it reads no dictionary entry at
  // all: the constructor's burst already stopped at the first selectable word, and the cursor is
  // drawn over pixels that are on screen. The definition read waits for Confirm.
  if (mode == Mode::Select) {
    // A restored position wins: it is where this reader actually was on this page.
    if (!restored) {
      cursorIndex = 0;
      selectMiddleOfPage();
    }
    refreshCursorBoxes();
    requestUpdate();
    return;
  }

  if (restored) {
    performLookup();
    requestUpdate();
    return;
  }
  // Find first position with a match
  const int maxIdx = static_cast<int>(scan.selectableGlyphs.size()) - 1;
  for (cursorIndex = 0; cursorIndex <= maxIdx; cursorIndex++) {
    performLookup();
    if (hasResult) break;
  }
  if (cursorIndex > maxIdx) cursorIndex = 0;
  requestUpdate();
}

void EpubReaderWordLookupActivity::onExit() {
  LOG_INF("WLA", "onExit heap: free=%u maxAlloc=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  // Persist the current cursor position (a no-op if the scan never finished, or the cache path
  // is unset) so the next open of this exact page resumes here instead of at word one.
  if (!scanCachePath.empty()) {
    scan.saveCache(scanCachePath, scanSpine, scanPage, static_cast<uint16_t>(cursorIndex));
  }
  // Return the dictionary cache memory (~30KB) to the pool -- the reader needs it for heavy
  // operations like re-pagination (zip inflate wants one contiguous 32KB block).
  DictIndex::releaseCaches();
  Activity::onExit();
}

void EpubReaderWordLookupActivity::moveCursor(int delta) {
  // Moving past the last already-discovered word while the background scan is still running:
  // scan forward just enough to reveal the next one (typically a few hundred ms), so early
  // rapid navigation works instead of clamping at a stale end.
  if (delta > 0 && !scan.isDone() && cursorIndex + delta >= static_cast<int>(scan.selectableGlyphs.size())) {
    const size_t want = static_cast<size_t>(cursorIndex + delta) + 1;
    while (!scan.isDone() && scan.selectableGlyphs.size() < want) {
      stepScan(50);
    }
  }
  if (scan.selectableGlyphs.empty()) return;
  const int maxIdx = static_cast<int>(scan.selectableGlyphs.size()) - 1;
  // scan.selectableGlyphs is already the pre-filtered list of positions buildSelectableGlyphs()
  // confirmed have a dictionary match -- every index in it is valid by construction, so this just
  // moves one step and shows whatever's there. The previous version re-validated via
  // performLookup() and kept advancing past any position where that didn't independently agree
  // with the scan, silently skipping entries end-users could never reach -- confirmed on a real
  // device as "every second entry is skipped" during navigation (1, 3, 5, 7, ...).
  int newIndex = cursorIndex + delta;
  if (scan.isDone()) {
    // The full page is mapped, so "the end" is real -- cycle past it instead of dead-ending,
    // matching how e-reader dictionaries commonly let you loop through a page's word list.
    if (newIndex < 0)
      newIndex = maxIdx;
    else if (newIndex > maxIdx)
      newIndex = 0;
  } else {
    // Background scan still running: "the end" isn't final yet, so clamp instead of cycling --
    // wrapping to word one here would be surprising and skip words not yet discovered.
    if (newIndex < 0) newIndex = 0;
    if (newIndex > maxIdx) newIndex = maxIdx;
  }
  cursorIndex = newIndex;
  performLookup();
}

// --- Select mode ----------------------------------------------------------------------------

// Index math for a reading-order step, without moveCursor()'s dictionary read: select mode shows
// no definition, so a step costs two rectangle inverts and one fast refresh. Returns false when
// the step would land past the scan frontier -- the word is probably there, it just has not been
// segmented yet, and the caller parks the move rather than blocking on it.
bool EpubReaderWordLookupActivity::stepCursor(const int delta, int& outIndex) {
  outIndex = cursorIndex;
  if (scan.selectableGlyphs.empty()) return scan.isDone();
  if (cursorIndex < 0 || cursorIndex >= static_cast<int>(scan.selectToAllIdx.size())) return scan.isDone();

  // selectableGlyphs is in DISCOVERY order, which the wrapped vertical walk makes different from
  // reading order (see WordSelectionScan::startAtGlyph). A reading-order step is therefore the
  // entry with the nearest page position on the requested side, not index +/- 1.
  const size_t here = scan.selectToAllIdx[static_cast<size_t>(cursorIndex)];
  int best = -1;
  size_t bestPos = 0;
  int firstIdx = -1;
  size_t firstPos = 0;
  int lastIdx = -1;
  size_t lastPos = 0;
  for (size_t i = 0; i < scan.selectToAllIdx.size(); i++) {
    const size_t pos = scan.selectToAllIdx[i];
    if (firstIdx < 0 || pos < firstPos) {
      firstIdx = static_cast<int>(i);
      firstPos = pos;
    }
    if (lastIdx < 0 || pos > lastPos) {
      lastIdx = static_cast<int>(i);
      lastPos = pos;
    }
    if (delta > 0 ? pos > here : pos < here) {
      const bool better = best < 0 || (delta > 0 ? pos < bestPos : pos > bestPos);
      if (better) {
        best = static_cast<int>(i);
        bestPos = pos;
      }
    }
  }

  if (best >= 0) {
    outIndex = best;
    return true;
  }
  // No neighbour on that side. Forward: the page may simply not be segmented that far yet, so
  // park rather than pretend it ends here. Backward needs no wait -- with the wrapped walk the
  // head region is segmented last, so "nothing before me" can also mean "not yet", and the same
  // park applies.
  if (!scan.isDone()) return false;
  // Walk complete, so the end is real, and what to do there depends on how the move got its size.
  // A single step cycles round, as the definition view does. An accumulated multi-step move
  // (presses that piled up while the walk caught up) stops at the end instead: wrapping would
  // land on a word at the opposite end of the page, which is never where those presses aimed.
  const bool singleStep = delta == 1 || delta == -1;
  if (singleStep) {
    outIndex = delta > 0 ? firstIdx : lastIdx;
  } else {
    outIndex = delta > 0 ? lastIdx : firstIdx;
  }
  if (outIndex < 0) outIndex = cursorIndex;
  return true;
}

void EpubReaderWordLookupActivity::moveSelection(const int delta) {
  int target = cursorIndex;
  if (!stepCursor(delta, target)) {
    // Keep counting presses while the walk catches up, so holding the button past the frontier
    // lands where the user aimed instead of one word short of it.
    if (pending.kind == PendingMove::Kind::Word && (pending.delta > 0) == (delta > 0)) {
      pending.delta += delta;
    } else {
      pending = PendingMove{};
      pending.kind = PendingMove::Kind::Word;
      pending.delta = delta;
    }
    return;
  }
  pending = PendingMove{};
  if (target == cursorIndex) return;
  cursorIndex = target;
  refreshCursorBoxes();
  requestUpdate();
}

// Jump to the neighbouring column. Vertical Japanese runs right to left, so direction +1 (the
// Left button) moves FORWARD in the text. The target is spatial and known immediately -- every
// cell's column comes from the page layout, not from the dictionary walk -- so the move is
// accepted even when that part of the page has not been segmented yet, and completed by
// resolvePendingMove() as the frontier reaches it.
void EpubReaderWordLookupActivity::jumpColumn(const int direction) {
  if (cursorIndex < 0 || cursorIndex >= static_cast<int>(scan.selectableGlyphs.size())) return;
  const auto& cur = scan.selectableGlyphs[static_cast<size_t>(cursorIndex)];
  uint16_t minColumn = 0;
  uint16_t maxColumn = 0;
  if (!columnBounds(minColumn, maxColumn)) return;
  // Round-trip at the page edges: stepping words already cycles at the ends, and a column jump
  // stopping dead at the last column made the two navigations disagree. Bounds come from the
  // layout, so this is right even before the walk has segmented that column.
  int target = static_cast<int>(cur.column) + direction;
  if (target > static_cast<int>(maxColumn)) {
    target = minColumn;
  } else if (target < static_cast<int>(minColumn)) {
    target = maxColumn;
  }

  pending = PendingMove{};
  pending.kind = PendingMove::Kind::Column;
  pending.delta = direction;
  pending.targetColumn = static_cast<uint16_t>(target);
  pending.anchorRow = cur.row;
  resolvePendingMove();  // usually lands at once: the walk is normally well ahead of the reader
  // Landed: get the next column in this direction ready, so holding the button stays fluid.
  if (pending.kind == PendingMove::Kind::None) prefetchColumn(static_cast<uint16_t>(target), direction);
}

bool EpubReaderWordLookupActivity::columnRange(const uint16_t column, size_t& first, size_t& last) const {
  bool found = false;
  for (size_t i = 0; i < scan.allGlyphs.size(); i++) {
    if (scan.allGlyphs[i].column != column) continue;
    if (!found) {
      first = i;
      found = true;
    }
    last = i;
  }
  return found;
}

bool EpubReaderWordLookupActivity::columnBounds(uint16_t& outMin, uint16_t& outMax) const {
  if (scan.allGlyphs.empty()) return false;
  outMin = UINT16_MAX;
  outMax = 0;
  for (const auto& g : scan.allGlyphs) {
    outMin = std::min(outMin, g.column);
    outMax = std::max(outMax, g.column);
  }
  return true;
}

bool EpubReaderWordLookupActivity::columnAnchorGlyph(const uint16_t column, const uint16_t row,
                                                     size_t& outGlyph) const {
  bool found = false;
  int bestDistance = INT_MAX;
  for (size_t i = 0; i < scan.allGlyphs.size(); i++) {
    const auto& g = scan.allGlyphs[i];
    if (g.column != column) continue;
    const int distance = std::abs(static_cast<int>(g.row) - static_cast<int>(row));
    if (!found || distance < bestDistance) {
      bestDistance = distance;
      outGlyph = i;
      found = true;
    }
  }
  return found;
}

void EpubReaderWordLookupActivity::prefetchColumn(const uint16_t fromColumn, const int direction) {
  uint16_t minColumn = 0;
  uint16_t maxColumn = 0;
  if (!columnBounds(minColumn, maxColumn)) return;
  int next = static_cast<int>(fromColumn) + direction;
  if (next > static_cast<int>(maxColumn)) next = minColumn;
  if (next < static_cast<int>(minColumn)) next = maxColumn;
  size_t first = 0;
  size_t last = 0;
  if (columnRange(static_cast<uint16_t>(next), first, last)) scan.aimAtGlyph(first);
}

int EpubReaderWordLookupActivity::selectableInColumn(const uint16_t column, const uint16_t anchorRow) const {
  int best = -1;
  int bestDistance = INT_MAX;
  for (size_t i = 0; i < scan.selectableGlyphs.size(); i++) {
    const auto& g = scan.selectableGlyphs[i];
    if (g.column != column) continue;
    const int distance = std::abs(static_cast<int>(g.row) - static_cast<int>(anchorRow));
    if (distance < bestDistance) {
      bestDistance = distance;
      best = static_cast<int>(i);
    }
  }
  return best;
}

// Column/row come from the layout, so the middle of the page is known the moment it loads --
// even while the dictionary walk is still working through it. Starting there rather than at word
// one halves the worst-case number of presses to any word, the same reasoning as the horizontal
// picker's "middle row nearest mid-screen".
bool EpubReaderWordLookupActivity::middleTarget(uint16_t& outColumn, uint16_t& outRow, size_t& outFirstGlyph) const {
  if (scan.allGlyphs.empty()) return false;

  uint16_t minColumn = UINT16_MAX;
  uint16_t maxColumn = 0;
  uint16_t minRow = UINT16_MAX;
  uint16_t maxRow = 0;
  for (const auto& g : scan.allGlyphs) {
    minColumn = std::min(minColumn, g.column);
    maxColumn = std::max(maxColumn, g.column);
    minRow = std::min(minRow, g.row);
    maxRow = std::max(maxRow, g.row);
  }
  outColumn = static_cast<uint16_t>((minColumn + maxColumn) / 2);
  outRow = static_cast<uint16_t>((minRow + maxRow) / 2);

  size_t first = 0;
  size_t last = 0;
  outFirstGlyph = columnRange(outColumn, first, last) ? first : 0;
  return true;
}

void EpubReaderWordLookupActivity::selectMiddleOfPage() {
  if (scan.allGlyphs.empty() || scan.selectableGlyphs.empty()) return;

  uint16_t midColumn = 0;
  uint16_t midRow = 0;
  size_t firstGlyph = 0;
  if (!middleTarget(midColumn, midRow, firstGlyph)) return;

  // Already walked that far -- the usual case, since the burst carries the walk to the middle.
  const int target = selectableInColumn(midColumn, midRow);
  if (target >= 0) {
    cursorIndex = target;
    return;
  }
  // Not yet mapped -- hand it to the same wait the user's own column jumps use, so the cursor
  // lands as the walk arrives instead of blocking here. The cursor stays on word one until then,
  // which is a valid position to look up or step away from.
  pending = PendingMove{};
  pending.kind = PendingMove::Kind::Column;
  pending.delta = 1;
  pending.targetColumn = midColumn;
  pending.anchorRow = midRow;
  resolvePendingMove();
}

// Completes a parked move once the scan has mapped what it needs. Called every tick while a move
// is parked. Nothing is drawn for the wait itself: select mode has no hint bar to put it in.
void EpubReaderWordLookupActivity::resolvePendingMove() {
  if (pending.kind == PendingMove::Kind::None) return;

  bool moved = false;

  bool stillWaiting = false;
  if (pending.kind == PendingMove::Kind::Word) {
    int target = cursorIndex;
    if (stepCursor(pending.delta, target)) {
      moved = target != cursorIndex;
      cursorIndex = target;
    } else {
      stillWaiting = true;
    }
  } else if (pending.hasWaitTarget && !scan.isDone() && !scan.isGlyphMapped(pending.waitUntilGlyph)) {
    stillWaiting = true;  // known-unreached column: one comparison, no re-walk
  } else {
    // Walk in the jump's direction: a column can legitimately hold no selectable word (all
    // particles, or a run of punctuation), and stopping there would strand the cursor. Running
    // out of columns is a completed move that simply had nowhere to go -- the request is dropped
    // either way, so a jump at the edge of the page can never leave a wait pending forever.
    uint16_t minColumn = 0;
    uint16_t maxColumn = 0;
    const bool haveBounds = columnBounds(minColumn, maxColumn);
    const int columnsOnPage = haveBounds ? (maxColumn - minColumn + 1) : 0;
    int column = pending.targetColumn;
    // At most one lap: a page whose every column is unselectable must end the search, not spin.
    for (int visited = 0; visited < columnsOnPage; visited++, column += pending.delta) {
      if (haveBounds) {
        if (column > static_cast<int>(maxColumn)) column = minColumn;
        if (column < static_cast<int>(minColumn)) column = maxColumn;
      }
      size_t first = 0;
      size_t last = 0;
      if (!columnRange(static_cast<uint16_t>(column), first, last)) continue;  // gap in the page
      size_t anchorGlyph = last;
      if (!columnAnchorGlyph(static_cast<uint16_t>(column), pending.anchorRow, anchorGlyph)) anchorGlyph = last;
      if (!scan.isDone() && !scan.isGlyphMapped(anchorGlyph)) {
        // Park on THIS column: the walk only moves on once a column is mapped, so the range is
        // looked up once per column entered rather than once per tick of the wait.
        pending.targetColumn = static_cast<uint16_t>(column);
        pending.waitUntilGlyph = anchorGlyph;
        pending.hasWaitTarget = true;
        stillWaiting = true;
        // Segment what the reader asked for, not everything between here and there: aiming the
        // walk at this column turns a wait for ~half a page into a wait for one column. Cells
        // already done are skipped, so nothing is scanned twice.
        scan.aimAtGlyph(anchorGlyph);
        break;
      }
      const int hit = selectableInColumn(static_cast<uint16_t>(column), pending.anchorRow);
      if (hit >= 0) {
        moved = hit != cursorIndex;
        cursorIndex = hit;
        break;
      }
    }
  }
  if (!stillWaiting) pending = PendingMove{};

  if (pending.kind != PendingMove::Kind::None) {
    return;
  }
  // Only a moved cursor changes what is on screen: select mode paints no hint bar, so a wait
  // starting or ending has nothing to redraw (a repaint here would cost a refresh for nothing).
  if (moved) {
    refreshCursorBoxes();
    requestUpdate();
  }
}

void EpubReaderWordLookupActivity::enterDefinition() {
  mode = Mode::Definition;
  pending = PendingMove{};
  // The definition view paints over the page, so the highlight goes with it. Clearing the record
  // of what is drawn is left to the render task, which owns it: selectPageDrawn = false already
  // routes the next select render through the repaint branch, and that resets it there.
  selectPageDrawn = false;
  initialRenderDone = false;
  performLookup();
}

void EpubReaderWordLookupActivity::returnToSelect() {
  mode = Mode::Select;
  selectPageDrawn = false;  // the definition overdrew the page; it has to be painted again
  // The definition view has its own word navigation, so the cursor may have moved while it was up.
  refreshCursorBoxes();
  initialRenderDone = false;
  fastRefreshCount = 0;
  requestUpdate();
}

// Returns false when the activity is finishing or has switched view, so the caller stops touching
// state this tick.
bool EpubReaderWordLookupActivity::handleSelectInput() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return false;
  }

  // On the page, a short power click SELECTS -- the same thing Look Up does. It is the one button
  // reachable without moving the hand off the side buttons that step words, and this view draws
  // no labels to say so. It only leaves the panel from the definition view (handleDefinitionInput),
  // where a click closes the dictionary outright rather than stepping back to the page.
  if (ReaderUtils::wordLookupPowerClick(mappedInput)) {
    if (pending.kind == PendingMove::Kind::None) {
      enterDefinition();
      return false;
    }
    return true;
  }

  // The panel is entered while Confirm is still held (the reader opens it on a long press), so
  // the release that follows is not a selection -- wait for a fresh press first.
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) confirmPressSeen = true;
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && confirmPressSeen) {
    // Blocked only while a move is parked: the user is waiting to arrive somewhere, and looking
    // up the word they are leaving would be the wrong entry for a wait of a few hundred ms.
    // A page with nothing selectable still opens the definition view, which is where "No match
    // found" (or "Loading..." while the walk runs) lives -- otherwise Confirm would do nothing at
    // all and the page would look broken rather than empty.
    if (pending.kind == PendingMove::Kind::None) {
      enterDefinition();
      return false;
    }
    return true;
  }

  // Side buttons step word by word (down a column is the same gesture as a page turn), front
  // Left/Right jump columns. Button::Up/Down are the physical side buttons whatever the page-turn
  // layout setting is, so this mapping holds on every device that has them.
  buttonNavigator.onPressAndContinuous(MappedInputManager::Button::Down, [this] { moveSelection(1); });
  buttonNavigator.onPressAndContinuous(MappedInputManager::Button::Up, [this] { moveSelection(-1); });
  buttonNavigator.onPressAndContinuous(MappedInputManager::Button::Left, [this] { jumpColumn(1); });
  buttonNavigator.onPressAndContinuous(MappedInputManager::Button::Right, [this] { jumpColumn(-1); });
  return true;
}

void EpubReaderWordLookupActivity::refreshCursorBoxes() {
  // Built in locals first: the scan reads below are the slow part, and the render task must never
  // observe a half-built set. Only the publish at the end runs with the render task locked out.
  HighlightBox boxes[kMaxHighlightBoxes];
  int count = 0;

  if (cursorIndex >= 0 && static_cast<size_t>(cursorIndex) < scan.selectToAllIdx.size()) {
    const size_t start = scan.selectToAllIdx[static_cast<size_t>(cursorIndex)];
    if (start < scan.allGlyphs.size()) {
      const size_t span = std::max<size_t>(scan.selectableGlyphs[static_cast<size_t>(cursorIndex)].matchLen, 1);
      const size_t end = std::min(start + span, scan.allGlyphs.size());
      const int cellPx = selectCtx.cellPx;

      size_t i = start;
      while (i < end && count < kMaxHighlightBoxes) {
        const uint16_t column = scan.allGlyphs[i].column;
        size_t j = i + 1;
        while (j < end && scan.allGlyphs[j].column == column) j++;
        const auto& firstCell = scan.allGlyphs[i];
        const auto& lastCell = scan.allGlyphs[j - 1];
        const int top = std::min(firstCell.y, lastCell.y) + selectCtx.marginTop;
        const int bottom = std::max(firstCell.y, lastCell.y) + selectCtx.marginTop + cellPx;
        HighlightBox& box = boxes[count++];
        // Cell-exact, no padding: the cell IS the em box, so the box lands clear of the
        // neighbouring column's ink and of any ruby, which is drawn outside the cell.
        box.x = static_cast<int16_t>(firstCell.x + selectCtx.marginLeft);
        box.y = static_cast<int16_t>(top);
        box.w = static_cast<int16_t>(cellPx);
        box.h = static_cast<int16_t>(bottom - top);
        i = j;
      }
    }
  }

  // Publish as one unit. The critical section spans a ~32-byte copy, which is what it takes to
  // stop the render task from pairing this move's count with the previous move's rectangles.
  portENTER_CRITICAL(&boxMux);
  for (int i = 0; i < count; i++) cursorBoxes[i] = boxes[i];
  cursorBoxCount = count;
  portEXIT_CRITICAL(&boxMux);
}

void EpubReaderWordLookupActivity::invertBoxes(const HighlightBox* boxes, const int count) const {
  for (int i = 0; i < count; i++) renderer.invertRect(boxes[i].x, boxes[i].y, boxes[i].w, boxes[i].h);
}

void EpubReaderWordLookupActivity::renderSelect() {
  const bool repaint = !selectPageDrawn;
  if (repaint) {
    renderer.clearScreen();
    // Only count the page as drawn when it actually was: a failed repaint (slot not re-faultable
    // under low heap) would otherwise stick a blank frame for the rest of the panel's life, with
    // the cursor XOR-ing over emptiness. Leaving the flag clear retries on the next render.
    const bool drew = selectCtx.repaintPage && selectCtx.repaintPage(selectCtx.repaintCtx);
    drawnBoxCount = 0;  // the repaint took the old highlight with it
    selectPageDrawn = drew;
    if (!drew) {
      // The page could not be drawn. Drawing the cursor now would XOR it onto a cleared screen --
      // the blank frame with a floating highlight this flag exists to avoid. Leave the frame
      // unflushed and retry on the next render, when the slot may be faultable again.
      return;
    }
  } else if (drawnBoxCount > 0) {
    invertBoxes(drawnBoxes, drawnBoxCount);  // erase: inverting again restores what was under it
    drawnBoxCount = 0;
  }

  // No hint bar here. Vertical text is full-bleed -- the reader's page already occupies the
  // bottom band -- so painting hints over it hides the last line of every column. The panel is
  // drawn ON the page precisely so the text stays readable, and a label bar undoes that. The
  // buttons are unchanged and documented in USER_GUIDE 6.2; only the on-screen labels are gone.
  // Hints are still drawn in the definition view, which owns its whole screen.

  // Take the whole set in one go, count included, and draw only from the copy: reading the count
  // a second time could pair it with rectangles this render never copied, XOR-ing a stale box
  // onto the page. drawnBoxes is what the erase above will undo, so it must be exactly this.
  portENTER_CRITICAL(&boxMux);
  const int count = cursorBoxCount;
  for (int i = 0; i < count; i++) drawnBoxes[i] = cursorBoxes[i];
  portEXIT_CRITICAL(&boxMux);
  drawnBoxCount = count;
  invertBoxes(drawnBoxes, drawnBoxCount);

  if (repaint) {
    // A freshly painted page deserves a clean pass; the fast LUT would carry over the definition
    // view's ghost.
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    fastRefreshCount = 0;
    return;
  }
  fastRefreshCount++;
  if (fastRefreshCount >= kFullRefreshInterval) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    fastRefreshCount = 0;
  } else {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }
}

std::string EpubReaderWordLookupActivity::buildLookupText(size_t startIdx) const {
  std::string text;
  if (startIdx >= scan.selectableGlyphs.size() || startIdx >= scan.selectToAllIdx.size()) return text;

  const size_t allStart = scan.selectToAllIdx[startIdx];
  const uint32_t paraIdx = scan.allGlyphs[allStart].paragraphIndex;
  int charCount = 0;

  for (size_t i = allStart; i < scan.allGlyphs.size() && charCount < WordSelectionScan::kMaxLookupChars; i++) {
    const auto& g = scan.allGlyphs[i];
    if (g.paragraphIndex != paraIdx) break;
    WordSelectionScan::encodeUtf8(g.codepoint, text);
    charCount++;
  }
  return text;
}

void EpubReaderWordLookupActivity::prependBookReading(const std::string& surface) {
  if (bookCachePath.empty() || surface.empty()) return;
  std::string readings;
  if (!RubyGlossary::lookup(bookCachePath, surface, readings)) return;
  std::string line = tr(STR_IN_THIS_BOOK);
  line += ' ';
  line += readings;
  // Blank line: DefinitionText::drawWrapped renders an empty line as a half-line gap,
  // visually separating the book reading from the dictionary entry below it.
  line += "\n\n";
  resultDefinition = line + resultDefinition;
}

void EpubReaderWordLookupActivity::performLookup() {
  // Hold the rendering mutex while the result strings are rebuilt: the render task wraps and
  // draws resultDefinition/resultHeadword CONCURRENTLY on its own task, and mutating them
  // mid-render tears the string under the renderer -- confirmed crash_report: out_of_range
  // abort inside DefinitionText::drawWrapped when navigation triggered a lookup during a slow
  // (multi-second) e-ink refresh. The lock briefly delays one render; requestUpdate() then
  // redraws with the fresh result.
  RenderLock lock;
  // Mid-session self-heal: the open-time check can't help when the heap degrades DURING a long
  // navigation session (font glyphs loaded per rendered definition accumulate; a crash_report
  // showed the definition read inside DictIndex aborting after renders had slowed from 1.2s to
  // 6.3s as the heap ran down). Same release as at open; fonts reload lazily.
  if (ESP.getMaxAllocHeap() < 20 * 1024) {
    LOG_INF("WLA", "Low heap mid-session (maxAlloc=%u); releasing font caches", ESP.getMaxAllocHeap());
    if (auto* fcm = renderer.getFontCacheManager()) {
      fcm->releaseAllFontMemory();
    }
  }
  // Signals render() to show "Loading..." instead of "No match found" while the lookup below
  // runs -- fast navigation otherwise briefly flashes the no-match text in the window between
  // clearing the previous result and the next lookup (~100-300ms) completing.
  lookupInFlight = true;
  performLookupImpl();
  lookupInFlight = false;
}

void EpubReaderWordLookupActivity::performLookupImpl() {
  hasResult = false;
  resultHeadword.clear();
  resultDefinition.clear();
  resultReading.clear();
  resultGrammar.clear();
  resultSource = nullptr;
  resultDictionaryLabel.clear();
  sectionText.clear();
  sectionLabel.clear();
  sectionReading.clear();
  sectionGrammar.clear();
  sectionKind.clear();
  sectionHead.clear();
  currentSection = 0;
  resultMatchLen = 0;
  scrollOffset = 0;
  totalLines = 9999;

  std::string text = buildLookupText(static_cast<size_t>(cursorIndex));
  if (text.empty()) return;

  // If the text starts with digits (2年, １５人), look up the counter/word that
  // follows and show the digits as a prefix so the reading is clear (2年).
  std::string digitPrefix;
  {
    size_t b = 0;
    while (b < text.size()) {
      auto c = static_cast<unsigned char>(text[b]);
      if (c >= '0' && c <= '9') {
        digitPrefix.push_back(static_cast<char>(c));
        b += 1;
      } else if (c == 0xEF && b + 2 < text.size() && static_cast<unsigned char>(text[b + 1]) == 0xBC &&
                 static_cast<unsigned char>(text[b + 2]) >= 0x90 && static_cast<unsigned char>(text[b + 2]) <= 0x99) {
        // Fullwidth digit ０-９ (U+FF10–U+FF19)
        digitPrefix.append(text, b, 3);
        b += 3;
      } else {
        break;
      }
    }
    if (b > 0 && b < text.size()) {
      text = text.substr(b);  // look up the part after the digits
    } else {
      digitPrefix.clear();  // nothing after digits, or no digits
    }
  }

  // Fictional katakana name + honorific (ヘムレンさん): if the dictionary only covers a prefix of
  // the name (ヘム) or nothing, show the whole katakana run instead. It has no dictionary entry,
  // so the definition body stays empty -- but it reads as one name rather than "heme". Dictionary
  // names (スナフキン) cover the whole run, so this branch doesn't fire for them.
  const size_t nameRun = WordSelectionScan::katakanaNameRunBeforeHonorific(text);
  if (nameRun >= 2) {
    WordLookupResult nr;
    int nrChars = 0;
    if (WordLookup::lookup(text, 0, nr)) {
      size_t pos = 0;
      while (pos < nr.matchLength && pos < text.size()) {
        auto c = static_cast<unsigned char>(text[pos]);
        if (c < 0x80)
          pos += 1;
        else if ((c & 0xE0) == 0xC0)
          pos += 2;
        else if ((c & 0xF0) == 0xE0)
          pos += 3;
        else
          pos += 4;
        nrChars++;
      }
    }
    if (static_cast<int>(nameRun) > nrChars) {
      size_t nb = 0;
      int nc = 0;
      while (nb < text.size() && nc < static_cast<int>(nameRun)) {
        auto c = static_cast<unsigned char>(text[nb]);
        if (c < 0x80)
          nb += 1;
        else if ((c & 0xE0) == 0xC0)
          nb += 2;
        else if ((c & 0xF0) == 0xE0)
          nb += 3;
        else
          nb += 4;
        nc++;
      }
      hasResult = true;
      resultHeadword = digitPrefix + text.substr(0, nb);
      resultDefinition = tr(STR_LOOKUP_NAME);  // no dictionary entry -- label it as a name
      resultSource = "JMnedict";
      resultMatchLen = static_cast<int>(nameRun);
      // Names are the glossary's prime case: the book's own furigana is often the ONLY
      // source for a name's reading.
      prependBookReading(text.substr(0, nb));
      requestUpdate();  // this early return would otherwise skip the requestUpdate() at the end,
                        // leaving the name un-rendered (screen keeps the previous word -> looks skipped)
      return;
    }
  }

  WordLookupResult result;
  if (WordLookup::lookup(text, 0, result)) {
    WordSelectionScan::stripTrailingParticle(text, result);
    hasResult = true;
    resultHeadword = digitPrefix + result.entry.headword;
    resultDefinition = std::move(result.entry.definition);
    DefinitionText::EntryMetadata metadata;
    DefinitionText::extractEntryMetadata(resultDefinition, resultHeadword, metadata);
    resultReading = std::move(metadata.reading);
    resultGrammar = std::move(metadata.grammar);
    resultSource = result.entry.sourceDict == DictIndex::DICT_NAMES     ? "JMnedict"
                   : result.entry.sourceDict == DictIndex::DICT_GRAMMAR ? "Grammar"
                                                                        : "JMdict";
    resultDictionaryLabel = std::move(metadata.source);
    prependBookReading(text.substr(0, std::min(result.matchLength, text.size())));
    int chars = 0;
    size_t pos = 0;
    while (pos < result.matchLength && pos < text.size()) {
      auto c = static_cast<unsigned char>(text[pos]);
      if (c < 0x80)
        pos += 1;
      else if ((c & 0xE0) == 0xC0)
        pos += 2;
      else if ((c & 0xF0) == 0xE0)
        pos += 3;
      else
        pos += 4;
      chars++;
    }
    resultMatchLen = chars;

    // For short hiragana-only matches (≤3 chars), check if the grammar dict
    // has a better entry and promote it to the main result. Functional words
    // like こと, もの, よう get unhelpful JMdict hits ("ancient capital").
    if (chars <= 3 && Storage.exists(DictIndex::grammarIdxPath())) {
      bool allHiragana = true;
      for (size_t b = 0; b < result.matchLength && b < text.size();) {
        auto c = static_cast<unsigned char>(text[b]);
        uint32_t cp = 0;
        if (c < 0x80) {
          cp = c;
          b += 1;
        } else if ((c & 0xE0) == 0xC0) {
          cp = ((c & 0x1F) << 6) | (text[b + 1] & 0x3F);
          b += 2;
        } else if ((c & 0xF0) == 0xE0) {
          cp = ((c & 0x0F) << 12) | ((text[b + 1] & 0x3F) << 6) | (text[b + 2] & 0x3F);
          b += 3;
        } else {
          b += 4;
        }
        if (cp < 0x3040 || cp > 0x309F) {
          allHiragana = false;
          break;
        }
      }
      if (allHiragana) {
        DictEntry gramEntry;
        if (DictIndex::lookupInFile(resultHeadword.c_str(), DictIndex::grammarIdxPath(), DictIndex::grammarDatPath(),
                                    gramEntry)) {
          resultDefinition = std::move(gramEntry.definition);
          DefinitionText::EntryMetadata grammarMetadata;
          DefinitionText::extractEntryMetadata(resultDefinition, resultHeadword, grammarMetadata);
          resultReading = std::move(grammarMetadata.reading);
          resultGrammar = std::move(grammarMetadata.grammar);
          resultDictionaryLabel = std::move(grammarMetadata.source);
          resultSource = "Grammar";
        }
      }
    }
  }

  // Grammar scan: search for grammar patterns in a window around the cursor.
  // Try starting from a few characters BEFORE the cursor (to catch patterns
  // like ことになる when cursor is on こと) and also from the cursor itself.
  hasGrammar = false;
  grammarHeadword.clear();
  grammarDefinition.clear();
  // The grammar overlay is a nicety on top of the main result. Its lookups build several
  // transient strings and read whole grammar entries; under a near-exhausted heap those
  // allocations abort() (-fno-exceptions) -- confirmed by a real device crash_report with a
  // ~30-byte string allocation failing in this block. Show the plain result instead of crashing.
  if (ESP.getMaxAllocHeap() < 16 * 1024) {
    LOG_ERR("WLA", "Skipping grammar scan, heap too low (maxAlloc=%u)", ESP.getMaxAllocHeap());
  } else if (Storage.exists(DictIndex::grammarIdxPath())) {
    const size_t allStart = scan.selectToAllIdx[static_cast<size_t>(cursorIndex)];
    const uint32_t paraIdx = scan.allGlyphs[allStart].paragraphIndex;

    // Try starting positions: cursor-3, cursor-2, cursor-1, cursor
    int bestGramLen = 0;
    for (int backoff = 3; backoff >= 0; backoff--) {
      size_t scanStart = allStart;
      for (int b = 0; b < backoff && scanStart > 0; b++) {
        scanStart--;
        if (scan.allGlyphs[scanStart].paragraphIndex != paraIdx) {
          scanStart++;
          break;
        }
      }

      std::string gramText;
      int gCharCount = 0;
      for (size_t j = scanStart; j < scan.allGlyphs.size() && gCharCount < 12; j++) {
        if (scan.allGlyphs[j].paragraphIndex != paraIdx) break;
        WordSelectionScan::encodeUtf8(scan.allGlyphs[j].codepoint, gramText);
        gCharCount++;
      }

      for (int wLen = std::min(gCharCount, 10); wLen >= 2; wLen--) {
        size_t byteEnd = 0;
        int cnt = 0;
        for (size_t b = 0; b < gramText.size() && cnt < wLen; cnt++) {
          auto c = static_cast<unsigned char>(gramText[b]);
          if (c < 0x80)
            b += 1;
          else if ((c & 0xE0) == 0xC0)
            b += 2;
          else if ((c & 0xF0) == 0xE0)
            b += 3;
          else
            b += 4;
          byteEnd = b;
        }
        std::string window = gramText.substr(0, byteEnd);
        DictEntry gramEntry;
        if (DictIndex::lookupInFile(window.c_str(), DictIndex::grammarIdxPath(), DictIndex::grammarDatPath(),
                                    gramEntry)) {
          if (gramEntry.headword != resultHeadword && wLen > bestGramLen) {
            bestGramLen = wLen;
            hasGrammar = true;
            grammarHeadword = std::move(gramEntry.headword);
            grammarDefinition = std::move(gramEntry.definition);
          }
          break;
        }
      }
    }
  }

  // Merge grammar into the definition so the single scroll-aware render loop
  // handles it (gets maxDefY clamping and scroll offset for free).
  if (hasGrammar) {
    // Built with one guarded reserve + appends: the old `a + b + c` temporary chain peaked at
    // roughly twice the combined definition size in contiguous heap -- an abort() risk exactly
    // when definitions are long. If even the reserve doesn't fit, keep the main result alone.
    const size_t mergedLen = resultDefinition.size() + grammarHeadword.size() + grammarDefinition.size() + 32;
    if (ESP.getMaxAllocHeap() > mergedLen + 8 * 1024) {
      resultDefinition.reserve(mergedLen);
      resultDefinition += "\n\n— Grammar: ";
      resultDefinition += grammarHeadword;
      resultDefinition += " —\n";
      resultDefinition += grammarDefinition;
    } else {
      LOG_ERR("WLA", "Skipping grammar merge, heap too low (maxAlloc=%u)", ESP.getMaxAllocHeap());
    }
  }

  splitDefinitionIntoSections();
  if (sectionText.empty())
    DefinitionText::formatEntryBody(resultDefinition, resultSource != nullptr && strcmp(resultSource, "Grammar") == 0
                                                          ? resultHeadword
                                                          : std::string());
  requestUpdate();
}

void EpubReaderWordLookupActivity::loop() {
  if (mode == Mode::Select) {
    if (!handleSelectInput()) return;
  } else if (!handleDefinitionInput()) {
    return;
  }

  // Progressive background scan: keep mapping the page's selectable words in small slices
  // between input polls (skipLoopDelay() holds full CPU and fast ticks while this runs, so a
  // slice never swallows a button press). Everything here runs on the main task -- the render
  // task only ever reads counter sizes -- so no lock is needed and, unlike the abandoned
  // reader-idle precompute, nothing can starve another activity's rendering.
  // The render task's font decompressor can temporarily consume nearly the entire largest block.
  // Do not overlap that transient allocation with dictionary reads/vector growth.
  if (!scan.isDone() && !RenderLock::peek()) {
    // A parked move is a user waiting on the frontier with nothing else to look at: spend longer
    // slices so it arrives sooner. Ordinary background mapping keeps the small slice.
    const bool done = stepScan(pending.kind != PendingMove::Kind::None ? 120 : 40);
    if (mode == Mode::Definition) {
      // The open can show "No match" if the initial burst found nothing yet -- promote the first
      // word as soon as the background scan discovers it.
      if (!hasResult && !scan.selectableGlyphs.empty()) {
        performLookup();
        requestUpdate();
      }
    } else if (cursorBoxCount == 0 && !scan.selectableGlyphs.empty()) {
      refreshCursorBoxes();  // first word of a cold page found: draw the cursor onto it
      requestUpdate();
    }
    if (done) {
      DictIndex::logAndResetStats("progressive scan complete");
      // Select mode shows no counter, so a redraw here would cost an e-ink flash for no change.
      if (mode == Mode::Definition) requestUpdate();
    }
  }

  if (mode == Mode::Select) resolvePendingMove();
}

bool EpubReaderWordLookupActivity::handleDefinitionInput() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // Back steps out of the definition and onto the page it came from, keeping the scan, the
    // cursor and the page pixels' place in the flow; only leaving select mode ends the panel.
    if (selectCtx.valid()) {
      returnToSelect();
      return false;
    }
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return false;
  }
  if (ReaderUtils::wordLookupPowerClick(mappedInput)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return false;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    performLookup();
    return false;
  }

  const bool sideButtonsForLookup =
      SETTINGS.wordLookupSideButtons != 0 && SETTINGS.sideButtonLayout != CrossPointSettings::SIDE_BUTTONS_DISABLED;
  const bool swapFrontButtons = mappedInput.isNavDirectionSwapped();
  const auto nextEntryButton =
      sideButtonsForLookup ? MappedInputManager::Button::PageForward : MappedInputManager::Button::Right;
  const auto previousEntryButton =
      sideButtonsForLookup ? MappedInputManager::Button::PageBack : MappedInputManager::Button::Left;
  const auto scrollDownButton =
      sideButtonsForLookup ? (swapFrontButtons ? MappedInputManager::Button::Left : MappedInputManager::Button::Right)
                           : MappedInputManager::Button::Down;
  const auto scrollUpButton =
      sideButtonsForLookup ? (swapFrontButtons ? MappedInputManager::Button::Right : MappedInputManager::Button::Left)
                           : MappedInputManager::Button::Up;
  if (pagedDefinition()) {
    // Tategaki reached this view from the page itself, with the word already chosen, so these
    // buttons walk the entry's sources instead of jumping to another word.
    buttonNavigator.onPressAndContinuous(nextEntryButton, [this] { moveSection(1); });
    buttonNavigator.onPressAndContinuous(previousEntryButton, [this] { moveSection(-1); });
  } else {
    buttonNavigator.onPressAndContinuous(nextEntryButton, [this] { moveCursor(1); });
    buttonNavigator.onPressAndContinuous(previousEntryButton, [this] { moveCursor(-1); });
  }
  // Paged mode moves a whole screenful per press; free scrolling keeps its 5-line nudge.
  const int step = pagedDefinition() ? std::max(1, visibleCapacity) : 5;
  buttonNavigator.onPressAndContinuous(scrollDownButton, [this, step] {
    if (hasResult && scrollOffset < maxScroll) {
      scrollOffset = std::min(maxScroll, scrollOffset + step);
      requestUpdate();
    }
  });
  buttonNavigator.onPressAndContinuous(scrollUpButton, [this, step] {
    if (scrollOffset > 0) {
      scrollOffset = std::max(0, scrollOffset - step);
      requestUpdate();
    }
  });
  return true;
}

void EpubReaderWordLookupActivity::moveSection(const int delta) {
  const int count = static_cast<int>(sectionText.size());
  if (count <= 1) return;
  const int next = currentSection + delta;
  if (next < 0 || next >= count) return;  // ends are hard stops: the entry is a list, not a ring
  currentSection = next;
  scrollOffset = 0;  // a new source starts at its top
  requestUpdate();
}

// Split the merged definition into one piece per source. DictIndex joins the entries it merges
// with "\n\n---\n", and the grammar entry is appended under its own "— Grammar: … —" heading;
// each piece ends with the attribution line its converter wrote ("JMdict | Tatoeba"), which is
// lifted out of the body and shown in the panel footer instead. Tategaki only -- horizontal and
// manga keep the single scrolling blob, where Left/Right move the word cursor.
void EpubReaderWordLookupActivity::splitDefinitionIntoSections() {
  sectionText.clear();
  sectionLabel.clear();
  sectionReading.clear();
  sectionGrammar.clear();
  sectionKind.clear();
  sectionHead.clear();
  currentSection = 0;
  if (!pagedDefinition() || resultDefinition.empty()) return;

  static constexpr char kEntrySep[] = "\n\n---\n";
  static constexpr char kGrammarSep[] = "\n\n— Grammar: ";
  const auto coveredBy = [](const std::string& subset, const std::string& superset) {
    size_t lineStart = 0;
    while (lineStart <= subset.size()) {
      const size_t lineEnd = subset.find('\n', lineStart);
      const size_t length = lineEnd == std::string::npos ? subset.size() - lineStart : lineEnd - lineStart;
      if (length > 0) {
        size_t found = superset.find(subset.data() + lineStart, 0, length);
        while (found != std::string::npos && ((found > 0 && superset[found - 1] != '\n') ||
                                              (found + length < superset.size() && superset[found + length] != '\n'))) {
          found = superset.find(subset.data() + lineStart, found + 1, length);
        }
        if (found == std::string::npos) return false;
      }
      if (lineEnd == std::string::npos) break;
      lineStart = lineEnd + 1;
    }
    return true;
  };
#ifndef NDEBUG
  assert(coveredBy("1. alpha", "1. alpha\n2. beta"));
  assert(!coveredBy("1. alpha\n3. gamma", "1. alpha\n2. beta"));
#endif

  // Vocab until the grammar separator is crossed; a name lookup has no separators at all, so its
  // single piece takes the kind the lookup itself resolved.
  StrId kind = resultSource != nullptr && strcmp(resultSource, "JMnedict") == 0 ? StrId::STR_DICT_KIND_NAME
                                                                                : StrId::STR_DICT_KIND_VOCAB;
  std::string grammarHead;  // the pattern named by the grammar heading, once it is seen
  // Cut at whichever separator comes first, repeatedly.
  size_t pos = 0;
  while (pos <= resultDefinition.size()) {
    const size_t entryAt = resultDefinition.find(kEntrySep, pos);
    const size_t grammarAt = resultDefinition.find(kGrammarSep, pos);
    const size_t cut = std::min(entryAt, grammarAt);
    const size_t end = cut == std::string::npos ? resultDefinition.size() : cut;
    std::string piece = resultDefinition.substr(pos, end - pos);

    // The grammar entry opens with its own "— Grammar: <pattern> —" heading. That pattern is
    // what the page is about, so it becomes the panel's headword and leaves the body.
    std::string head;
    static constexpr char kGrammarHead[] = "\xe2\x80\x94 Grammar: ";
    if (piece.compare(0, sizeof(kGrammarHead) - 1, kGrammarHead) == 0) {
      const size_t lineEnd = piece.find('\n');
      const size_t headEnd = lineEnd == std::string::npos ? piece.size() : lineEnd;
      head = piece.substr(sizeof(kGrammarHead) - 1, headEnd - (sizeof(kGrammarHead) - 1));
      // Trim the closing em dash and the space before it.
      const size_t dash = head.rfind("\xe2\x80\x94");
      if (dash != std::string::npos) head.erase(dash);
      const size_t tailSpace = head.find_last_not_of(" \t");
      head.erase(tailSpace == std::string::npos ? 0 : tailSpace + 1);
      piece.erase(0, lineEnd == std::string::npos ? piece.size() : lineEnd + 1);
      grammarHead = head;
    } else if (kind == StrId::STR_DICT_KIND_GRAMMAR) {
      // The heading is written once, but the grammar index can return several entries for that
      // one pattern, separated like any others. They are all about the pattern, so they keep its
      // name in the header rather than falling back to the surface word that was looked up.
      head = grammarHead;
    }

    // The attribution is the last non-empty line; it names the source, so it belongs in the
    // footer rather than dangling under the text.
    std::string label;
    size_t tail = piece.find_last_not_of("\n \t");
    if (tail != std::string::npos) {
      const size_t lineStart = piece.find_last_of('\n', tail);
      const size_t from = lineStart == std::string::npos ? 0 : lineStart + 1;
      const std::string lastLine = piece.substr(from, tail - from + 1);
      if (lastLine.find("JMdict") != std::string::npos || lastLine.find("JMnedict") != std::string::npos ||
          lastLine.find("Tatoeba") != std::string::npos) {
        label = lastLine;
        piece.erase(from);
      }
    }
    // Trim the blank lines the cut leaves behind so a page never opens on empty space.
    const size_t last = piece.find_last_not_of("\n \t");
    piece.erase(last == std::string::npos ? 0 : last + 1);
    if (!piece.empty()) {
      DefinitionText::EntryMetadata metadata;
      DefinitionText::extractEntryMetadata(piece, head.empty() ? resultHeadword : head, metadata);
      if (!metadata.source.empty()) label = std::move(metadata.source);
      if (sectionText.empty()) {
        if (metadata.reading.empty()) metadata.reading = resultReading;
        if (metadata.grammar.empty()) metadata.grammar = resultGrammar;
      }
      DefinitionText::formatEntryBody(piece, kind == StrId::STR_DICT_KIND_GRAMMAR ? head : std::string());

      bool duplicate = false;
      size_t replaceAt = sectionText.size();
      for (size_t i = 0; i < sectionText.size(); i++) {
        if (sectionText[i] == piece ||
            (kind == StrId::STR_DICT_KIND_NAME && sectionKind[i] == kind && coveredBy(piece, sectionText[i]))) {
          duplicate = true;
          break;
        }
        if (kind == StrId::STR_DICT_KIND_NAME && sectionKind[i] == kind && coveredBy(sectionText[i], piece)) {
          replaceAt = i;
          break;
        }
      }

      if (replaceAt < sectionText.size()) {
        if (label.empty()) label = std::move(sectionLabel[replaceAt]);
        if (metadata.reading.empty()) metadata.reading = std::move(sectionReading[replaceAt]);
        if (metadata.grammar.empty()) metadata.grammar = std::move(sectionGrammar[replaceAt]);
        if (head.empty()) head = std::move(sectionHead[replaceAt]);
        sectionText[replaceAt] = std::move(piece);
        sectionLabel[replaceAt] = std::move(label);
        sectionReading[replaceAt] = std::move(metadata.reading);
        sectionGrammar[replaceAt] = std::move(metadata.grammar);
        sectionKind[replaceAt] = kind;
        sectionHead[replaceAt] = std::move(head);
      } else if (!duplicate) {
        sectionText.push_back(std::move(piece));
        sectionLabel.push_back(std::move(label));
        sectionReading.push_back(std::move(metadata.reading));
        sectionGrammar.push_back(std::move(metadata.grammar));
        sectionKind.push_back(kind);
        sectionHead.push_back(std::move(head));
      }
    }

    if (cut == std::string::npos) break;
    if (cut == grammarAt) {
      kind = StrId::STR_DICT_KIND_GRAMMAR;  // everything from here on is the grammar entry
      // Keep the "— Grammar: <headword> —" heading with the grammar text it introduces.
      pos = cut + 2;  // step over the blank line, not the heading
      const size_t headingEnd = resultDefinition.find('\n', pos);
      if (headingEnd == std::string::npos) break;
    } else {
      pos = cut + sizeof(kEntrySep) - 1;
    }
  }
  // Free the merged copy: the pieces own the text now.
  if (!sectionText.empty()) {
    resultDefinition.clear();
    resultDefinition.shrink_to_fit();
  }
}

const std::string& EpubReaderWordLookupActivity::visibleDefinition() const {
  if (!sectionText.empty() && currentSection < static_cast<int>(sectionText.size())) {
    return sectionText[currentSection];
  }
  return resultDefinition;
}

const char* EpubReaderWordLookupActivity::visibleHeadword() const {
  if (!sectionHead.empty() && currentSection < static_cast<int>(sectionHead.size()) &&
      !sectionHead[currentSection].empty()) {
    return sectionHead[currentSection].c_str();
  }
  return resultHeadword.c_str();
}

const char* EpubReaderWordLookupActivity::visibleKind() const {
  if (sectionKind.empty() || currentSection >= static_cast<int>(sectionKind.size())) return nullptr;
  return I18N.get(sectionKind[currentSection]);
}

const char* EpubReaderWordLookupActivity::visibleReading() const {
  if (!sectionReading.empty() && currentSection < static_cast<int>(sectionReading.size()))
    return sectionReading[currentSection].c_str();
  return resultReading.c_str();
}

const char* EpubReaderWordLookupActivity::visibleGrammar() const {
  if (!sectionGrammar.empty() && currentSection < static_cast<int>(sectionGrammar.size()))
    return sectionGrammar[currentSection].c_str();
  return resultGrammar.c_str();
}

const char* EpubReaderWordLookupActivity::visibleLabel() const {
  if (!sectionLabel.empty() && currentSection < static_cast<int>(sectionLabel.size()) &&
      !sectionLabel[currentSection].empty()) {
    return sectionLabel[currentSection].c_str();
  }
  if (!resultDictionaryLabel.empty()) return resultDictionaryLabel.c_str();
  return resultSource;
}

void EpubReaderWordLookupActivity::renderContentArea(const Rect& body) {
  // Built-in font on purpose, NOT SETTINGS.getReaderFontId(): the lookup panel's definitions
  // and UI already render in built-in fonts, so an SD reader font (e.g. UD Digi Kyokasho) made
  // the headword a different typeface than the rest of the view -- and pulled whole SD font
  // groups (16KB decompression buffers each) into a heap that is already at its tightest here.
  const int defFont = DefinitionText::wordLookupFontId();
  const uint16_t defScale = DefinitionText::wordLookupFontScale();
  sdFontSystem.ensureWordLookupFallback(renderer, defFont, DefinitionText::wordLookupFontPointSize());

  // Bulk-load every glyph the headword + definition need before drawing/measuring any of them --
  // same fix, and same root cause, as the vertical-page-turn slowness fixed earlier this session.
  // Without this, dictionary definitions (which merge up to 5 entries and can run to hundreds of
  // characters spanning many different compressed font groups) fall through the slow one-by-one
  // glyph fallback path a character at a time. ONE prewarm per string: the FontDecompressor reuses
  // its 4 page-buffer slots WITHIN a call but not across calls, so per-line prewarming exhausts
  // them ("All 4 slots full") and is slower, not faster.
  if (hasResult) {
    if (auto* fcm = renderer.getFontCacheManager()) {
      fcm->clearCache();
      fcm->prewarmCache(defFont, resultHeadword.c_str(), 1 << EpdFontFamily::BOLD);
      renderer.prewarmText(defFont, visibleReading(), 1 << EpdFontFamily::REGULAR);
      renderer.prewarmText(defFont, visibleGrammar(), 1 << EpdFontFamily::REGULAR);
      // Prewarm only the ON-SCREEN slice of the definition, in ONE call. A merged 5-entry
      // definition can run to thousands of bytes, but only ~13 lines show; warming the whole
      // thing was the ~1s-per-step navigation cost (renders serialize on the RenderLock, so a
      // slow render stalls the next keypress). ~1KB covers a full screen of Latin OR CJK. Only
      // when scrollOffset==0 (navigating a new word); a scrolled view warms the whole definition
      // since its visible window is further in. ONE call, not per-line: the decompressor reuses
      // its 4 page-buffer slots within a call but not across calls.
      constexpr size_t kVisiblePrewarmBytes = 1024;
      const std::string& shown = visibleDefinition();
      if (scrollOffset == 0 && shown.size() > kVisiblePrewarmBytes) {
        size_t cut = kVisiblePrewarmBytes;  // back up to a UTF-8 lead byte so the last char is whole
        while (cut > 0 && (static_cast<unsigned char>(shown[cut]) & 0xC0) == 0x80) cut--;
        std::string head = shown.substr(0, cut);
        renderer.prewarmText(defFont, head.c_str(), (1 << EpdFontFamily::REGULAR) | (1 << EpdFontFamily::ITALIC));
      } else {
        renderer.prewarmText(defFont, shown.c_str(), (1 << EpdFontFamily::REGULAR) | (1 << EpdFontFamily::ITALIC));
      }
    }
  }

  if (scan.selectableGlyphs.empty() || !hasResult) {
    // "No match found" is only the truth once nothing is still in flight; during fast
    // navigation or while the progressive scan is still mapping the page, show Loading.
    const bool stillWorking = lookupInFlight || !scan.isDone();
    UITheme::drawCenteredText(renderer, body, UI_12_FONT_ID, body.y + body.height / 2,
                              stillWorking ? tr(STR_LOADING) : tr(STR_NO_MATCH), true);
  } else {
    DefinitionText::EntryMetadata metadata{visibleReading(), visibleGrammar()};
    const int defLineH = renderer.getLineHeightScaled(defFont, defScale);
    const int metadataLines = DefinitionText::entryMetadataLineCount(metadata);
    const int definitionScroll = std::max(0, scrollOffset - metadataLines);
    Rect definitionBody = body;
    const int metadataEndY =
        DefinitionText::drawEntryMetadata(renderer, body, defFont, defScale, metadata, scrollOffset, defLineH);
    definitionBody.y = metadataEndY - std::min(scrollOffset, metadataLines) * defLineH;
    definitionBody.height = std::max(0, body.y + body.height - definitionBody.y);

    const int maxWidth = definitionBody.width;
    const int textX = definitionBody.x;
    const int defY = definitionBody.y;

    const int maxDefY = definitionBody.y + definitionBody.height;
    const auto wrap = DefinitionText::drawWrapped(renderer, defFont, visibleDefinition(), textX, defY, defLineH,
                                                  maxWidth, maxDefY, definitionScroll, defScale);

    totalLines = metadataLines + wrap.totalLines;
    // Leave at least a screenful visible: max scroll = total - capacity
    visibleCapacity = std::max(1, body.height / defLineH);
    maxScroll = std::max(0, totalLines - visibleCapacity);
  }
}

void EpubReaderWordLookupActivity::render(RenderLock&&) {
  if (mode == Mode::Select) {
    renderSelect();
    return;
  }

  // Counter, right-aligned on the headword line. Paged mode counts pages of the definition;
  // free-scrolling mode keeps the word position within the page (35/50), whose total is
  // unknown until the progressive scan finishes -- an ellipsis stands in meanwhile.
  std::string counterText;
  if (hasResult && !scan.selectableGlyphs.empty()) {
    if (pagedDefinition()) {
      // Which source you are on, not which word on the page -- the word count belongs to the
      // selection view, and 1/834 says nothing about the entry you are reading.
      if (sectionText.size() > 1) {
        counterText = std::to_string(currentSection + 1) + "/" + std::to_string(sectionText.size());
      }
    } else {
      counterText = std::to_string(cursorIndex + 1) + "/" +
                    (scan.isDone() ? std::to_string(scan.selectableGlyphs.size()) : std::string("\xe2\x80\xa6"));
    }
  }

  if (hasResult) {
    // DictionaryPanel paints its fixed 12pt bold header before the body renderer runs. Warm that
    // exact face first; otherwise the first CJK lookup can resolve through the fallback after the
    // panel is already on screen, and only the next navigation appears in the right typeface.
    constexpr int kPanelHeaderFont = NOTOSERIF_12_FONT_ID;
    sdFontSystem.ensureWordLookupFallback(renderer, kPanelHeaderFont, 12);
    if (auto* fcm = renderer.getFontCacheManager()) {
      fcm->clearCache();
      fcm->prewarmCache(kPanelHeaderFont, visibleHeadword(), 1 << EpdFontFamily::BOLD);
    }
  }

  // The panel is opaque and always covers the same rectangle, so a re-render overwrites the
  // previous one; the reader's page stays visible around it instead of being cleared away.
  const auto layout = DictionaryPanel::draw(renderer, hasResult ? visibleHeadword() : "", visibleLabel(),
                                            counterText.empty() ? nullptr : counterText.c_str(), visibleKind());
  renderContentArea(layout.body);

  const bool sideButtonsForLookup =
      SETTINGS.wordLookupSideButtons != 0 && SETTINGS.sideButtonLayout != CrossPointSettings::SIDE_BUTTONS_DISABLED;
  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), sideButtonsForLookup ? tr(STR_DIR_UP) : tr(STR_DIR_LEFT),
                            sideButtonsForLookup ? tr(STR_DIR_DOWN) : tr(STR_DIR_RIGHT));
  DictionaryPanel::clearButtonHints(renderer);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (!initialRenderDone) {
    renderer.displayBuffer();
    initialRenderDone = true;
    fastRefreshCount = 0;
  } else {
    fastRefreshCount++;
    if (fastRefreshCount >= kFullRefreshInterval) {
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      fastRefreshCount = 0;
    } else {
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    }
  }

  // The framebuffer owns the finished pixels; keeping the decompressed glyph slab until the
  // next keypress only fragments the heap while the dictionary caches are resident.
  if (auto* fcm = renderer.getFontCacheManager()) fcm->releaseAllFontMemory();
}
