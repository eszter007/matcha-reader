#pragma once
#include <I18n.h>

#include <functional>
#include <span>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "SettingInfo.h"
#include "SettingsSection.h"
#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

// The rows of one SettingsSection; activating a row toggles/cycles it, opens
// its option popup, or launches its sub-screen. With a subCategory (Sleep,
// Shortcuts) it lists only that category's rows, reached from a row in section.
class SettingsActivity final : public UiListActivity {
  const SettingsSection section;
  const StrId subCategory;
  std::vector<SettingInfo> settings;

  bool preserveQuickResumeTimeoutOn = false;
  bool quickResumeTimeoutAutoEnabled = false;

  OptionPopup optionPopup;

  // Row structure (label/actionValue), rebuilt only when the setting list
  // changes; rowValues_ holds the live value text, refreshed every
  // buildScreen() by assigning into the existing strings.
  std::vector<std::string> rowValues_;
  std::vector<freeink::ui::ListItem> rowItems_;

  void rebuildSettings();
  void rebuildLibraryIndex();
  void rebuildRowItems();
  void toggleSelectedSetting();
  void openSleepTimeoutPicker();
  void applyUiSettingChange(uint8_t CrossPointSettings::* valuePtr);
  void syncQuickResumeTimeoutForSleepScreen(bool sleepScreenChanged, bool quickResumeTimeoutChanged);
  static std::string settingValueText(const SettingInfo& setting);

  int listCount() const override { return static_cast<int>(settings.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  const char* headerTitle() const override;
  void drawFooter() override;

 public:
  SettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, SettingsSection section,
                   StrId subCategory = StrId::STR_NONE_OPT);
  void onEnter() override;
  void render(RenderLock&& lock) override;
};
