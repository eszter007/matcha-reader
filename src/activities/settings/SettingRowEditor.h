#pragma once
#include <string>
#include <vector>

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

  // The help line (settingHelp) of the first row that has one, drawn in a band above the footer.
  // Shown whatever row is selected: a touch screen selects none. helpBandHeight() is the room a
  // screen with such a row keeps free under its list.
  static int helpBandHeight(const GfxRenderer& renderer);
  static void drawHelp(const GfxRenderer& renderer, const std::vector<SettingInfo>& rows, int footerHeight);

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
#if FREEINK_CAP_FRONTLIGHT
  // Light Schedule rows whose values want a list or slider rather than a step on Confirm.
  bool openLightSchedulePicker(const SettingInfo& setting);
#endif
};
