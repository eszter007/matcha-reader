#include "EpubReaderWordLookupActivity.h"

#include "DefinitionTextRenderer.h"

#include <Arduino.h>
#include <DictIndex.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WordLookup.h>

#include "CrossPointSettings.h"
#include "Epub/Page.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

EpubReaderWordLookupActivity::EpubReaderWordLookupActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                           const VerticalPage& page, std::string scanCachePath,
                                                           const uint16_t spineIndex, const uint16_t pageIndex)
    : Activity("WordLookup", renderer, mappedInput),
      scanCachePath(std::move(scanCachePath)),
      scanSpine(spineIndex),
      scanPage(pageIndex) {
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
  while (!scan.isDone() && scan.selectableGlyphs.empty() && millis() - scanStart < 1500) {
    stepScan(50);
  }
  LOG_INF("WLA", "progressive scan (%s): first word after %u ms", label, millis() - scanStart);
}

// See the header: heal a low-heap-truncated scan once by freeing fonts and re-walking the intact
// glyph list. cursorIndex is intentionally left alone -- the rebuilt selectable list only grows,
// and every caller already guards against an out-of-range cursor while it refills, so the user's
// position resumes naturally once the rescan passes it again.
bool EpubReaderWordLookupActivity::stepScan(uint32_t budgetMs) {
  const bool done = scan.step(budgetMs);
  if (scan.wasTruncated() && !scanHealAttempted && !scan.allGlyphs.empty()) {
    scanHealAttempted = true;
    LOG_INF("WLA", "Scan truncated by low heap; releasing fonts and rescanning (maxAlloc=%u)",
            ESP.getMaxAllocHeap());
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
  if (scan.restoredCursorIndex != WordSelectionScan::kNoRestoredCursor &&
      scan.restoredCursorIndex < scan.selectableGlyphs.size()) {
    cursorIndex = scan.restoredCursorIndex;
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
    if (newIndex < 0) newIndex = maxIdx;
    else if (newIndex > maxIdx) newIndex = 0;
  } else {
    // Background scan still running: "the end" isn't final yet, so clamp instead of cycling --
    // wrapping to word one here would be surprising and skip words not yet discovered.
    if (newIndex < 0) newIndex = 0;
    if (newIndex > maxIdx) newIndex = maxIdx;
  }
  cursorIndex = newIndex;
  performLookup();
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
      } else if (c == 0xEF && b + 2 < text.size() &&
                 static_cast<unsigned char>(text[b + 1]) == 0xBC &&
                 static_cast<unsigned char>(text[b + 2]) >= 0x90 &&
                 static_cast<unsigned char>(text[b + 2]) <= 0x99) {
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
        if (c < 0x80) pos += 1;
        else if ((c & 0xE0) == 0xC0) pos += 2;
        else if ((c & 0xF0) == 0xE0) pos += 3;
        else pos += 4;
        nrChars++;
      }
    }
    if (static_cast<int>(nameRun) > nrChars) {
      size_t nb = 0;
      int nc = 0;
      while (nb < text.size() && nc < static_cast<int>(nameRun)) {
        auto c = static_cast<unsigned char>(text[nb]);
        if (c < 0x80) nb += 1;
        else if ((c & 0xE0) == 0xC0) nb += 2;
        else if ((c & 0xF0) == 0xE0) nb += 3;
        else nb += 4;
        nc++;
      }
      hasResult = true;
      resultHeadword = digitPrefix + text.substr(0, nb);
      resultDefinition = tr(STR_LOOKUP_NAME);  // no dictionary entry -- label it as a name
      resultMatchLen = static_cast<int>(nameRun);
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
    int chars = 0;
    size_t pos = 0;
    while (pos < result.matchLength && pos < text.size()) {
      auto c = static_cast<unsigned char>(text[pos]);
      if (c < 0x80) pos += 1;
      else if ((c & 0xE0) == 0xC0) pos += 2;
      else if ((c & 0xF0) == 0xE0) pos += 3;
      else pos += 4;
      chars++;
    }
    resultMatchLen = chars;

    // For short hiragana-only matches (≤3 chars), check if the grammar dict
    // has a better entry and promote it to the main result. Functional words
    // like こと, もの, よう get unhelpful JMdict hits ("ancient capital").
    if (chars <= 3 && Storage.exists(DictIndex::GRAMMAR_IDX_PATH)) {
      bool allHiragana = true;
      for (size_t b = 0; b < result.matchLength && b < text.size();) {
        auto c = static_cast<unsigned char>(text[b]);
        uint32_t cp = 0;
        if (c < 0x80) { cp = c; b += 1; }
        else if ((c & 0xE0) == 0xC0) { cp = ((c & 0x1F) << 6) | (text[b+1] & 0x3F); b += 2; }
        else if ((c & 0xF0) == 0xE0) { cp = ((c & 0x0F) << 12) | ((text[b+1] & 0x3F) << 6) | (text[b+2] & 0x3F); b += 3; }
        else { b += 4; }
        if (cp < 0x3040 || cp > 0x309F) { allHiragana = false; break; }
      }
      if (allHiragana) {
        DictEntry gramEntry;
        if (DictIndex::lookupInFile(resultHeadword.c_str(), DictIndex::GRAMMAR_IDX_PATH,
                                     DictIndex::GRAMMAR_DAT_PATH, gramEntry)) {
          resultDefinition = std::move(gramEntry.definition);
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
  } else if (Storage.exists(DictIndex::GRAMMAR_IDX_PATH)) {
    const size_t allStart = scan.selectToAllIdx[static_cast<size_t>(cursorIndex)];
    const uint32_t paraIdx = scan.allGlyphs[allStart].paragraphIndex;

    // Try starting positions: cursor-3, cursor-2, cursor-1, cursor
    int bestGramLen = 0;
    for (int backoff = 3; backoff >= 0; backoff--) {
      size_t scanStart = allStart;
      for (int b = 0; b < backoff && scanStart > 0; b++) {
        scanStart--;
        if (scan.allGlyphs[scanStart].paragraphIndex != paraIdx) { scanStart++; break; }
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
          if (c < 0x80) b += 1;
          else if ((c & 0xE0) == 0xC0) b += 2;
          else if ((c & 0xF0) == 0xE0) b += 3;
          else b += 4;
          byteEnd = b;
        }
        std::string window = gramText.substr(0, byteEnd);
        DictEntry gramEntry;
        if (DictIndex::lookupInFile(window.c_str(), DictIndex::GRAMMAR_IDX_PATH,
                                     DictIndex::GRAMMAR_DAT_PATH, gramEntry)) {
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
    const size_t mergedLen =
        resultDefinition.size() + grammarHeadword.size() + grammarDefinition.size() + 32;
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

  requestUpdate();
}

void EpubReaderWordLookupActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    performLookup();
    return;
  }

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [this] { moveCursor(1); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [this] { moveCursor(-1); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down}, [this] {
    if (hasResult && scrollOffset < maxScroll) { scrollOffset = std::min(maxScroll, scrollOffset + 5); requestUpdate(); }
  });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up}, [this] {
    if (scrollOffset > 0) { scrollOffset = std::max(0, scrollOffset - 5); requestUpdate(); }
  });

  // Progressive background scan: keep mapping the page's selectable words in small slices
  // between input polls (skipLoopDelay() holds full CPU and fast ticks while this runs, so a
  // slice never swallows a button press). Everything here runs on the main task -- the render
  // task only ever reads counter sizes -- so no lock is needed and, unlike the abandoned
  // reader-idle precompute, nothing can starve another activity's rendering.
  if (!scan.isDone()) {
    const bool done = stepScan(40);
    // The open can show "No match" if the initial burst found nothing yet -- promote the first
    // word as soon as the background scan discovers it.
    if (!hasResult && !scan.selectableGlyphs.empty()) {
      performLookup();
      requestUpdate();
    }
    if (done) {
      DictIndex::logAndResetStats("progressive scan complete");
      requestUpdate();  // redraw the position counter with the final total
    }
  }
}

void EpubReaderWordLookupActivity::renderContentArea(const Rect& screen, int contentTop) {
  auto metrics = UITheme::getInstance().getMetrics();
  // Built-in font on purpose, NOT SETTINGS.getReaderFontId(): the lookup panel's definitions
  // and UI already render in built-in fonts, so an SD reader font (e.g. UD Digi Kyokasho) made
  // the headword a different typeface than the rest of the view -- and pulled whole SD font
  // groups (16KB decompression buffers each) into a heap that is already at its tightest here.
  const int jaFont = NOTOSERIF_16_FONT_ID;

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
      fcm->prewarmCache(jaFont, resultHeadword.c_str(), 1 << EpdFontFamily::BOLD);
      // Prewarm only the ON-SCREEN slice of the definition, in ONE call. A merged 5-entry
      // definition can run to thousands of bytes, but only ~13 lines show; warming the whole
      // thing was the ~1s-per-step navigation cost (renders serialize on the RenderLock, so a
      // slow render stalls the next keypress). ~1KB covers a full screen of Latin OR CJK. Only
      // when scrollOffset==0 (navigating a new word); a scrolled view warms the whole definition
      // since its visible window is further in. ONE call, not per-line: the decompressor reuses
      // its 4 page-buffer slots within a call but not across calls.
      constexpr size_t kVisiblePrewarmBytes = 1024;
      if (scrollOffset == 0 && resultDefinition.size() > kVisiblePrewarmBytes) {
        size_t cut = kVisiblePrewarmBytes;  // back up to a UTF-8 lead byte so the last char is whole
        while (cut > 0 && (static_cast<unsigned char>(resultDefinition[cut]) & 0xC0) == 0x80) cut--;
        const char saved = resultDefinition[cut];
        resultDefinition[cut] = '\0';  // safe: render task holds the lock, sole accessor here
        fcm->prewarmCache(SMALL_FONT_ID, resultDefinition.c_str(), 1 << EpdFontFamily::REGULAR);
        resultDefinition[cut] = saved;
      } else {
        fcm->prewarmCache(SMALL_FONT_ID, resultDefinition.c_str(), 1 << EpdFontFamily::REGULAR);
      }
    }
  }

  if (scan.selectableGlyphs.empty() || !hasResult) {
    // "No match found" is only the truth once nothing is still in flight; during fast
    // navigation or while the progressive scan is still mapping the page, show Loading.
    const bool stillWorking = lookupInFlight || !scan.isDone();
    UITheme::drawCenteredText(renderer, screen, UI_12_FONT_ID,
                              screen.y + screen.height / 2, stillWorking ? tr(STR_LOADING) : tr(STR_NO_MATCH), true);
  } else {
    const int maxWidth = screen.width - metrics.contentSidePadding * 2;
    const int textX = screen.x + metrics.contentSidePadding;

    int defY;

    if (scrollOffset == 0) {
      // Headword at the top (position counter is now in the header).
      renderer.drawText(jaFont, textX, contentTop, resultHeadword.c_str(), true, EpdFontFamily::BOLD);
      defY = contentTop + renderer.getLineHeight(jaFont) + metrics.verticalSpacing;
    } else {
      // When scrolled, show a compact header line with the headword + scroll mark.
      std::string scrollInfo = resultHeadword + " \xe2\x96\xb2";
      renderer.drawText(SMALL_FONT_ID, textX, contentTop, scrollInfo.c_str(), true);
      defY = contentTop + renderer.getLineHeight(SMALL_FONT_ID) + 4;
    }

    const int defFont = SMALL_FONT_ID;
    const int defLineH = renderer.getLineHeight(defFont);
    // screen.height already excludes the button-hints band, so its bottom edge
    // is the top of the buttons; stay a hair above it.
    const int maxDefY = screen.y + screen.height - 2;
    const int firstDefY = defY;
    const auto wrap = DefinitionText::drawWrapped(renderer, defFont, resultDefinition, textX, defY, defLineH,
                                                  maxWidth, maxDefY, scrollOffset);

    totalLines = wrap.totalLines;
    // Leave at least a screenful visible: max scroll = total - capacity
    const int visibleCapacity = (maxDefY - firstDefY) / defLineH;
    maxScroll = std::max(0, totalLines - visibleCapacity);
  }
}

