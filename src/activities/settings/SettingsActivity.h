#pragma once
#include <I18n.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "CrossPointSettings.h"
#include "activities/UiTabListActivity.h"
#include "components/OptionPopup.h"

enum class SettingType { TOGGLE, ENUM, ACTION, VALUE, STRING };

enum class SettingAction {
  None,
  RemapFrontButtons,
  CustomiseStatusBar,
  KOReaderSync,
  OPDSBrowser,
  Network,
  ClearCache,
  CheckForUpdates,
  SdFirmwareUpdate,
  Language,
  DownloadFonts,
  TextSettings,
};

struct SettingInfo {
  StrId nameId;
  SettingType type;
  uint8_t CrossPointSettings::* valuePtr = nullptr;
  std::vector<StrId> enumValues;
  std::vector<std::string> enumStringValues;  // runtime alternative to StrId enumValues (for SD card fonts etc.)
  SettingAction action = SettingAction::None;

  struct ValueRange {
    uint8_t min;
    uint8_t max;
    uint8_t step;
  };
  ValueRange valueRange = {};

  const char* key = nullptr;             // JSON API key (nullptr for ACTION types)
  StrId category = StrId::STR_NONE_OPT;  // Category for web UI grouping
  bool obfuscated = false;               // Save/load via base64 obfuscation (passwords)
  bool inTextSettings = false;           // Surfaced in the Text Settings screen; hidden from the flat Reader list

  // Direct char[] string fields (for settings stored in CrossPointSettings)
  size_t stringOffset = 0;
  size_t stringMaxLen = 0;

  // Dynamic accessors (for settings stored outside CrossPointSettings, e.g. KOReaderCredentialStore)
  std::function<uint8_t()> valueGetter;
  std::function<void(uint8_t)> valueSetter;
  std::function<std::string()> stringGetter;
  std::function<void(const std::string&)> stringSetter;

  SettingInfo& withObfuscated() {
    obfuscated = true;
    return *this;
  }

  SettingInfo& withTextSettings() {
    inTextSettings = true;
    return *this;
  }

  static SettingInfo Toggle(StrId nameId, uint8_t CrossPointSettings::* ptr, const char* key = nullptr,
                            StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::TOGGLE;
    s.valuePtr = ptr;
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo Enum(StrId nameId, uint8_t CrossPointSettings::* ptr, std::vector<StrId> values,
                          const char* key = nullptr, StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::ENUM;
    s.valuePtr = ptr;
    s.enumValues = std::move(values);
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo Action(StrId nameId, SettingAction action) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::ACTION;
    s.action = action;
    return s;
  }

  static SettingInfo Value(StrId nameId, uint8_t CrossPointSettings::* ptr, const ValueRange valueRange,
                           const char* key = nullptr, StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::VALUE;
    s.valuePtr = ptr;
    s.valueRange = valueRange;
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo String(StrId nameId, char* ptr, size_t maxLen, const char* key = nullptr,
                            StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::STRING;
    s.stringOffset = (size_t)ptr - (size_t)&SETTINGS;
    s.stringMaxLen = maxLen;
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo DynamicEnum(StrId nameId, std::vector<StrId> values, std::function<uint8_t()> getter,
                                 std::function<void(uint8_t)> setter, const char* key = nullptr,
                                 StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::ENUM;
    s.enumValues = std::move(values);
    s.valueGetter = std::move(getter);
    s.valueSetter = std::move(setter);
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo DynamicString(StrId nameId, std::function<std::string()> getter,
                                   std::function<void(const std::string&)> setter, const char* key = nullptr,
                                   StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::STRING;
    s.stringGetter = std::move(getter);
    s.stringSetter = std::move(setter);
    s.key = key;
    s.category = category;
    return s;
  }

  // For a toggle backed by state outside CrossPointSettings (e.g. a per-book override that
  // lives on the reader activity, not the settings singleton). Reuses the existing uint8_t
  // valueGetter/valueSetter fields -- no struct layout change -- so the TOGGLE branches in
  // SettingsActivity that already check valueGetter/valueSetter (mirroring the DynamicEnum
  // path) work unmodified.
  static SettingInfo DynamicToggle(StrId nameId, std::function<bool()> getter, std::function<void(bool)> setter,
                                   StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::TOGGLE;
    s.valueGetter = [g = std::move(getter)]() -> uint8_t { return g() ? 1 : 0; };
    s.valueSetter = [st = std::move(setter)](const uint8_t v) { st(v != 0); };
    s.category = category;
    return s;
  }
};

class SettingsActivity final : public UiTabListActivity {
  int initialCategory = 0;
  bool finishOnBack = false;
  bool japaneseBook = false;
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
  // Manga has no font/margin/text-layout settings (no Text Settings sub-screen) and no image
  // rendering mode (manga pages ARE images) -- both are hidden from the Reader category for it.
  // Rotate Panels, Reading Orientation and Customise Status Bar all still apply and stay.
  bool mangaMode = false;

  int selectedCategoryIndex = 0;  // Currently selected category
  int settingsCount = 0;

  // Per-category settings derived from shared list + device-only actions
  std::vector<SettingInfo> displaySettings;
  std::vector<SettingInfo> readerSettings;
  std::vector<SettingInfo> controlsSettings;
  std::vector<SettingInfo> systemSettings;
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
  static const StrId categoryNames[categoryCount];

  // --- UiTabListActivity contract ---
  int listCount() const override { return settingsCount; }
  int tabCount() const override { return categoryCount; }
  int activeTab() const override { return selectedCategoryIndex; }
  const char* tabLabel(int index) const override { return I18N.get(categoryNames[index]); }
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
  void selectCategory(int categoryIndex);
  void applyUiSettingChange(uint8_t CrossPointSettings::* valuePtr);

  void enterCategory(int categoryIndex);
  void toggleCurrentSetting();
  void openSleepTimeoutPicker();
  void rebuildSettingsLists();
  void saveSettings();
  void syncQuickResumeTimeoutForSleepScreen(bool sleepScreenChanged, bool quickResumeTimeoutChanged);

 public:
  // initialCategory: category tab to open on (0=Display, 1=Reader, 2=Controls, 3=System).
  // finishOnBack: pop back to the pushing activity (e.g. the reader menu's "Reader Settings")
  // instead of replacing the stack with Home.
  // showReaderToggles/verticalTextEnabled/furiganaEnabled: see the member comment above.
  // mangaMode hides settings that do not apply to image-based manga books.
  // hideMangaOnlySettings hides Rotate Panels in non-manga embedded Reader Settings.
  explicit SettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const int initialCategory = 0,
                            const bool finishOnBack = false, const bool japaneseBook = false,
                            std::string dictionaryLanguage = {}, const bool showReaderToggles = false,
                            const bool verticalTextEnabled = false, const bool furiganaEnabled = false,
                            const bool mangaMode = false, const bool hideMangaOnlySettings = false)
      : UiTabListActivity("Settings", renderer, mappedInput),
        initialCategory(initialCategory),
        finishOnBack(finishOnBack),
        japaneseBook(japaneseBook),
        dictionaryLanguage(std::move(dictionaryLanguage)),
        showReaderToggles(showReaderToggles),
        verticalTextState(verticalTextEnabled),
        furiganaState(furiganaEnabled),
        mangaMode(mangaMode),
        hideMangaOnlySettings(hideMangaOnlySettings) {}
  void onEnter() override;
  void onExit() override;
  void render(RenderLock&&) override;
};
