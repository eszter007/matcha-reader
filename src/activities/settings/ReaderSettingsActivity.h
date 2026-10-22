#pragma once
#include <I18n.h>

#include <algorithm>
#include <functional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "CrossPointSettings.h"
#include "SettingInfo.h"
#include "activities/UiTabListActivity.h"
#include "components/OptionPopup.h"
#include "util/CjkScript.h"

class ReaderSettingsActivity final : public UiTabListActivity {
  int initialCategory = 0;
  bool finishOnBack = false;
  // Script of the book that pushed this screen (None from Home or a Latin book). CJK books use
  // the converted-dictionary flow, so the StarDict picker is omitted for them.
  CjkScript bookScript = CjkScript::None;
  std::string dictionaryLanguage;
  // Vertical Text / Furigana: per-book overrides that live on the pushing reader activity, not
  // in CrossPointSettings. showReaderToggles gates whether they appear at all (mirrors the
  // condition the reader menu used before these moved here: isJapaneseBook() || forced on).
  // Mutated in place by their DynamicToggle setters; read back on finish() via a MenuResult (see
  // ActivityResult.h) so the caller can apply them the same way it already applies font/margin
  // changes made in this screen.
  bool showReaderToggles = false;
  bool verticalTextState = false;
  bool furiganaState = false;
  // What the toggles showed on entry: a toggle left as it was is reported as untouched (-1), so
  // a Book Language change in the same visit is not undone by the direction it started with.
  bool initialVerticalTextState = false;
  bool initialFuriganaState = false;
  // Book Language: the per-book override (cjk::LanguageChoice) that re-tags a mis-tagged EPUB.
  // Shown for every text book pushed from the reader; -1 when the caller offers no such row.
  int8_t languageState = -1;
  // Manga has no font/margin/text-layout settings (no Text Settings sub-screen) and no image
  // rendering mode (manga pages ARE images) -- both are hidden from the Reader category for it.
  // Rotate Panels, Reading Orientation and Customise Status Bar all still apply and stay.
  bool mangaMode = false;

  // Single-category mode. The Library and Sleep rows in Display, and Shortcuts in Controls, open
  // this same screen showing only their own category, none of which is one of the four tabs.
  // STR_NONE_OPT means the ordinary tabbed screen. Everything else -- row building, value text, the option popup,
  // toggles, actions -- is the usual path, so a sub-screen costs a list rather than an activity.
  StrId submenuCategory = StrId::STR_NONE_OPT;
  bool isSubmenu() const { return submenuCategory != StrId::STR_NONE_OPT; }

  int selectedCategoryIndex = 0;  // Currently selected category
  int settingsCount = 0;

  // Per-category settings derived from shared list + device-only actions
  std::vector<SettingInfo> displaySettings;
  std::vector<SettingInfo> readerSettings;
  std::vector<SettingInfo> controlsSettings;
  std::vector<SettingInfo> systemSettings;
  std::vector<SettingInfo> submenuSettings;
  const std::vector<SettingInfo>* currentSettings = nullptr;

  bool preserveQuickResumeTimeoutOn = false;
  bool quickResumeTimeoutAutoEnabled = false;
  // Home settings remain global. Reader-launched settings hide controls that only apply to manga.
  bool hideMangaOnlySettings = false;

  OptionPopup optionPopup;

  // Row structure (label/actionValue) for *currentSettings, rebuilt only when
  // the active category or a category's setting list changes
  // (rebuildRowItems(), called from selectCategory()/rebuildSettingsLists())
  // — not on every repaint. rowValues_ holds the live per-row value text,
  // refreshed every buildScreen() call by assigning into the existing
  // strings (no vector growth).
  std::vector<std::string> rowValues_;
  std::vector<freeink::ui::ListItem> rowItems_;
  void rebuildRowItems();

  static constexpr int categoryCount = 4;
  static constexpr StrId categoryNames[categoryCount] = {StrId::STR_CAT_DISPLAY, StrId::STR_CAT_READER,
                                                         StrId::STR_CAT_CONTROLS, StrId::STR_CAT_SYSTEM};

