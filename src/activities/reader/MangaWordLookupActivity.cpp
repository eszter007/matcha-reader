#include "MangaWordLookupActivity.h"

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
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

MangaWordLookupActivity::MangaWordLookupActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                 std::string panelText, std::string scanCachePath,
                                                 const uint16_t pageIndex, const uint16_t panelIndex)
    : Activity("MangaWordLookup", renderer, mappedInput),
      scanCachePath(std::move(scanCachePath)),
      scanPage(pageIndex),
      scanPanel(panelIndex) {
  scan.initFromUtf8Text(panelText);
  initScanFromCacheOrBurst();
}

// A persisted scan for this exact text skips all scanning; otherwise start progressively.
void MangaWordLookupActivity::initScanFromCacheOrBurst() {
  // Self-heal fragmentation before the lookup allocates its scan vectors and dict caches.
  // Device telemetry showed maxAlloc degrading monotonically across open/close cycles
  // (28.7K -> 22.5K -> 19.4K ...) while total free fully recovered: font hot-group/slab
  // buffers regrown while RENDERING definitions persist past onExit and split the large
  // block the dict caches vacate. Left unchecked this ends in an allocation abort() a few
  // pages later (confirmed crash_report). Freeing font memory coalesces the heap; fonts
  // reload lazily, and the reader re-warms its caches on return anyway.
  if (ESP.getMaxAllocHeap() < 28 * 1024) {
    LOG_INF("MWLA", "Low contiguous heap (maxAlloc=%u); releasing font caches", ESP.getMaxAllocHeap());
    if (auto* fcm = renderer.getFontCacheManager()) {
      fcm->releaseAllFontMemory();
      LOG_INF("MWLA", "After font release: maxAlloc=%u", ESP.getMaxAllocHeap());
    }
  }
  if (!scanCachePath.empty() && scan.tryLoadCache(scanCachePath, scanPage, scanPanel)) {
    return;
  }
  runInitialBurst();
}

// Progressive open: scan only far enough to find the FIRST selectable word; the rest of the
// text is mapped incrementally from loop(). Manga texts are short, so this usually completes
// in well under a second anyway -- the burst just guarantees a fast open on long combined text.
void MangaWordLookupActivity::runInitialBurst() {
  const uint32_t scanStart = millis();
  LOG_INF("MWLA", "progressive scan: %u characters", static_cast<unsigned>(scan.allGlyphs.size()));
  while (!scan.isDone() && scan.selectableGlyphs.empty() && millis() - scanStart < 1500) {
    scan.step(50);
  }
  LOG_INF("MWLA", "progressive scan: first word after %u ms", millis() - scanStart);
}

