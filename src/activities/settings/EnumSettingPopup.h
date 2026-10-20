#pragma once

#include <functional>
#include <utility>

#include "CrossPointSettings.h"
#include "SettingInfo.h"
#include "components/OptionPopup.h"

// Changes an enum row whose value lives in CrossPointSettings, the same way on every settings
// screen. A row with more than two options opens `popup` in the row's menu order and returns true;
// `onPicked` runs after the stored value is set. A two-option row flips in place and returns false.
inline bool editEnumSetting(OptionPopup& popup, const SettingInfo& setting, std::function<void()> onPicked) {
  const uint8_t current = SETTINGS.*(setting.valuePtr);
  const auto labels = setting.orderedEnumLabels();
  if (labels.size() > 2) {
    popup.show(setting.nameId, labels.data(), static_cast<int>(labels.size()), setting.slotFromStored(current),
               [valuePtr = setting.valuePtr, order = setting.enumOrder, onPicked = std::move(onPicked)](int slot) {
                 SETTINGS.*valuePtr = SettingInfo::storedFromSlot(order, static_cast<uint8_t>(slot));
                 onPicked();
               });
    return true;
  }
  SETTINGS.*(setting.valuePtr) =
      setting.storedFromSlot((setting.slotFromStored(current) + 1) % static_cast<uint8_t>(labels.size()));
  return false;
}