  // --- UiTabListActivity contract ---
  int listCount() const override { return settingsCount; }
  int tabCount() const override { return isSubmenu() ? 1 : categoryCount; }
  // Embedded in the reader the category band is hidden and locked: no top tabs on the ring.
  int ringTopTabCount() const override { return finishOnBack ? 0 : tabCount(); }
  int activeTab() const override { return isSubmenu() ? 0 : selectedCategoryIndex; }
  const char* tabLabel(int index) const override {
    return I18N.get(isSubmenu() ? submenuCategory : categoryNames[index]);
  }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onTabAction(int index) override;
  void stepTab(int direction) override;
  void navigateButtons() override;
  bool handleButtons() override;
  bool handleCustomInput() override;

  static std::string settingValueText(const SettingInfo& setting);
  // True when the row is an on/off setting, so it draws a switch instead of a value string.
  // Covers both TOGGLE forms and the two-value {OFF, ON} enums, which already flip in place
  // rather than opening the option popup.
  static bool settingIsSwitch(const SettingInfo& setting);
  static bool settingSwitchState(const SettingInfo& setting);
  // A row the user cannot change: an enum with a getter but nothing to write through. Reader
  // Settings shows the dictionary this way -- the book's language picks it, so the row reports
  // the choice rather than offering one. Such rows draw disabled and the cursor steps over them.
  static bool settingIsReadOnly(const SettingInfo& setting);
  // First ring position at or after `ring` whose row is enabled, walking `direction`. Falls back
  // to the current position when every row is disabled.
  int enabledRingFrom(int ring, int direction) const;
  void selectCategory(int categoryIndex);
  void applyUiSettingChange(uint8_t CrossPointSettings::* valuePtr);

  void toggleCurrentSetting();
  void openSleepTimeoutPicker();
  void rebuildLibraryIndex();
  void rebuildSettingsLists();
  void saveSettings();
  void syncQuickResumeTimeoutForSleepScreen(bool sleepScreenChanged, bool quickResumeTimeoutChanged);

  void drawChrome() override;
  void drawFooter() override;
  HomeTab tabBarTab() const override { return HomeTab::Settings; }

 public:
  // initialCategory: category tab to open on (0=Display, 1=Reader, 2=Controls, 3=System).
  // finishOnBack: pop back to the pushing activity (e.g. the reader menu's "Reader Settings")
  // instead of replacing the stack with Home.
  // showReaderToggles/verticalTextEnabled/furiganaEnabled: see the member comment above.
  // mangaMode hides settings that do not apply to image-based manga books.
  // hideMangaOnlySettings hides Rotate Panels in non-manga embedded Reader Settings.
  explicit ReaderSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const int initialCategory = 0,
                                  const bool finishOnBack = false, const CjkScript bookScript = CjkScript::None,
                                  std::string dictionaryLanguage = {}, const bool showReaderToggles = false,
                                  const bool verticalTextEnabled = false, const bool furiganaEnabled = false,
                                  const bool mangaMode = false, const bool hideMangaOnlySettings = false,
                                  const StrId submenuCategory = StrId::STR_NONE_OPT, const int8_t languageChoice = -1)
      : UiTabListActivity("Settings", renderer, mappedInput),
        initialCategory(initialCategory),
        finishOnBack(finishOnBack),
        bookScript(bookScript),
        dictionaryLanguage(std::move(dictionaryLanguage)),
        showReaderToggles(showReaderToggles),
        verticalTextState(verticalTextEnabled),
        furiganaState(furiganaEnabled),
        initialVerticalTextState(verticalTextEnabled),
        initialFuriganaState(furiganaEnabled),
        languageState(languageChoice),
        mangaMode(mangaMode),
        submenuCategory(submenuCategory),
        hideMangaOnlySettings(hideMangaOnlySettings) {}
  void onEnter() override;
  void onExit() override;
  void render(RenderLock&& lock) override;
};