void MangaWordLookupActivity::onEnter() {
  Activity::onEnter();
  // Heap telemetry for the word-lookup OOM crash hunt -- see EpubReaderWordLookupActivity.
  LOG_INF("MWLA", "onEnter heap: free=%u maxAlloc=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  // A scan-cache hit remembers the last cursor position for this exact panel/page text -- see
  // EpubReaderWordLookupActivity::onEnter().
  if (scan.restoredCursorIndex != WordSelectionScan::kNoRestoredCursor &&
      scan.restoredCursorIndex < scan.selectableGlyphs.size()) {
    cursorIndex = scan.restoredCursorIndex;
    performLookup();
    requestUpdate();
    return;
  }
  const int maxIdx = static_cast<int>(scan.selectableGlyphs.size()) - 1;
  for (cursorIndex = 0; cursorIndex <= maxIdx; cursorIndex++) {
    performLookup();
    if (hasResult) break;
  }
  if (cursorIndex > maxIdx) cursorIndex = 0;
  requestUpdate();
}

void MangaWordLookupActivity::onExit() {
  LOG_INF("MWLA", "onExit heap: free=%u maxAlloc=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  // Persist the current cursor position -- see EpubReaderWordLookupActivity::onExit().
  if (!scanCachePath.empty()) {
    scan.saveCache(scanCachePath, scanPage, scanPanel, static_cast<uint16_t>(cursorIndex));
  }
  // Return the dictionary cache memory (~30KB) to the pool -- see EpubReaderWordLookupActivity.
  DictIndex::releaseCaches();
  Activity::onExit();
}

void MangaWordLookupActivity::moveCursor(int delta) {
  // Moving past the last already-discovered word while the background scan is still running:
  // scan forward just enough to reveal the next one (see EpubReaderWordLookupActivity).
  if (delta > 0 && !scan.isDone() && cursorIndex + delta >= static_cast<int>(scan.selectableGlyphs.size())) {
    const size_t want = static_cast<size_t>(cursorIndex + delta) + 1;
    while (!scan.isDone() && scan.selectableGlyphs.size() < want) {
      scan.step(50);
    }
  }
  if (scan.selectableGlyphs.empty()) return;
  const int maxIdx = static_cast<int>(scan.selectableGlyphs.size()) - 1;
  // scan.selectableGlyphs is already the pre-filtered list of positions buildSelectableGlyphs()
  // confirmed have a dictionary match -- every index in it is valid by construction. Same fix as
  // EpubReaderWordLookupActivity::moveCursor(): the previous retry-skip loop re-validated via a
  // fresh performLookup() and kept advancing past any position that didn't independently agree,
  // silently skipping entries ("every second entry is skipped" during navigation).
  int newIndex = cursorIndex + delta;
  if (scan.isDone()) {
    // Full text mapped -- cycle past the ends instead of dead-ending (see EPUB activity).
    if (newIndex < 0) newIndex = maxIdx;
    else if (newIndex > maxIdx) newIndex = 0;
  } else {
    if (newIndex < 0) newIndex = 0;
    if (newIndex > maxIdx) newIndex = maxIdx;
  }
  cursorIndex = newIndex;
  performLookup();
}

std::string MangaWordLookupActivity::buildLookupText(size_t startIdx) const {
  std::string text;
  if (startIdx >= scan.selectToAllIdx.size()) return text;

  const size_t allStart = scan.selectToAllIdx[startIdx];
  int charCount = 0;
  for (size_t i = allStart; i < scan.allGlyphs.size() && charCount < WordSelectionScan::kMaxLookupChars; i++) {
    WordSelectionScan::encodeUtf8(scan.allGlyphs[i].codepoint, text);
    charCount++;
  }
  return text;
}

void MangaWordLookupActivity::performLookup() {
  // Serialize against the render task, which reads the result strings concurrently -- see
  // EpubReaderWordLookupActivity::performLookup() for the confirmed tear/abort.
  RenderLock lock;
  // Render shows "Loading..." instead of "No match found" while this runs (fast navigation).
  lookupInFlight = true;
  performLookupImpl();
  lookupInFlight = false;
}

void MangaWordLookupActivity::performLookupImpl() {
  hasResult = false;
  resultHeadword.clear();
  resultDefinition.clear();
  resultMatchLen = 0;
  scrollOffset = 0;
  totalLines = 9999;

  std::string text = buildLookupText(static_cast<size_t>(cursorIndex));
  if (text.empty()) return;

  WordLookupResult result;
  if (WordLookup::lookup(text, 0, result)) {
    WordSelectionScan::stripTrailingParticle(text, result);
    hasResult = true;
    resultHeadword = result.entry.headword;
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

    // Check grammar dictionary for short hiragana matches
    if (chars <= 3 && Storage.exists(DictIndex::GRAMMAR_IDX_PATH)) {
      bool allHiragana = true;
      for (size_t b = 0; b < result.matchLength && b < text.size();) {
        auto c = static_cast<unsigned char>(text[b]);
        uint32_t cp2 = 0;
        if (c < 0x80) { cp2 = c; b += 1; }
        else if ((c & 0xE0) == 0xC0) { cp2 = ((c & 0x1F) << 6) | (text[b+1] & 0x3F); b += 2; }
        else if ((c & 0xF0) == 0xE0) { cp2 = ((c & 0x0F) << 12) | ((text[b+1] & 0x3F) << 6) | (text[b+2] & 0x3F); b += 3; }
        else { b += 4; }
        if (cp2 < 0x3040 || cp2 > 0x309F) { allHiragana = false; break; }
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

  // Grammar scan
  // Nicety on top of the main result; skip under memory pressure instead of risking an
  // allocation abort() -- see EpubReaderWordLookupActivity's grammar scan.
  if (ESP.getMaxAllocHeap() < 16 * 1024) {
    LOG_ERR("MWLA", "Skipping grammar scan, heap too low (maxAlloc=%u)", ESP.getMaxAllocHeap());
  } else if (Storage.exists(DictIndex::GRAMMAR_IDX_PATH) && cursorIndex < static_cast<int>(scan.selectToAllIdx.size())) {
    const size_t allStart = scan.selectToAllIdx[cursorIndex];
    int bestGramLen = 0;
    std::string bestGramHw, bestGramDef;

    for (int backoff = 3; backoff >= 0; backoff--) {
      size_t scanStart = allStart;
      for (int b2 = 0; b2 < backoff && scanStart > 0; b2++) scanStart--;

      std::string gramText;
      int gCharCount = 0;
      for (size_t j = scanStart; j < scan.allGlyphs.size() && gCharCount < 12; j++) {
        WordSelectionScan::encodeUtf8(scan.allGlyphs[j].codepoint, gramText);
        gCharCount++;
      }

      for (int wLen = std::min(gCharCount, 10); wLen >= 2; wLen--) {
        size_t byteEnd = 0;
        int cnt = 0;
        for (size_t b2 = 0; b2 < gramText.size() && cnt < wLen; cnt++) {
          auto c = static_cast<unsigned char>(gramText[b2]);
          if (c < 0x80) b2 += 1;
          else if ((c & 0xE0) == 0xC0) b2 += 2;
          else if ((c & 0xF0) == 0xE0) b2 += 3;
          else b2 += 4;
          byteEnd = b2;
        }
        std::string window = gramText.substr(0, byteEnd);
        DictEntry gramEntry;
        if (DictIndex::lookupInFile(window.c_str(), DictIndex::GRAMMAR_IDX_PATH,
                                     DictIndex::GRAMMAR_DAT_PATH, gramEntry)) {
          if (gramEntry.headword != resultHeadword && wLen > bestGramLen) {
            bestGramLen = wLen;
            bestGramHw = std::move(gramEntry.headword);
            bestGramDef = std::move(gramEntry.definition);
          }
          break;
        }
      }
    }

    if (bestGramLen > 0) {
      // Guarded reserve + appends instead of a temporary chain -- see the EPUB activity.
      const size_t mergedLen = resultDefinition.size() + bestGramHw.size() + bestGramDef.size() + 32;
      if (ESP.getMaxAllocHeap() > mergedLen + 8 * 1024) {
        resultDefinition.reserve(mergedLen);
        resultDefinition += "\n\n— Grammar: ";
        resultDefinition += bestGramHw;
        resultDefinition += " —\n";
        resultDefinition += bestGramDef;
      } else {
        LOG_ERR("MWLA", "Skipping grammar merge, heap too low (maxAlloc=%u)", ESP.getMaxAllocHeap());
      }
    }
  }

  requestUpdate();
}

void MangaWordLookupActivity::loop() {
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

  // Progressive background scan + one-time result persistence -- same structure as
  // EpubReaderWordLookupActivity::loop(), see there for the full rationale.
  if (!scan.isDone()) {
    const bool done = scan.step(40);
    if (!hasResult && !scan.selectableGlyphs.empty()) {
      performLookup();
      requestUpdate();
    }
    if (done) {
      DictIndex::logAndResetStats("progressive scan complete");
      requestUpdate();
    }
  }
}

void MangaWordLookupActivity::renderContentArea(const Rect& screen, int contentTop) {
  auto metrics = UITheme::getInstance().getMetrics();
  // Built-in font on purpose, NOT SETTINGS.getReaderFontId(): the lookup panel's definitions
  // and UI already render in built-in fonts, so an SD reader font (e.g. UD Digi Kyokasho) made
  // the headword a different typeface than the rest of the view -- and pulled whole SD font
  // groups (16KB decompression buffers each) into a heap that is already at its tightest here.
  const int jaFont = NOTOSERIF_16_FONT_ID;

  // Bulk-load every glyph the headword + definition need before drawing/measuring any of them.
  // Without this, each drawText()/getTextWidth() call for a character that isn't already cached
  // falls through the slow one-by-one fallback path -- confirmed on a real device as 12-18 SECOND
  // render times for a single Word Lookup screen (same root cause as the vertical-page-turn
  // slowness fixed earlier: dictionary definitions merge up to 5 entries and can run to hundreds
  // of characters spanning many different compressed font groups, each a cache miss without this).
  if (hasResult) {
    if (auto* fcm = renderer.getFontCacheManager()) {
      fcm->clearCache();
      // Scoped to the exact style each is drawn with below (headword: BOLD; definition: REGULAR)
      // -- a blanket "all 4 styles" request can itself exhaust the font decompressor's 4-slot
      // page buffer (each style, plus one more per style if the font has a fallback), same trap
      // found and fixed for vertical-page rendering earlier this session.
      fcm->prewarmCache(jaFont, resultHeadword.c_str(), 1 << EpdFontFamily::BOLD);
      fcm->prewarmCache(SMALL_FONT_ID, resultDefinition.c_str(), 1 << EpdFontFamily::REGULAR);
    }
  }

  if (scan.selectableGlyphs.empty() || !hasResult) {
    const bool stillWorking = lookupInFlight || !scan.isDone();
    UITheme::drawCenteredText(renderer, screen, UI_12_FONT_ID,
                              screen.y + screen.height / 2, stillWorking ? tr(STR_LOADING) : tr(STR_NO_MATCH), true);
    return;
  }

  const int maxWidth = screen.width - metrics.contentSidePadding * 2;
  const int textX = screen.x + metrics.contentSidePadding;
  int defY;

  if (scrollOffset == 0) {
    renderer.drawText(jaFont, textX, contentTop, resultHeadword.c_str(), true, EpdFontFamily::BOLD);
    defY = contentTop + renderer.getLineHeight(jaFont) + metrics.verticalSpacing;
  } else {
    std::string scrollInfo = resultHeadword + " \xe2\x96\xb2";
    renderer.drawText(SMALL_FONT_ID, textX, contentTop, scrollInfo.c_str(), true);
    defY = contentTop + renderer.getLineHeight(SMALL_FONT_ID) + 4;
  }

  const int defFont = SMALL_FONT_ID;
  const int defLineH = renderer.getLineHeight(defFont);
  const int maxDefY = screen.y + screen.height - 2;
  const int firstDefY = defY;
  const auto wrap = DefinitionText::drawWrapped(renderer, defFont, resultDefinition, textX, defY, defLineH,
                                                maxWidth, maxDefY, scrollOffset);

  totalLines = wrap.totalLines;
  const int visibleCapacity = (maxDefY - firstDefY) / defLineH;
  maxScroll = std::max(0, totalLines - visibleCapacity);
}

void MangaWordLookupActivity::render(RenderLock&&) {
  auto& theme = UITheme::getInstance();
  auto metrics = theme.getMetrics();
  Rect screen = theme.getScreenSafeArea(renderer, true, false);

  const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  std::string posText;
  if (hasResult && !scan.selectableGlyphs.empty()) {
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
    const int physBottom = renderer.getScreenHeight();
    renderer.fillRect(0, contentTop, renderer.getScreenWidth(), physBottom - contentTop, false);
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
