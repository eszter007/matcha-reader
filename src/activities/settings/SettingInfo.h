#pragma once
#include <I18n.h>

#include <algorithm>
#include <functional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "CrossPointSettings.h"

// A row of the settings table (SettingsList.h), shared by the Settings sections and Reader Settings.
enum class SettingType { TOGGLE, ENUM, ACTION, VALUE, STRING };

enum class SettingAction {
  None,
  RemapFrontButtons,
  CustomiseStatusBar,
  ClockSettings,
  KOReaderSync,
  OPDSBrowser,
  Network,
  Bluetooth,
  ClearCache,
  RebuildLibraryIndex,
  CheckForUpdates,
  SdFirmwareUpdate,
  Language,
  DownloadFonts,
  TextSettings,
  Plugins,
  KeyboardLayouts,
  HomeButton,
  LibrarySettings,
  SleepSettings,
  ShortcutsSettings,
  About,
};

struct SettingInfo {
  StrId nameId;
  SettingType type;
  uint8_t CrossPointSettings::* valuePtr = nullptr;
  std::vector<StrId> enumValues;
  std::span<const StrId> staticEnumValues;
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

  // Stored values, in the order the options should be OFFERED. Empty means "offer them in
  // stored-value order", which is the default. Persisted indices are frozen by every settings
  // file already on a card, so a menu that reads badly cannot be fixed by renumbering the enum --
  // this reorders the presentation alone. enumValues/staticEnumValues stay indexed BY STORED
  // VALUE, so settingValueText() and the persistence clamp need no mapping.
  std::vector<uint8_t> enumOrder;
  bool webHidden = false;  // Persisted normally, but omitted from the generic Web Settings API

  std::span<const StrId> enumLabels() const {
    return staticEnumValues.empty() ? std::span<const StrId>(enumValues) : staticEnumValues;
  }

  // Menu slot -> stored value, and back. Identity while enumOrder is empty. Both clamp, so a
  // corrupt or migrated byte lands on the first slot rather than indexing out of the table.
  uint8_t storedFromSlot(const uint8_t slot) const { return storedFromSlot(enumOrder, slot); }
  static uint8_t storedFromSlot(const std::vector<uint8_t>& order, const uint8_t slot) {
    if (order.empty()) return slot;
    return slot < order.size() ? order[slot] : order[0];
  }
  uint8_t slotFromStored(const uint8_t stored) const {
    if (enumOrder.empty()) return stored;
    const auto it = std::find(enumOrder.begin(), enumOrder.end(), stored);
    return it != enumOrder.end() ? static_cast<uint8_t>(it - enumOrder.begin()) : 0;
  }
  // The labels in menu order. Returned by value: the popup wants a contiguous array and the
  // reordered view does not exist anywhere else. At most SIDE_BUTTON_ACTION_COUNT entries.
  std::vector<StrId> orderedEnumLabels() const {
    const auto labels = enumLabels();
    if (enumOrder.empty()) return std::vector<StrId>(labels.begin(), labels.end());
    std::vector<StrId> out;
    out.reserve(enumOrder.size());
    for (const uint8_t stored : enumOrder) {
      if (stored < labels.size()) out.push_back(labels[stored]);
    }
    return out;
  }

  SettingInfo& withEnumOrder(std::vector<uint8_t> order) {
    enumOrder = std::move(order);
    return *this;
  }
  SettingInfo& withWebHidden() {
    webHidden = true;
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

  static SettingInfo StaticEnum(StrId nameId, uint8_t CrossPointSettings::* ptr, std::span<const StrId> values,
                                const char* key = nullptr, StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::ENUM;
    s.valuePtr = ptr;
    s.staticEnumValues = values;
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

  static SettingInfo String(StrId nameId, const char* ptr, size_t maxLen, const char* key = nullptr,
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
