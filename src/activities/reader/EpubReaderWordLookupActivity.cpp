#include "EpubReaderWordLookupActivity.h"

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
                                                           const VerticalPage& page)
    : Activity("WordLookup", renderer, mappedInput) {
  scan.initFromVerticalPage(page);
  runInitialBurst("vertical");
}

EpubReaderWordLookupActivity::EpubReaderWordLookupActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                           const Page& page)
    : Activity("WordLookup", renderer, mappedInput) {
  scan.initFromPage(page);
  runInitialBurst("horizontal");
}

// Progressive open: scan only far enough to find the FIRST selectable word so the panel can show
// a definition within a few hundred ms. The rest of the page is mapped incrementally from loop()
// while the user reads (see there); moveCursor() scans further on demand if the user outruns it.
// The cap bounds the open even on a pathological page with no early match.
void EpubReaderWordLookupActivity::runInitialBurst(const char* label) {
  const uint32_t scanStart = millis();
  LOG_INF("WLA", "progressive scan (%s): %u characters", label, static_cast<unsigned>(scan.allGlyphs.size()));
  while (!scan.isDone() && scan.selectableGlyphs.empty() && millis() - scanStart < 1500) {
    scan.step(50);
  }
  LOG_INF("WLA", "progressive scan (%s): first word after %u ms", label, millis() - scanStart);
}

void EpubReaderWordLookupActivity::onEnter() {
  Activity::onEnter();
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
      scan.step(50);
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
  cursorIndex += delta;
  if (cursorIndex < 0) cursorIndex = 0;
  if (cursorIndex > maxIdx) cursorIndex = maxIdx;
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
  if (Storage.exists(DictIndex::GRAMMAR_IDX_PATH)) {
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
    resultDefinition += "\n\n— Grammar: " + grammarHeadword + " —\n" + grammarDefinition;
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
    const bool done = scan.step(40);
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
  const int jaFont = SETTINGS.getReaderFontId();

  // Bulk-load every glyph the headword + definition need before drawing/measuring any of them --
  // same fix, and same root cause, as the vertical-page-turn slowness fixed earlier this session.
  // Without this, dictionary definitions (which merge up to 5 entries and can run to hundreds of
  // characters spanning many different compressed font groups) fall through the slow one-by-one
  // glyph fallback path a character at a time.
  if (hasResult) {
    if (auto* fcm = renderer.getFontCacheManager()) {
      fcm->clearCache();
      fcm->prewarmCache(jaFont, resultHeadword.c_str(), 1 << EpdFontFamily::BOLD);
      fcm->prewarmCache(SMALL_FONT_ID, resultDefinition.c_str(), 1 << EpdFontFamily::REGULAR);
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
    int linesDrawn = 0;
    int lineIndex = 0;
    // screen.height already excludes the button-hints band, so its bottom edge
    // is the top of the buttons; stay a hair above it.
    const int maxDefY = screen.y + screen.height - 2;
    const int firstDefY = defY;
    const int kMaxDefLines = 999;
    std::string defText = resultDefinition;
    size_t nlPos = 0;
    while (nlPos <= defText.size() && linesDrawn < kMaxDefLines) {
      size_t nextNl = defText.find('\n', nlPos);
      std::string paragraph = (nextNl == std::string::npos)
          ? defText.substr(nlPos) : defText.substr(nlPos, nextNl - nlPos);
      nlPos = (nextNl == std::string::npos) ? defText.size() + 1 : nextNl + 1;

      if (paragraph.empty()) {
        lineIndex++;
        if (lineIndex > scrollOffset) defY += defLineH / 2;
        continue;
      }
      // Try space-based wrapping first (for Latin text), then fall back to
      // character-level wrapping (for CJK text without spaces).
      std::string rem = paragraph;
      while (!rem.empty() && linesDrawn < kMaxDefLines) {
        if (renderer.getTextWidth(defFont, rem.c_str()) <= maxWidth) {
          lineIndex++;
          if (lineIndex > scrollOffset && defY + defLineH <= maxDefY) {
            renderer.drawText(defFont, textX, defY, rem.c_str(), true);
            defY += defLineH;
            linesDrawn++;
          }
          break;
        }
        // Try to break at the last space that fits
        std::string bestLine;
        size_t lastSpaceBreak = std::string::npos;
        std::string accum;
        const char* p = rem.c_str();
        while (*p) {
          size_t charLen = 1;
          auto c0 = static_cast<unsigned char>(*p);
          if (c0 >= 0xF0) charLen = 4;
          else if (c0 >= 0xE0) charLen = 3;
          else if (c0 >= 0xC0) charLen = 2;
          std::string test = accum + std::string(p, charLen);
          if (renderer.getTextWidth(defFont, test.c_str()) > maxWidth) {
            // Never orphan sentence-ending punctuation on its own line
            uint32_t nextCp = 0;
            if (charLen == 3) nextCp = ((c0 & 0x0F) << 12) | ((static_cast<unsigned char>(p[1]) & 0x3F) << 6) | (static_cast<unsigned char>(p[2]) & 0x3F);
            else if (charLen == 1) nextCp = c0;
            if (nextCp == 0x3002 || nextCp == 0x3001 || nextCp == 0xFF01 || nextCp == 0xFF1F ||
                nextCp == '.' || nextCp == ',' || nextCp == '!' || nextCp == '?') {
              accum = test;
              p += charLen;
            }
            break;
          }
          accum = test;
          if (*p == ' ') lastSpaceBreak = accum.size();
          p += charLen;
        }
        if (accum.empty()) {
          // Single char wider than maxWidth — force it
          auto c0 = static_cast<unsigned char>(rem[0]);
          size_t cl = 1;
          if (c0 >= 0xF0) cl = 4; else if (c0 >= 0xE0) cl = 3; else if (c0 >= 0xC0) cl = 2;
          accum = rem.substr(0, cl);
          rem = rem.substr(cl);
        } else if (lastSpaceBreak != std::string::npos && lastSpaceBreak > 0) {
          // Break at last space to keep Latin words intact
          std::string line = accum.substr(0, lastSpaceBreak);
          rem = rem.substr(lastSpaceBreak);
          // Skip leading space
          if (!rem.empty() && rem[0] == ' ') rem = rem.substr(1);
          accum = line;
        } else {
          // No space found — break at character boundary (CJK text)
          rem = rem.substr(accum.size());
        }
        lineIndex++;
        if (lineIndex > scrollOffset && defY + defLineH <= maxDefY) {
          renderer.drawText(defFont, textX, defY, accum.c_str(), true);
          defY += defLineH;
          linesDrawn++;
        }
      }
    }

    totalLines = lineIndex;
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
