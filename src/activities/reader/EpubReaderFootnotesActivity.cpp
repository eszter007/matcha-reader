#include "EpubReaderFootnotesActivity.h"

#include <Epub.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>

#include "DefinitionTextRenderer.h"
#include "FootnoteTextExtractor.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void EpubReaderFootnotesActivity::onEnter() {
  Activity::onEnter();
  selectFootnote(startIndex);
}

void EpubReaderFootnotesActivity::onExit() { Activity::onExit(); }

void EpubReaderFootnotesActivity::selectFootnote(const int index) {
  if (footnotes.empty()) return;
  selectedIndex = ((index % static_cast<int>(footnotes.size())) + static_cast<int>(footnotes.size())) %
                  static_cast<int>(footnotes.size());
  scrollOffset = 0;
  totalLines = 0;
  maxScroll = 0;
  noteLoaded = epub && FootnoteText::extract(*epub, currentSpineIndex, footnotes[selectedIndex].href, noteText);
  if (!noteLoaded) {
    // Extraction failed (unresolvable href, unreadable target): show the raw target so the
    // panel still says WHERE Confirm would jump.
    noteText = footnotes[selectedIndex].href;
  }
  requestUpdate();
}

void EpubReaderFootnotesActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
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

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right},
                                       [this] { selectFootnote(selectedIndex + 1); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left},
                                       [this] { selectFootnote(selectedIndex - 1); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down}, [this] {
    if (scrollOffset < maxScroll) {
      scrollOffset = std::min(maxScroll, scrollOffset + 5);
      requestUpdate();
    }
  });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up}, [this] {
    if (scrollOffset > 0) {
      scrollOffset = std::max(0, scrollOffset - 5);
      requestUpdate();
    }
  });
}

void EpubReaderFootnotesActivity::renderContentArea(const Rect& screen, const int contentTop) {
  auto metrics = UITheme::getInstance().getMetrics();
  const int maxWidth = screen.width - metrics.contentSidePadding * 2;
  const int textX = screen.x + metrics.contentSidePadding;

  if (footnotes.empty()) {
    UITheme::drawCenteredText(renderer, screen, UI_12_FONT_ID, screen.y + screen.height / 2, tr(STR_NO_FOOTNOTES),
                              true);
    return;
  }

  // No headline: the note text itself starts with its number ("2. ..."), so a number title
  // would just duplicate it. The header's position counter already says which note this is.
  // Larger font than Word Lookup's definitions -- notes are prose meant to be read, not
  // dictionary entries to skim.
  const int defY = contentTop;
  const int defFont = UI_12_FONT_ID;
  const int defLineH = renderer.getLineHeight(defFont);
  const int maxDefY = screen.y + screen.height - 2;
  const int firstDefY = defY;
  const auto wrap =
      DefinitionText::drawWrapped(renderer, defFont, noteText, textX, defY, defLineH, maxWidth, maxDefY, scrollOffset);

  totalLines = wrap.totalLines;
  const int visibleCapacity = (maxDefY - firstDefY) / defLineH;
  maxScroll = std::max(0, totalLines - visibleCapacity);
}

void EpubReaderFootnotesActivity::render(RenderLock&&) {
  auto& theme = UITheme::getInstance();
  auto metrics = theme.getMetrics();
  Rect screen = theme.getScreenSafeArea(renderer, true, false);

  const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  std::string posText;
  if (!footnotes.empty()) {
    posText = std::to_string(selectedIndex + 1) + "/" + std::to_string(footnotes.size());
  }
  const Rect headerRect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight};

  if (!initialRenderDone) {
    renderer.clearScreen();
    GUI.drawHeader(renderer, headerRect, tr(STR_FOOTNOTES), posText.empty() ? nullptr : posText.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

    renderContentArea(screen, contentTop);

    renderer.displayBuffer();
    initialRenderDone = true;
    fastRefreshCount = 0;
  } else {
    // Same partial-redraw scheme as Word Lookup: clear from the content top to the physical
    // bottom, redraw header (position counter) and hints, fast-refresh with periodic settles.
    const int physBottom = renderer.getScreenHeight();
    renderer.fillRect(0, contentTop, renderer.getScreenWidth(), physBottom - contentTop, false);
    GUI.drawHeader(renderer, headerRect, tr(STR_FOOTNOTES), posText.empty() ? nullptr : posText.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

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
