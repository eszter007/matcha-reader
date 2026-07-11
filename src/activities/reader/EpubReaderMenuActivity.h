#pragma once
#include <Epub.h>
#include <I18n.h>

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class EpubReaderMenuActivity final : public Activity {
 public:
  // Menu actions available from the reader menu.
  enum class MenuAction {
    SELECT_CHAPTER,
    FOOTNOTES,
    GO_TO_PERCENT,
    AUTO_PAGE_TURN,
    ROTATE_SCREEN,
    BOOKMARKS,
    TOGGLE_BOOKMARK,
    SCREENSHOT,
    DISPLAY_QR,
    GO_HOME,
    SYNC,
    DELETE_CACHE,
    WORD_LOOKUP,
    TRANSLATE_PAGE,
    TOGGLE_VERTICAL,
    TOGGLE_FURIGANA
  };

  // hasWordLookup gates whether Word Lookup appears at all (book-level: is
  // there a dictionary + is this a supported language) -- stable across a
  // book's pages, so hiding it doesn't shift other items around per-page.
  // hasPageText reflects whether the CURRENT page/panel actually has text
  // to act on; when false, Word Lookup/Translate/QR are dimmed (still
  // shown, still navigable) rather than hidden, since that can change
  // page-to-page (e.g. manga panels without OCR'd dialogue, image-only
  // EPUB pages) and hiding/showing per-page would shift menu positions.
  explicit EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title,
                                  const int currentPage, const int totalPages, const int bookProgressPercent,
                                  const uint8_t currentOrientation, const bool hasFootnotes,
                                  const bool hasBookmarks = false, const bool hasWordLookup = false,
                                  const bool showVerticalToggle = false, const bool verticalEnabled = false,
                                  const bool furiganaEnabled = true, const bool hasPageText = true,
                                  const bool imageReaderMinimal = false);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct MenuItem {
    MenuAction action;
    StrId labelId;
  };

  static std::vector<MenuItem> buildMenuItems(bool hasFootnotes, bool hasBookmarks, bool hasWordLookup,
                                               bool showVerticalToggle, bool verticalEnabled, bool furiganaEnabled,
                                               bool imageReaderMinimal);

  std::vector<MenuItem> menuItems;
  bool hasPageText = true;

  int selectedIndex = 0;

  ButtonNavigator buttonNavigator;
  std::string title = "Reader Menu";
  uint8_t pendingOrientation = 0;
  uint8_t selectedPageTurnOption = 0;
  bool pendingVerticalEnabled = false;
  bool pendingFuriganaEnabled = true;
  const std::vector<StrId> orientationLabels = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_INVERTED,
                                                StrId::STR_LANDSCAPE_CCW};
  const std::vector<const char*> pageTurnLabels = {I18N.get(StrId::STR_STATE_OFF), "1", "3", "6", "12"};
  int currentPage = 0;
  int totalPages = 0;
  int bookProgressPercent = 0;
};
