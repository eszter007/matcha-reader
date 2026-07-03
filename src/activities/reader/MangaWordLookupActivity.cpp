#include "MangaWordLookupActivity.h"

#include <DictIndex.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <WordLookup.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

bool isLookupableChar(uint32_t cp) {
  if (cp < 0x30) return false;
  if (cp >= 0x3040 && cp <= 0x309F) return true;
  if (cp >= 0x30A0 && cp <= 0x30FF) return true;
  if (cp >= 0x4E00 && cp <= 0x9FFF) return true;
  if (cp >= 0x3400 && cp <= 0x4DBF) return true;
  if (cp >= 0xF900 && cp <= 0xFAFF) return true;
  return cp >= 0x80;
}

bool isCJK(uint32_t cp) {
  return (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) ||
         (cp >= 0xF900 && cp <= 0xFAFF);
}

bool stripTrailingParticle(const std::string& text, WordLookupResult& result) {
  if (result.matchLength == 0) return false;
  size_t pos = 0, lastStart = 0;
  uint32_t lastCp = 0, prevCp = 0;
  int chars = 0;
  while (pos < result.matchLength && pos < text.size()) {
    prevCp = lastCp;
    lastStart = pos;
    auto c = static_cast<unsigned char>(text[pos]);
    if (c < 0x80) { lastCp = c; pos += 1; }
    else if ((c & 0xE0) == 0xC0) { lastCp = ((c & 0x1F) << 6) | (text[pos + 1] & 0x3F); pos += 2; }
    else if ((c & 0xF0) == 0xE0) { lastCp = ((c & 0x0F) << 12) | ((text[pos + 1] & 0x3F) << 6) | (text[pos + 2] & 0x3F); pos += 3; }
    else { lastCp = 0; pos += 4; }
    chars++;
  }
  if (chars < 2) return false;

  const bool isParticle = lastCp == 0x306E || lastCp == 0x306F || lastCp == 0x304C || lastCp == 0x3092 ||
                          lastCp == 0x306B || lastCp == 0x3078 || lastCp == 0x3082 || lastCp == 0x3068;
  if (!isParticle) return false;
  if (!isCJK(prevCp)) return false;

  std::string stem = text.substr(0, lastStart);
  WordLookupResult sr;
  if (WordLookup::lookup(stem, 0, sr) && sr.matchLength == stem.size()) {
    result = std::move(sr);
    return true;
  }
  return false;
}

}  // namespace

MangaWordLookupActivity::MangaWordLookupActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                  std::string panelText)
    : Activity("MangaWordLookup", renderer, mappedInput) {
  // Decode UTF-8 text into codepoints
  size_t b = 0;
  while (b < panelText.size()) {
    auto c0 = static_cast<unsigned char>(panelText[b]);
    uint32_t cp;
    if (c0 < 0x80) { cp = c0; b += 1; }
    else if ((c0 & 0xE0) == 0xC0) { cp = (c0 & 0x1F) << 6 | (panelText[b + 1] & 0x3F); b += 2; }
    else if ((c0 & 0xF0) == 0xE0) { cp = (c0 & 0x0F) << 12 | ((panelText[b + 1] & 0x3F) << 6) | (panelText[b + 2] & 0x3F); b += 3; }
    else { cp = (c0 & 0x07) << 18 | ((panelText[b + 1] & 0x3F) << 12) | ((panelText[b + 2] & 0x3F) << 6) | (panelText[b + 3] & 0x3F); b += 4; }

    if (cp == '\n' || cp == '\r') continue;
    allGlyphs.push_back(GlyphRef{cp});
  }

  buildSelectableGlyphs();
}

void MangaWordLookupActivity::buildSelectableGlyphs() {
  selectableGlyphs.reserve(allGlyphs.size());
  selectToAllIdx.reserve(allGlyphs.size());

  size_t skipUntil = 0;
  for (size_t i = 0; i < allGlyphs.size(); i++) {
    if (i < skipUntil) continue;
    if (!isLookupableChar(allGlyphs[i].codepoint)) continue;

    std::string text;
    int charCount = 0;
    for (size_t j = i; j < allGlyphs.size() && charCount < kMaxLookupChars; j++) {
      encodeUtf8(allGlyphs[j].codepoint, text);
      charCount++;
    }

    WordLookupResult result;
    bool hasMatch = !text.empty() && WordLookup::lookup(text, 0, result);
    if (hasMatch) {
      stripTrailingParticle(text, result);
      int matchChars = 0;
      size_t pos = 0;
      while (pos < result.matchLength && pos < text.size()) {
        auto c = static_cast<unsigned char>(text[pos]);
        if (c < 0x80) pos += 1;
        else if ((c & 0xE0) == 0xC0) pos += 2;
        else if ((c & 0xF0) == 0xE0) pos += 3;
        else pos += 4;
        matchChars++;
      }

      if (matchChars > 1) {
        skipUntil = i + matchChars;
      }

      selectToAllIdx.push_back(i);
      selectableGlyphs.push_back(allGlyphs[i]);
    }
  }
}

void MangaWordLookupActivity::onEnter() {
  Activity::onEnter();
  const int maxIdx = static_cast<int>(selectableGlyphs.size()) - 1;
  for (cursorIndex = 0; cursorIndex <= maxIdx; cursorIndex++) {
    performLookup();
    if (hasResult) break;
  }
  if (cursorIndex > maxIdx) cursorIndex = 0;
  requestUpdate();
}

void MangaWordLookupActivity::onExit() { Activity::onExit(); }

