#pragma once
#include <string>

#include "CrossPointSettings.h"
#include "SettingInfo.h"

class Activity;
class GfxRenderer;
class MappedInputManager;
class OptionPopup;

// Row editing shared by the screens that list SettingInfo rows (Settings and Reader Settings).
// The screen supplies how it saves and rebuilds; the editing rules live here once.
class SettingRowEditor {
 public:
  class Host {
   public:
    virtual void saveSettings() = 0;
    // Save, rebuild the rows, and apply a change that must show at once (the theme).
    virtual void commitSettingChange(uint8_t CrossPointSettings::* valuePtr) = 0;

   protected:
    ~Host() = default;
  };

  enum class EditResult : uint8_t {
    NotEditable,  // not a value row this editor handles
    Opened,       // a popup or picker took over; it commits when the user picks
    Changed,      // changed in place and committed; rows may have been added or removed
  };

  SettingRowEditor(Activity& activity, GfxRenderer& renderer, MappedInputManager& mappedInput, Host& host,
                   OptionPopup& popup)
      : activity(activity), renderer(renderer), mappedInput(mappedInput), host(host), popup(popup) {}

  // Toggles, cycles or opens the popup for a value row. `setting` may be invalid afterwards.
  EditResult editValue(const SettingInfo& setting);
  // Launches an action row's sub-screen if it is one every settings screen shares.
  bool launchSharedAction(SettingAction action);
  void rebuildLibraryIndex();
  // Call on screen entry: Quick Resume keeps its timeout on while it is the sleep screen.
  void beginQuickResumeSync();

  static std::string valueText(const SettingInfo& setting);

 private:
  Activity& activity;
  GfxRenderer& renderer;
  MappedInputManager& mappedInput;
  Host& host;
  OptionPopup& popup;
  bool preserveQuickResumeTimeoutOn = false;
  bool quickResumeTimeoutAutoEnabled = false;

  void syncQuickResumeTimeoutForSleepScreen(bool sleepScreenChanged, bool quickResumeTimeoutChanged);
  void openSleepTimeoutPicker();
};
