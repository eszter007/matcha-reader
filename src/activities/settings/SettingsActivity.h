#pragma once
#include <I18n.h>

#include <functional>
#include <span>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "SettingInfo.h"
#include "SettingRowEditor.h"
#include "SettingsSection.h"
#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

// The rows of one SettingsSection; activating a row toggles/cycles it, opens
// its option popup, or launches its sub-screen. With a subCategory (Shortcuts)
// it lists only that category's rows, reached from a row in section.
class SettingsActivity final : public UiListActivity, private SettingRowEditor::Host {
  const SettingsSection section;
  const StrId subCategory;
  std::vector<SettingInfo> settings;

  OptionPopup optionPopup;
  SettingRowEditor editor{*this, renderer, mappedInput, *this, optionPopup};

  // Row structure (label/actionValue), rebuilt only when the setting list
  // changes; rowValues_ holds the live value text, refreshed every
  // buildScreen() by assigning into the existing strings.
  std::vector<std::string> rowValues_;
  std::vector<freeink::ui::ListItem> rowItems_;

  void rebuildSettings();
  void rebuildRowItems();
  void toggleSelectedSetting();
  void applyUiSettingChange(uint8_t CrossPointSettings::* valuePtr);
  void saveSettings() override;
  void commitSettingChange(uint8_t CrossPointSettings::* valuePtr) override;

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