void MangaWordLookupActivity::moveCursor(int delta) {
  if (selectableGlyphs.empty()) return;
  const int maxIdx = static_cast<int>(selectableGlyphs.size()) - 1;
  // selectableGlyphs is already the pre-filtered list of positions buildSelectableGlyphs()
  // confirmed have a dictionary match -- every index in it is valid by construction. Same fix as
  // EpubReaderWordLookupActivity::moveCursor(): the previous retry-skip loop re-validated via a
  // fresh performLookup() and kept advancing past any position that didn't independently agree,
  // silently skipping entries ("every second entry is skipped" during navigation).
  cursorIndex += delta;
  if (cursorIndex < 0) cursorIndex = 0;
  if (cursorIndex > maxIdx) cursorIndex = maxIdx;
  performLookup();
}

void MangaWordLookupActivity::encodeUtf8(uint32_t cp, std::string& out) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

std::string MangaWordLookupActivity::buildLookupText(size_t startIdx) const {
  std::string text;
  if (startIdx >= selectToAllIdx.size()) return text;

  const size_t allStart = selectToAllIdx[startIdx];
  int charCount = 0;
  for (size_t i = allStart; i < allGlyphs.size() && charCount < kMaxLookupChars; i++) {
    encodeUtf8(allGlyphs[i].codepoint, text);
    charCount++;
  }
  return text;
}

void MangaWordLookupActivity::performLookup() {
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
    stripTrailingParticle(text, result);
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
  if (Storage.exists(DictIndex::GRAMMAR_IDX_PATH) && cursorIndex < static_cast<int>(selectToAllIdx.size())) {
    const size_t allStart = selectToAllIdx[cursorIndex];
    int bestGramLen = 0;
    std::string bestGramHw, bestGramDef;

    for (int backoff = 3; backoff >= 0; backoff--) {
      size_t scanStart = allStart;
      for (int b2 = 0; b2 < backoff && scanStart > 0; b2++) scanStart--;

      std::string gramText;
      int gCharCount = 0;
      for (size_t j = scanStart; j < allGlyphs.size() && gCharCount < 12; j++) {
        encodeUtf8(allGlyphs[j].codepoint, gramText);
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
      resultDefinition += "\n\n— Grammar: " + bestGramHw + " —\n" + bestGramDef;
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
}

void MangaWordLookupActivity::renderContentArea(const Rect& screen, int contentTop) {
  auto metrics = UITheme::getInstance().getMetrics();
  const int jaFont = SETTINGS.getReaderFontId();

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

  if (selectableGlyphs.empty() || !hasResult) {
    UITheme::drawCenteredText(renderer, screen, UI_12_FONT_ID,
                              screen.y + screen.height / 2, tr(STR_NO_MATCH), true);
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
  int linesDrawn = 0;
  int lineIndex = 0;
  const int maxDefY = screen.y + screen.height - 2;
  const int firstDefY = defY;

  size_t nlPos = 0;
  std::string defText = resultDefinition;
  while (nlPos <= defText.size() && linesDrawn < 999) {
    size_t nextNl = defText.find('\n', nlPos);
    std::string paragraph = (nextNl == std::string::npos)
        ? defText.substr(nlPos) : defText.substr(nlPos, nextNl - nlPos);
    nlPos = (nextNl == std::string::npos) ? defText.size() + 1 : nextNl + 1;

    if (paragraph.empty()) {
      lineIndex++;
      if (lineIndex > scrollOffset) defY += defLineH / 2;
      continue;
    }

    std::string rem = paragraph;
    while (!rem.empty() && linesDrawn < 999) {
      if (renderer.getTextWidth(defFont, rem.c_str()) <= maxWidth) {
        lineIndex++;
        if (lineIndex > scrollOffset && defY + defLineH <= maxDefY) {
          renderer.drawText(defFont, textX, defY, rem.c_str(), true);
          defY += defLineH;
          linesDrawn++;
        }
        break;
      }

      std::string accum;
      size_t lastSpaceBreak = std::string::npos;
      const char* p = rem.c_str();
      while (*p) {
        size_t charLen = 1;
        auto c0 = static_cast<unsigned char>(*p);
        if (c0 >= 0xF0) charLen = 4;
        else if (c0 >= 0xE0) charLen = 3;
        else if (c0 >= 0xC0) charLen = 2;
        std::string test = accum + std::string(p, charLen);
        if (renderer.getTextWidth(defFont, test.c_str()) > maxWidth) break;
        accum = test;
        if (*p == ' ') lastSpaceBreak = accum.size();
        p += charLen;
      }

      if (accum.empty()) {
        auto c0 = static_cast<unsigned char>(rem[0]);
        size_t cl = 1;
        if (c0 >= 0xF0) cl = 4; else if (c0 >= 0xE0) cl = 3; else if (c0 >= 0xC0) cl = 2;
        accum = rem.substr(0, cl);
        rem = rem.substr(cl);
      } else if (lastSpaceBreak != std::string::npos && lastSpaceBreak > 0) {
        std::string line = accum.substr(0, lastSpaceBreak);
        rem = rem.substr(lastSpaceBreak);
        if (!rem.empty() && rem[0] == ' ') rem = rem.substr(1);
        accum = line;
      } else {
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
  const int visibleCapacity = (maxDefY - firstDefY) / defLineH;
  maxScroll = std::max(0, totalLines - visibleCapacity);
}

void MangaWordLookupActivity::render(RenderLock&&) {
  auto& theme = UITheme::getInstance();
  auto metrics = theme.getMetrics();
  Rect screen = theme.getScreenSafeArea(renderer, true, false);

  const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  std::string posText;
  if (hasResult && !selectableGlyphs.empty()) {
    posText = std::to_string(cursorIndex + 1) + "/" + std::to_string(selectableGlyphs.size());
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
