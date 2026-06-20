#include "EpubReaderWordLookupActivity.h"

#include <DictIndex.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <WordLookup.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int kMaxLookupChars = 8;
}

EpubReaderWordLookupActivity::EpubReaderWordLookupActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                            const VerticalPage& page)
    : Activity("WordLookup", renderer, mappedInput) {
  selectableGlyphs.reserve(page.glyphs.size());
  for (const auto& g : page.glyphs) {
    if (g.rotated) continue;
    selectableGlyphs.push_back(
        GlyphRef{g.x, g.y, g.column, g.row, g.codepoint, g.paragraphIndex, g.rotated});
  }
}

void EpubReaderWordLookupActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void EpubReaderWordLookupActivity::onExit() { Activity::onExit(); }

void EpubReaderWordLookupActivity::moveCursor(int delta) {
  if (selectableGlyphs.empty()) return;
  cursorIndex += delta;
  if (cursorIndex < 0) cursorIndex = 0;
  if (cursorIndex >= static_cast<int>(selectableGlyphs.size())) {
    cursorIndex = static_cast<int>(selectableGlyphs.size()) - 1;
  }
  hasResult = false;
  requestUpdate();
}

void EpubReaderWordLookupActivity::encodeUtf8(uint32_t cp, std::string& out) {
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

std::string EpubReaderWordLookupActivity::buildLookupText(size_t startIdx) const {
  std::string text;
  if (startIdx >= selectableGlyphs.size()) return text;

  const uint32_t paraIdx = selectableGlyphs[startIdx].paragraphIndex;
  int charCount = 0;

  for (size_t i = startIdx; i < selectableGlyphs.size() && charCount < kMaxLookupChars; i++) {
    const auto& g = selectableGlyphs[i];
    if (g.paragraphIndex != paraIdx) break;
    encodeUtf8(g.codepoint, text);
    charCount++;
  }
  return text;
}

void EpubReaderWordLookupActivity::performLookup() {
  hasResult = false;
  resultHeadword.clear();
  resultDefinition.clear();
  resultMatchLen = 0;

  std::string text = buildLookupText(static_cast<size_t>(cursorIndex));
  if (text.empty()) return;

  WordLookupResult result;
  if (WordLookup::lookup(text, 0, result)) {
    hasResult = true;
    resultHeadword = std::move(result.entry.headword);
    resultDefinition = std::move(result.entry.definition);
    resultMatchLen = static_cast<int>(result.matchLength);
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

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down}, [this] { moveCursor(1); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up}, [this] { moveCursor(-1); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [this] { moveCursor(10); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [this] { moveCursor(-10); });
}

void EpubReaderWordLookupActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto& theme = UITheme::getInstance();
  auto metrics = theme.getMetrics();
  Rect screen = theme.getScreenSafeArea(renderer, true, false);

  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_WORD_LOOKUP));

  if (selectableGlyphs.empty()) {
    UITheme::drawCenteredText(renderer, screen, UI_12_FONT_ID,
                              screen.y + screen.height / 2, tr(STR_NO_MATCH), true);
  } else if (hasResult) {
    const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int maxWidth = screen.width - metrics.horizontalPadding * 2;
    const int textX = screen.x + metrics.horizontalPadding;

    renderer.drawText(UI_12_FONT_ID, textX, contentTop, resultHeadword.c_str(), true, EpdFontFamily::BOLD);

    int defY = contentTop + renderer.getLineHeight(UI_12_FONT_ID) + metrics.verticalSpacing;

    auto lines = renderer.wrappedText(SMALL_FONT_ID, resultDefinition.c_str(), maxWidth, 8);
    for (const auto& line : lines) {
      renderer.drawText(SMALL_FONT_ID, textX, defY, line.c_str(), true);
      defY += renderer.getLineHeight(SMALL_FONT_ID);
    }

    // Attribution
    defY += metrics.verticalSpacing;
    renderer.drawText(SMALL_FONT_ID, textX, defY, tr(STR_DICT_CREDIT), true);
  } else {
    // Show cursor character preview
    const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 2;

    std::string preview;
    encodeUtf8(selectableGlyphs[cursorIndex].codepoint, preview);

    std::string posText = std::to_string(cursorIndex + 1) + "/" + std::to_string(selectableGlyphs.size());
    UITheme::drawCenteredText(renderer, screen, UI_12_FONT_ID, contentTop, preview.c_str(), true,
                              EpdFontFamily::BOLD);
    UITheme::drawCenteredText(renderer, screen, SMALL_FONT_ID,
                              contentTop + renderer.getLineHeight(UI_12_FONT_ID) + 4, posText.c_str(), true);

    // Show a text preview of the next few characters
    std::string windowPreview = buildLookupText(static_cast<size_t>(cursorIndex));
    if (!windowPreview.empty()) {
      UITheme::drawCenteredText(renderer, screen, UI_12_FONT_ID,
                                contentTop + renderer.getLineHeight(UI_12_FONT_ID) + 30, windowPreview.c_str(),
                                true);
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
