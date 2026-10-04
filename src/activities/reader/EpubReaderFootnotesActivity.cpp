#include "EpubReaderFootnotesActivity.h"

#include <Epub.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "FootnoteTextExtractor.h"
#include "MappedInputManager.h"
#include "PanelTouch.h"
#include "components/DictionaryPanel.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Notes are prose meant to be read, and in a Japanese book they are Japanese: the UI face
// carries the CJK fallback the dictionary's Latin serif does not.
constexpr int NOTE_FONT_ID = UI_12_FONT_ID;
}  // namespace

void EpubReaderFootnotesActivity::onEnter() {
  Activity::onEnter();
  selectFootnote(startIndex);
}

void EpubReaderFootnotesActivity::onExit() {
  FootnoteText::releaseStaging();
  Activity::onExit();
}

void EpubReaderFootnotesActivity::selectFootnote(const int index) {
  if (footnotes.empty()) {
    noteText = tr(STR_NO_FOOTNOTES);
  } else {
    selectedIndex = std::max(0, std::min(index, static_cast<int>(footnotes.size()) - 1));
    if (!(epub && FootnoteText::extract(*epub, currentSpineIndex, footnotes[selectedIndex].href, noteText))) {
      // Extraction failed (unresolvable href, unreadable target): show the raw target so the
      // panel still says WHERE Confirm would jump.
      noteText = footnotes[selectedIndex].href;
    }
  }
  const auto body = DictionaryPanel::compute(renderer).body;
  textPages.layout(renderer, NOTE_FONT_ID, noteText, body.width, body.height);
  scrollLine = 0;
  requestUpdate();
}

void EpubReaderFootnotesActivity::stepNote(const int direction) {
  const int next = selectedIndex + direction;
  if (next < 0 || next >= static_cast<int>(footnotes.size())) return;
  RenderLock lock(*this);  // the render task draws from the text this replaces
  selectFootnote(next);
}

void EpubReaderFootnotesActivity::scrollBy(const int lines) {
  const int maxLine = std::max(0, textPages.lineCount() - textPages.visibleLines());
  const int next = std::max(0, std::min(scrollLine + lines, maxLine));
  if (next == scrollLine) return;
  scrollLine = next;
  requestUpdate();
}

void EpubReaderFootnotesActivity::cancel() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

void EpubReaderFootnotesActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    cancel();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
      mappedInput.wasReleased(MappedInputManager::Button::Power)) {
    if (selectedIndex >= 0 && selectedIndex < static_cast<int>(footnotes.size())) {
      setResult(FootnoteResult{footnotes[selectedIndex].href});
      finish();
    }
    return;
  }

  // A few lines short of a full panel, so the last lines read stay in view as context.
  const int scrollStep = std::max(1, textPages.visibleLines() - 2);
  switch (PanelTouch::read(renderer, mappedInput)) {
    case PanelTouch::Action::Close:
      cancel();
      return;
    case PanelTouch::Action::Next:
      stepNote(1);
      return;
    case PanelTouch::Action::Previous:
      stepNote(-1);
      return;
    case PanelTouch::Action::ScrollDown:
      scrollBy(scrollStep);
      return;
    case PanelTouch::Action::ScrollUp:
      scrollBy(-scrollStep);
      return;
    case PanelTouch::Action::AddButton:
    case PanelTouch::Action::None:
      break;
  }

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::ScreenRight}, [this] { stepNote(1); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::ScreenLeft}, [this] { stepNote(-1); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::ScreenDown},
                                       [this, scrollStep] { scrollBy(scrollStep); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::ScreenUp},
                                       [this, scrollStep] { scrollBy(-scrollStep); });
}

void EpubReaderFootnotesActivity::render(RenderLock&&) {
  // No clearScreen: the panel floats over the reader's page, which is still in the framebuffer.
  // The note's place among the page's notes, always shown.
  char counter[16] = "";
  if (!footnotes.empty()) {
    snprintf(counter, sizeof(counter), "%d/%u", selectedIndex + 1, static_cast<unsigned>(footnotes.size()));
  }
  const char* position = tr(STR_FOOTNOTES);
  // The note's own marker ("1", "*") names it; the text itself usually repeats it.
  const char* headword =
      !footnotes.empty() && footnotes[selectedIndex].number[0] ? footnotes[selectedIndex].number : tr(STR_FOOTNOTES);
  const auto layout = DictionaryPanel::draw(renderer, headword, position, counter);

  // Two-pass draw inside a prewarm scope so SD-card glyphs load in one batch.
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  textPages.drawLines(renderer, NOTE_FONT_ID, layout.body.x, layout.body.y, noteText, scrollLine);
  scope.endScanAndPrewarm();
  textPages.drawLines(renderer, NOTE_FONT_ID, layout.body.x, layout.body.y, noteText, scrollLine);

  const bool hasPrev = selectedIndex > 0;
  const bool hasNext = selectedIndex + 1 < static_cast<int>(footnotes.size());
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), footnotes.empty() ? "" : tr(STR_SELECT), hasPrev ? "<" : "",
                                            hasNext ? ">" : "");
  DictionaryPanel::clearButtonHints(renderer);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
