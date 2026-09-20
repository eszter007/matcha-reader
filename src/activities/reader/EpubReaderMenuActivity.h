#pragma once
#include <Epub.h>
#include <I18n.h>

#include <string>
#include <vector>

#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

class EpubReaderMenuActivity final : public UiListActivity {
 public:
  // Menu actions available from the reader menu.
  enum class MenuAction {
    SELECT_CHAPTER,
    FOOTNOTES,
    TEXT_SETTINGS,
    NIGHT_MODE,
    FRONTLIGHT,
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
    DICTIONARY,
    WORD_LOOKUP,
    TRANSLATE_PAGE,
    TOGGLE_VERTICAL,
    TOGGLE_FURIGANA,
    TOGGLE_PANELS_ONLY,
    READER_SETTINGS
  };

  struct MenuItem {
    MenuAction action;
    StrId labelId;
  };

  static std::vector<MenuItem> buildMenuItems(bool hasFootnotes, bool hasBookmarks, bool hasWordLookup,
                                              bool imageReaderMinimal, bool mangaMode, bool hideGenericLookup,
                                              bool showPanelsOnlyToggle);

  explicit EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title,
                                  const int currentPage, const int totalPages, const int bookProgressPercent,
                                  const uint8_t currentOrientation, const bool hasFootnotes,
                                  const bool hasBookmarks = false, const bool hasWordLookup = false,
                                  const bool verticalEnabled = false, const bool furiganaEnabled = true,
                                  const bool hasPageText = true, const bool imageReaderMinimal = false,
                                  const bool mangaMode = false, const bool hideGenericLookup = false,
                                  const bool showPanelsOnlyToggle = false, const bool panelsOnlyEnabled = false,
                                  const bool scrubOnEnter = false);

  void render(RenderLock&&) override;
  bool handleHomeGesture() override;

 private:
  // Set when the reader page underneath drew images: its gray charge survives a FAST
  // diff and would show through this screen. Scrubbed once on the first paint.
  const bool scrubOnEnter_;
  bool firstPaint_ = true;
  // Row storage: menuItems is at most MAX_MENU_ITEMS, so a
  // fixed-capacity array avoids any heap allocation for the row list. Labels
  // are set once in the constructor (buildMenuRowItems()); buildScreen()
  // only refreshes the rows whose value reflects live state (rotation,
  // page-turn interval, night mode, frontlight).
  // 18 rows at most in the fork's buildMenuItems() (Word Lookup, Translate Page, Panels Only and
  // Reader Settings on top of upstream's set, plus upstream's Night Mode and Frontlight); 20
  // leaves headroom for one more without a silent truncation, which a fixed-capacity array
  // cannot report.
  static constexpr size_t MAX_MENU_ITEMS = 20;
  freeink::ui::ListItem menuRowItems[MAX_MENU_ITEMS]{};
  void buildMenuRowItems();

  int listCount() const override { return static_cast<int>(menuItems.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  // Popup input runs before any button or touch handling.
  bool handleCustomInput() override;
  // Back closes on RELEASE and Confirm activates on RELEASE.
  bool handleButtons() override;
  void navigateButtons() override;
  // Header via GUI.drawHeader inside the safe area for the battery indicator.
  void drawChrome() override;

  void closeCancelled();
  int enabledIndexFrom(int index, int direction) const;

  // Fixed menu layout
  // Not const: the fork rebuilds the row set when a toggle changes what is shown.
  std::vector<MenuItem> menuItems;
  // Whether the CURRENT page/panel has text to act on. Word Lookup / Translate / QR stay in the
  // list but render with ListItem::enabled=false when it doesn't, so their positions never shift
  // page-to-page (manga panels without OCR'd dialogue, image-only EPUB pages).
  bool hasPageText = true;

  OptionPopup optionPopup;
  std::string title = "Reader Menu";
  uint8_t pendingOrientation = 0;
  // Seed this menu's MenuResult (still read by the reader's apply-if-changed check); the in-menu
  // controls for vertical/furigana moved into Reader Settings.
  bool pendingVerticalEnabled = false;
  bool pendingFuriganaEnabled = true;
  bool panelsOnlyEnabled = false;
  uint8_t selectedPageTurnOption = 0;
  const std::vector<StrId> orientationLabels = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_INVERTED,
                                                StrId::STR_LANDSCAPE_CCW};
  const std::vector<const char*> pageTurnLabels = {I18N.get(StrId::STR_STATE_OFF), "1", "3", "6", "12"};
  int currentPage = 0;
  int totalPages = 0;
  int bookProgressPercent = 0;
};
