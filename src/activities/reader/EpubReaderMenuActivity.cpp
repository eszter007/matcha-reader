#include "EpubReaderMenuActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

EpubReaderMenuActivity::EpubReaderMenuActivity(
    GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title, const int currentPage,
    const int totalPages, const int bookProgressPercent, const uint8_t currentOrientation, const bool hasFootnotes,
    const bool hasBookmarks, const bool hasWordLookup, const bool verticalEnabled, const bool furiganaEnabled,
    const bool hasPageText, const bool imageReaderMinimal, const bool mangaMode, const bool hideGenericLookup,
    const bool showPanelsOnlyToggle, const bool panelsOnlyEnabled)
    : Activity("EpubReaderMenu", renderer, mappedInput),
      menuItems(buildMenuItems(hasFootnotes, hasBookmarks, hasWordLookup, imageReaderMinimal, mangaMode,
                               hideGenericLookup, showPanelsOnlyToggle)),
      hasPageText(hasPageText),
      title(title),
      pendingOrientation(currentOrientation),
      pendingVerticalEnabled(verticalEnabled),
      pendingFuriganaEnabled(furiganaEnabled),
      panelsOnlyEnabled(panelsOnlyEnabled),
      currentPage(currentPage),
      totalPages(totalPages),
      bookProgressPercent(bookProgressPercent) {}

std::vector<EpubReaderMenuActivity::MenuItem> EpubReaderMenuActivity::buildMenuItems(
std::vector<EpubReaderMenuActivity::MenuItem> EpubReaderMenuActivity::buildMenuItems(
    bool hasFootnotes, bool hasBookmarks, bool hasWordLookup, bool imageReaderMinimal, bool mangaMode,
    bool hideGenericLookup, bool showPanelsOnlyToggle) {
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
  // Vertical Text and Furigana moved into Reader Settings (SettingInfo::DynamicToggle, gated
  // there on the same condition this menu used to gate TOGGLE_VERTICAL/TOGGLE_FURIGANA) so they
  // sit with the rest of the reading-experience settings instead of this per-page action menu.
  // Reader Settings itself is shown for manga too, filtered down to the settings manga supports.
  if (showPanelsOnlyToggle) {
    items.push_back({MenuAction::TOGGLE_PANELS_ONLY, StrId::STR_PANELS_ONLY});
  }
  items.push_back({MenuAction::READER_SETTINGS, StrId::STR_READER_SETTINGS});
  if (hasBookmarks) {
    items.push_back({MenuAction::BOOKMARKS, StrId::STR_BOOKMARKS});
  }
  items.push_back({MenuAction::TOGGLE_BOOKMARK, StrId::STR_TOGGLE_BOOKMARK});
  // Free-form dictionary lookup doesn't apply to manga (Word Lookup covers OCR'd text) or to
  // unsegmented Japanese text, where Word Lookup is the only lookup that makes sense.
  if (!mangaMode && !hideGenericLookup) {
    items.push_back({MenuAction::DICTIONARY, StrId::STR_LOOKUP});
  }
  // Reading Orientation removed from this quick menu entirely; it's a proper setting under
  // Settings > Reader (SettingsList.h, tied to CrossPointSettings::orientation directly) for
  // every reader type now that Reader Settings is reachable from manga too (see above) --
  // ROTATE_SCREEN's handling below (the option popup, pendingOrientation) is unreachable but
  // left in place rather than torn out along with it.
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

void EpubReaderMenuActivity::onExit() {
  Activity::onExit(); }

void EpubReaderMenuActivity::closeCancelled() {
  ActivityResult result;
  result.isCancelled = true;
  // The toggles must ride along on EVERY exit path. Vertical Text and Furigana don't close the
  // menu when pressed -- they flip a pending flag and redraw -- so leaving with Back IS how the
  // user commits them. Omitting them here leaves MenuResult's -1 defaults, which the reader
  // reads as "unchanged", and the toggle silently does nothing. (Upstream added this early Back
  // handler; the fork's own Back branch further down carried the flags and became unreachable.)
  result.data =
      MenuResult{-1, pendingOrientation, selectedPageTurnOption, static_cast<int8_t>(pendingVerticalEnabled ? 1 : 0),
                 static_cast<int8_t>(pendingFuriganaEnabled ? 1 : 0)};
  setResult(std::move(result));
  finish();
}

bool EpubReaderMenuActivity::handleHomeGesture() {
  closeCancelled();
  return true;
}

void EpubReaderMenuActivity::loop() {
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) {
    // The popup acts on button press; if that input closed it, the trailing
    // release must be swallowed below (Back would close the menu, Confirm
    // would re-activate the selected item).
    popupClosing = !optionPopup.isActive();
    return;
  }
  if (popupClosing) {
    if (mappedInput.isPressed(MappedInputManager::Button::Back) ||
        mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      return;  // closing press still held
    }
    popupClosing = false;
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      return;  // swallow the release that closed the popup
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    closeCancelled();
    return;
  }

  auto activateSelected = [this] {
    const auto selectedAction = menuItems[selectedIndex].action;
    if (selectedAction == MenuAction::ROTATE_SCREEN) {
      optionPopup.show(StrId::STR_ORIENTATION, orientationLabels.data(), static_cast<int>(orientationLabels.size()),
                       pendingOrientation, [this](int idx) {
                         pendingOrientation = idx;
                         requestUpdate();
                       });
      requestUpdate();
      return;
    }

    if (selectedAction == MenuAction::AUTO_PAGE_TURN) {
      optionPopup.show(I18N.get(StrId::STR_AUTO_TURN_PAGES_PER_MIN), pageTurnLabels.data(),
                       static_cast<int>(pageTurnLabels.size()), selectedPageTurnOption, [this](int idx) {
                         selectedPageTurnOption = idx;
                         requestUpdate();
                       });
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
  };

  auto metrics = UITheme::getInstance().getMetrics();
  Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int contentTop =
      screen.y + metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing;
  const int contentHeight = screen.height - contentTop - metrics.verticalSpacing;
  switch (handleListTouch(selectedIndex, static_cast<int>(menuItems.size()), contentTop, contentHeight, false)) {
    case ListTouchResult::Activated:
      activateSelected();
      return;
    case ListTouchResult::Consumed:
      return;
    case ListTouchResult::None:
      break;
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, static_cast<int>(menuItems.size()));
    requestUpdate();
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, static_cast<int>(menuItems.size()));
    requestUpdate();
    return;
  }

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
    activateSelected();
    return;
  }
}

void EpubReaderMenuActivity::render(RenderLock&&) {
  if (optionPopup.processRender(renderer, mappedInput)) return;

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
        } else if (value == MenuAction::TOGGLE_PANELS_ONLY) {
          return I18N.get(panelsOnlyEnabled ? StrId::STR_STATE_ON : StrId::STR_STATE_OFF);
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
