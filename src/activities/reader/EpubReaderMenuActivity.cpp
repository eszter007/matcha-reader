#include "EpubReaderMenuActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

EpubReaderMenuActivity::EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                               const std::string& title, const int currentPage, const int totalPages,
                                               const int bookProgressPercent, const uint8_t currentOrientation,
                                               const bool hasFootnotes, const bool hasBookmarks,
                                               const bool hasWordLookup, const bool showVerticalToggle,
                                               const bool verticalEnabled, const bool furiganaEnabled,
                                               const bool hasPageText, const bool imageReaderMinimal)
    : Activity("EpubReaderMenu", renderer, mappedInput),
      menuItems(buildMenuItems(hasFootnotes, hasBookmarks, hasWordLookup, showVerticalToggle, verticalEnabled,
                               furiganaEnabled, imageReaderMinimal)),
      hasPageText(hasPageText),
      title(title),
      pendingOrientation(currentOrientation),
      pendingVerticalEnabled(verticalEnabled),
      pendingFuriganaEnabled(furiganaEnabled),
      currentPage(currentPage),
      totalPages(totalPages),
      bookProgressPercent(bookProgressPercent) {}

std::vector<EpubReaderMenuActivity::MenuItem> EpubReaderMenuActivity::buildMenuItems(bool hasFootnotes,
                                                                                     bool hasBookmarks,
                                                                                     bool hasWordLookup,
                                                                                     bool showVerticalToggle,
                                                                                     bool verticalEnabled,
                                                                                     bool furiganaEnabled,
                                                                                     bool imageReaderMinimal) {
  std::vector<MenuItem> items;
  items.reserve(16);

  // Minimal menu for the image readers (XTC): a page-based format has no text/footnotes/vertical
  // toggles, so only chapter select (when present), Go-to-page, bookmarks, screenshot and
  // clear-cache apply. hasFootnotes is repurposed as "has chapters" in this mode.
  if (imageReaderMinimal) {
    if (hasFootnotes) items.push_back({MenuAction::SELECT_CHAPTER, StrId::STR_SELECT_CHAPTER});
    items.push_back({MenuAction::GO_TO_PERCENT, StrId::STR_GO_TO_PERCENT});
    items.push_back({MenuAction::TOGGLE_BOOKMARK, StrId::STR_TOGGLE_BOOKMARK});
    if (hasBookmarks) items.push_back({MenuAction::BOOKMARKS, StrId::STR_BOOKMARKS});
    items.push_back({MenuAction::SCREENSHOT, StrId::STR_SCREENSHOT_BUTTON});
    items.push_back({MenuAction::DELETE_CACHE, StrId::STR_DELETE_CACHE});
    return items;
  }

  items.push_back({MenuAction::SELECT_CHAPTER, StrId::STR_SELECT_CHAPTER});
  if (hasFootnotes) {
    items.push_back({MenuAction::FOOTNOTES, StrId::STR_FOOTNOTES});
  }
  if (hasWordLookup) {
    items.push_back({MenuAction::WORD_LOOKUP, StrId::STR_WORD_LOOKUP});
  }
  items.push_back({MenuAction::TRANSLATE_PAGE, StrId::STR_TRANSLATE_PAGE});
  if (showVerticalToggle) {
    items.push_back({MenuAction::TOGGLE_VERTICAL, StrId::STR_VERTICAL_TEXT_LABEL});
    items.push_back({MenuAction::TOGGLE_FURIGANA, StrId::STR_FURIGANA_LABEL});
  }
  items.push_back({MenuAction::READER_SETTINGS, StrId::STR_READER_SETTINGS});
  if (hasBookmarks) {
    items.push_back({MenuAction::BOOKMARKS, StrId::STR_BOOKMARKS});
  }
  items.push_back({MenuAction::TOGGLE_BOOKMARK, StrId::STR_TOGGLE_BOOKMARK});
  items.push_back({MenuAction::ROTATE_SCREEN, StrId::STR_ORIENTATION});
  items.push_back({MenuAction::AUTO_PAGE_TURN, StrId::STR_AUTO_TURN_PAGES_PER_MIN});
  items.push_back({MenuAction::GO_TO_PERCENT, StrId::STR_GO_TO_PERCENT});
  items.push_back({MenuAction::SCREENSHOT, StrId::STR_SCREENSHOT_BUTTON});
  items.push_back({MenuAction::DISPLAY_QR, StrId::STR_DISPLAY_QR});
  items.push_back({MenuAction::GO_HOME, StrId::STR_GO_HOME_BUTTON});
  items.push_back({MenuAction::SYNC, StrId::STR_SYNC_PROGRESS});
  items.push_back({MenuAction::DELETE_CACHE, StrId::STR_DELETE_CACHE});
  return items;
}

void EpubReaderMenuActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void EpubReaderMenuActivity::onExit() { Activity::onExit(); }

void EpubReaderMenuActivity::loop() {
  // Handle navigation
  buttonNavigator.onNext([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, static_cast<int>(menuItems.size()));
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, static_cast<int>(menuItems.size()));
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const auto selectedAction = menuItems[selectedIndex].action;
    if (selectedAction == MenuAction::ROTATE_SCREEN) {
      // Cycle orientation preview locally; actual rotation happens on menu exit.
      pendingOrientation = (pendingOrientation + 1) % orientationLabels.size();
      requestUpdate();
      return;
    }

    if (selectedAction == MenuAction::AUTO_PAGE_TURN) {
      selectedPageTurnOption = (selectedPageTurnOption + 1) % pageTurnLabels.size();
      requestUpdate();
      return;
    }

    if (selectedAction == MenuAction::TOGGLE_VERTICAL) {
      pendingVerticalEnabled = !pendingVerticalEnabled;
      requestUpdate();
      return;
    }

    if (selectedAction == MenuAction::TOGGLE_FURIGANA) {
      pendingFuriganaEnabled = !pendingFuriganaEnabled;
      requestUpdate();
      return;
    }

    setResult(MenuResult{static_cast<int>(selectedAction), pendingOrientation, selectedPageTurnOption,
                         static_cast<int8_t>(pendingVerticalEnabled ? 1 : 0),
                         static_cast<int8_t>(pendingFuriganaEnabled ? 1 : 0)});
    finish();
    return;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    result.data = MenuResult{-1, pendingOrientation, selectedPageTurnOption,
                             static_cast<int8_t>(pendingVerticalEnabled ? 1 : 0),
                             static_cast<int8_t>(pendingFuriganaEnabled ? 1 : 0)};
    setResult(std::move(result));
    finish();
    return;
  }
}

void EpubReaderMenuActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 title.c_str());

  // Progress summary
  std::string progressLine;
  if (totalPages > 0) {
    progressLine = std::string(tr(STR_CHAPTER_PREFIX)) + std::to_string(currentPage) + "/" +
                   std::to_string(totalPages) + std::string(tr(STR_PAGES_SEPARATOR));
  }
  progressLine += std::string(tr(STR_BOOK_PREFIX)) + std::to_string(bookProgressPercent) + "%";
  GUI.drawSubHeader(
      renderer,
      Rect{screen.x, screen.y + metrics.topPadding + metrics.headerHeight, screen.width, metrics.tabBarHeight},
      progressLine.c_str());

  const int contentTop =
      screen.y + metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing;
  const int contentHeight = screen.height - contentTop - metrics.verticalSpacing;

  GUI.drawList(
      renderer, Rect{screen.x, contentTop, screen.width, contentHeight}, menuItems.size(), selectedIndex,
      [this](int index) { return I18N.get(menuItems[index].labelId); }, nullptr, nullptr,
      [this](int index) {
        const auto value = menuItems[index].action;
        if (value == MenuAction::ROTATE_SCREEN) {
          // Render current orientation value on the right edge of the content area.
          return I18N.get(orientationLabels[pendingOrientation]);
        } else if (value == MenuAction::AUTO_PAGE_TURN) {
          // Render current page turn value on the right edge of the content area.
          return pageTurnLabels[selectedPageTurnOption];
        } else if (value == MenuAction::TOGGLE_VERTICAL) {
          return I18N.get(pendingVerticalEnabled ? StrId::STR_STATE_ON : StrId::STR_STATE_OFF);
        } else if (value == MenuAction::TOGGLE_FURIGANA) {
          return I18N.get(pendingFuriganaEnabled ? StrId::STR_STATE_ON : StrId::STR_STATE_OFF);
        } else {
          return "";
        }
      },
      true,
      [this](int index) {
        if (hasPageText) return false;
        const auto value = menuItems[index].action;
        return value == MenuAction::WORD_LOOKUP || value == MenuAction::TRANSLATE_PAGE ||
               value == MenuAction::DISPLAY_QR;
      },
      /*showScrollbar=*/false);

  // Footer / Hints
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
