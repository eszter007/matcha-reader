#include "MangaChapterSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace fui = freeink::ui;

MangaChapterSelectionActivity::MangaChapterSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                             std::vector<manga::TocEntry> tocEntries,
                                                             const uint32_t currentPage)
    : UiListActivity("MangaChapterSelection", renderer, mappedInput), tocEntries(std::move(tocEntries)) {
  // Which chapter the current page falls within. Applied in onEnter(), not here: the base
  // class resets the nav there, so a selection made in the constructor never survives.
  for (size_t i = 0; i < this->tocEntries.size(); i++) {
    if (this->tocEntries[i].pageIndex <= currentPage) {
      initialSelected = static_cast<int>(i);
    } else {
      break;
    }
  }
  // The rows borrow their labels from tocEntries, which outlives them and is not touched again
  // after this: a second copy of every chapter title is heap this device would rather keep.
  items.reserve(this->tocEntries.size());
  for (size_t i = 0; i < this->tocEntries.size(); i++) {
    fui::ListItem item;
    item.label = this->tocEntries[i].title.c_str();
    // The row's identity travels in actionValue -- list() registers the tap
    // target as hit(rect, action, item.actionValue), so leaving it at its
    // default reported every row as index 0 and every tap opened the first
    // chapter.
    item.actionValue = static_cast<int16_t>(i);
    items.push_back(item);
  }
}

void MangaChapterSelectionActivity::onEnter() {
  UiListActivity::onEnter();
  // After the base, which resets the nav: the first screen build then pulls the viewport to the
  // chapter the reader is in, the same way the EPUB picker does.
  nav.selected = initialSelected;
  requestUpdate();
}

void MangaChapterSelectionActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  // Content: the safe area minus the header band drawChrome paints the title in.
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)), static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (listCount() == 0) {
    screen.centeredText(tr(STR_NO_CHAPTERS), screen.theme().bodyText);
    return;
  }

  fui::ListProps props;
  props.count = static_cast<uint16_t>(listCount());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in handleButtons()
  syncListViewport(screen, props);
  props.items = items.data();
  props.itemsWindowFirst = 0;
  screen.list(props);
}

void MangaChapterSelectionActivity::activateIndex(const int index) {
  if (index < 0 || index >= listCount()) {
    return;
  }
  // The activated row leaves this screen (finish); a lingering flash would gray
  // an unrelated element on the next render.
  app.clearTapFlash();
  nav.selected = index;
  setResult(PageResult{tocEntries[index].pageIndex});
  finish();
}

bool MangaChapterSelectionActivity::handleButtons() {
  // Confirm activates on RELEASE, matching the EPUB picker: the press that
  // opened this screen must not also select the row under the cursor.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateIndex(nav.selected);
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return true;
  }
  return false;
}

// Header only: UiListActivity::render() calls drawFooter() for the button hints, and it draws the
// same four labels. Painting them here as well drew them twice -- three times on the pass where a
// wrapped row makes the list re-layout and drawChrome() runs again.
void MangaChapterSelectionActivity::drawChrome() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  GUI.drawHeader(renderer, Rect{safe.x, safe.y + metrics.topPadding, safe.width, metrics.headerHeight},
                 tr(STR_SELECT_CHAPTER));
}