void EpubReaderWordLookupActivity::render(RenderLock&&) {
  auto& theme = UITheme::getInstance();
  auto metrics = theme.getMetrics();
  Rect screen = theme.getScreenSafeArea(renderer, true, false);

  const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int footerHeight = renderer.getLineHeight(SMALL_FONT_ID) + metrics.verticalSpacing;
  const int contentBottom = screen.y + screen.height - footerHeight;

  // Position counter (35/50) shown right-aligned on the header baseline.
  std::string posText;
  if (hasResult && !scan.selectableGlyphs.empty()) {
    // Total is unknown until the progressive scan finishes; show an ellipsis meanwhile.
    posText = std::to_string(cursorIndex + 1) + "/" +
              (scan.isDone() ? std::to_string(scan.selectableGlyphs.size()) : std::string("\xe2\x80\xa6"));
  }
  const Rect headerRect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight};

  if (!initialRenderDone) {
    renderer.clearScreen();

    GUI.drawHeader(renderer, headerRect, tr(STR_WORD_LOOKUP), posText.empty() ? nullptr : posText.c_str());

    renderContentArea(screen, contentTop);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

    renderer.displayBuffer();
    initialRenderDone = true;
    fastRefreshCount = 0;
  } else {
    // Clear from content top all the way to the physical bottom (including the
    // button-hint band margins, which screen.height excludes), then redraw hints.
    const int physBottom = renderer.getScreenHeight();
    renderer.fillRect(0, contentTop, renderer.getScreenWidth(), physBottom - contentTop, false);
    // Redraw the header so the position counter updates (drawHeader clears it).
    GUI.drawHeader(renderer, headerRect, tr(STR_WORD_LOOKUP), posText.empty() ? nullptr : posText.c_str());
    const auto labels2 = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
    GUI.drawButtonHints(renderer, labels2.btn1, labels2.btn2, labels2.btn3, labels2.btn4);

    renderContentArea(screen, contentTop);

    fastRefreshCount++;
    if (fastRefreshCount >= kFullRefreshInterval) {
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      fastRefreshCount = 0;
    } else {
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    }
  }
}
