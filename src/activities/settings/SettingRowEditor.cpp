#include "SettingRowEditor.h"

#include <GfxRenderer.h>
#include <LibraryBuilder.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>

#include "AboutActivity.h"
#include "ButtonRemapActivity.h"
#include "ClearCacheActivity.h"
#include "ClockSettingsActivity.h"
#include "EnumSettingPopup.h"
#include "FontDownloadActivity.h"
#include "HomeButtonSettingsActivity.h"
#include "KOReaderSettingsActivity.h"
#include "KeyboardLayoutsActivity.h"
#include "LanguageSelectActivity.h"
#include "MappedInputManager.h"
#include "OpdsServerListActivity.h"
#include "OtaUpdateActivity.h"
#include "SdFirmwareUpdateActivity.h"
#include "SilentRestart.h"
#include "StatusBarSettingsActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/IntervalSelectionActivity.h"
#include "components/OptionPopup.h"
#include "components/UITheme.h"

SettingRowEditor::EditResult SettingRowEditor::editValue(const SettingInfo& setting) {
  const bool sleepScreenChanged = setting.valuePtr == &CrossPointSettings::sleepScreen;
  const bool quickResumeTimeoutChanged = setting.valuePtr == &CrossPointSettings::quickResumeSleepScreen;

  if (setting.nameId == StrId::STR_TIME_TO_SLEEP) {
    openSleepTimeoutPicker();
    return EditResult::Opened;
  }

  if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
    // Toggle the boolean value using the member pointer
    const bool currentValue = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = !currentValue;
  } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
    const auto valuePtr = setting.valuePtr;
    if (editEnumSetting(popup, setting, [this, valuePtr, sleepScreenChanged, quickResumeTimeoutChanged] {
          syncQuickResumeTimeoutForSleepScreen(sleepScreenChanged, quickResumeTimeoutChanged);
          host.commitSettingChange(valuePtr);
        })) {
      activity.requestUpdate();
      return EditResult::Opened;
    }
  } else if (setting.type == SettingType::ENUM && setting.valueGetter && setting.valueSetter) {
    const uint8_t totalValues = setting.enumStringValues.empty()
                                    ? static_cast<uint8_t>(setting.enumLabels().size())
                                    : static_cast<uint8_t>(setting.enumStringValues.size());
    const uint8_t cur = setting.valueGetter();
    if (totalValues > 2) {
      const auto valueSetter = setting.valueSetter;
      auto onSelect = [this, valueSetter, sleepScreenChanged, quickResumeTimeoutChanged](int idx) {
        valueSetter(idx);
        syncQuickResumeTimeoutForSleepScreen(sleepScreenChanged, quickResumeTimeoutChanged);
        host.commitSettingChange(nullptr);
      };
      if (!setting.enumStringValues.empty()) {
        popup.show(setting.nameId, setting.enumStringValues, cur, std::move(onSelect));
      } else {
        const auto enumLabels = setting.enumLabels();
        popup.show(setting.nameId, enumLabels.data(), static_cast<int>(enumLabels.size()), cur, std::move(onSelect));
      }
      activity.requestUpdate();
      return EditResult::Opened;
    }
    setting.valueSetter((cur + 1) % totalValues);
  } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
    const int8_t currentValue = SETTINGS.*(setting.valuePtr);
    if (currentValue + setting.valueRange.step > setting.valueRange.max) {
      SETTINGS.*(setting.valuePtr) = setting.valueRange.min;
    } else {
      SETTINGS.*(setting.valuePtr) = currentValue + setting.valueRange.step;
    }
  } else {
    return EditResult::NotEditable;
  }

  syncQuickResumeTimeoutForSleepScreen(sleepScreenChanged, quickResumeTimeoutChanged);
  host.commitSettingChange(setting.valuePtr);
  return EditResult::Changed;
}

bool SettingRowEditor::launchSharedAction(const SettingAction action) {
  auto resultHandler = [this](const ActivityResult&) { host.saveSettings(); };
  auto rebuildOnReturn = [this](const ActivityResult&) { host.commitSettingChange(nullptr); };

  switch (action) {
    case SettingAction::HomeButton: {
      // Activities must outlive this call and are owned by the activity stack.
      auto next = makeUniqueNoThrow<HomeButtonSettingsActivity>(renderer, mappedInput);
      if (!next) {
        LOG_ERR("SET", "OOM: Home button settings");
        return true;
      }
      activity.startActivityForResult(std::move(next), [this](const ActivityResult&) { activity.requestUpdate(); });
      return true;
    }
    case SettingAction::RemapFrontButtons:
      activity.startActivityForResult(std::make_unique<ButtonRemapActivity>(renderer, mappedInput), resultHandler);
      return true;
    case SettingAction::CustomiseStatusBar:
      activity.startActivityForResult(std::make_unique<StatusBarSettingsActivity>(renderer, mappedInput),
                                      resultHandler);
      return true;
    case SettingAction::ClockSettings:
      if (auto next = makeUniqueNoThrow<ClockSettingsActivity>(renderer, mappedInput)) {
        activity.startActivityForResult(std::move(next), resultHandler);
      } else {
        LOG_ERR("SETTINGS", "OOM: ClockSettingsActivity");
      }
      return true;
    case SettingAction::KOReaderSync:
      activity.startActivityForResult(std::make_unique<KOReaderSettingsActivity>(renderer, mappedInput), resultHandler);
      return true;
    case SettingAction::OPDSBrowser:
      activity.startActivityForResult(std::make_unique<OpdsServerListActivity>(renderer, mappedInput), resultHandler);
      return true;
    case SettingAction::Network: {
      auto next = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput, false);
      if (!next) {
        LOG_ERR("SETTINGS", "OOM: WifiSelectionActivity");
        return true;
      }
      activity.startActivityForResult(std::move(next), [](const ActivityResult&) {
        SETTINGS.saveToFile();
        // Every other WiFi consumer hands the radio to a session it owns;
        // these rows only save credentials, so nothing here would ever
        // release the driver's heap. The scan alone brings it up, so tear
        // down whether or not the user joined a network.
        if (WiFi.getMode() == WIFI_MODE_NULL) return;
        WiFi.disconnect(false);
        delay(30);
        // Unlike the onExit() teardowns, this runs from the loop task with
        // no lock held; the restart popup paints straight to the panel.
        RenderLock lock;
        silentRestartToSettings();
      });
      return true;
    }
    case SettingAction::RebuildLibraryIndex:
      rebuildLibraryIndex();
      return true;
    case SettingAction::ClearCache:
      activity.startActivityForResult(std::make_unique<ClearCacheActivity>(renderer, mappedInput), resultHandler);
      return true;
    case SettingAction::CheckForUpdates:
      activity.startActivityForResult(std::make_unique<OtaUpdateActivity>(renderer, mappedInput), resultHandler);
      return true;
    case SettingAction::SdFirmwareUpdate:
      activity.startActivityForResult(std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInput), resultHandler);
      return true;
    case SettingAction::DownloadFonts:
      activity.startActivityForResult(std::make_unique<FontDownloadActivity>(renderer, mappedInput), rebuildOnReturn);
      return true;
    case SettingAction::Language:
      // Row labels are translated once when the rows are built and don't
      // re-run on Pop (see ActivityManager::loop()), so a language switch
      // needs an explicit rebuild here rather than the generic resultHandler.
      activity.startActivityForResult(std::make_unique<LanguageSelectActivity>(renderer, mappedInput), rebuildOnReturn);
      return true;
    case SettingAction::KeyboardLayouts:
      if (auto next = makeUniqueNoThrow<KeyboardLayoutsActivity>(renderer, mappedInput)) {
        activity.startActivityForResult(std::move(next), nullptr);
      } else {
        LOG_ERR("SETTINGS", "OOM: KeyboardLayoutsActivity");
      }
      return true;
    case SettingAction::About:
      if (auto next = makeUniqueNoThrow<AboutActivity>(renderer, mappedInput)) {
        activity.startActivityForResult(std::move(next), nullptr);
      } else {
        LOG_ERR("SETTINGS", "OOM: AboutActivity");
      }
      return true;
    case SettingAction::None:
      return true;
    default:
      return false;
  }
}

void SettingRowEditor::rebuildLibraryIndex() {
  // Prevent SD-backed fonts from opening a second reader while EPUB metadata is scanned.
  // Keep the popup static because an e-ink refresh per folder would dominate the rebuild.
  RenderLock lock(activity);
  GUI.drawPopup(renderer, tr(STR_LIBRARY_REBUILDING));

  library::BuildStats stats;
  const bool ok = library::buildLibraryIndex("/", stats, SETTINGS.libraryUseMetadata != 0);
  if (ok) {
    LOG_INF("LIB", "rebuild: %u books (%u new, %u renamed, %u removed, %u enriched) in %ums",
            static_cast<unsigned>(stats.books), static_cast<unsigned>(stats.added),
            static_cast<unsigned>(stats.renamed), static_cast<unsigned>(stats.removed),
            static_cast<unsigned>(stats.enriched), static_cast<unsigned>(stats.walkMs));
    if (stats.dedupDegraded) LOG_ERR("LIB", "rebuild completed without duplicate detection");
  } else {
    LOG_ERR("LIB", "index rebuild failed");
  }

  GUI.drawPopup(renderer, ok ? tr(STR_LIBRARY_REBUILD_DONE) : tr(STR_LIBRARY_REBUILD_FAILED));
  delay(1200);
  activity.requestUpdate(true);
}

void SettingRowEditor::beginQuickResumeSync() {
  preserveQuickResumeTimeoutOn =
      SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
  quickResumeTimeoutAutoEnabled = false;
  syncQuickResumeTimeoutForSleepScreen(/*sleepScreenChanged=*/true, /*quickResumeTimeoutChanged=*/false);
}

void SettingRowEditor::syncQuickResumeTimeoutForSleepScreen(bool sleepScreenChanged, bool quickResumeTimeoutChanged) {
  if (quickResumeTimeoutChanged) {
    preserveQuickResumeTimeoutOn =
        SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
    quickResumeTimeoutAutoEnabled = false;
  }

  if (SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::QUICK_RESUME) {
    if (SETTINGS.quickResumeSleepScreen != CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT) {
      SETTINGS.quickResumeSleepScreen = CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
      quickResumeTimeoutAutoEnabled = !preserveQuickResumeTimeoutOn;
    } else if (sleepScreenChanged && !preserveQuickResumeTimeoutOn) {
      quickResumeTimeoutAutoEnabled = true;
    }
    return;
  }

  if (sleepScreenChanged && quickResumeTimeoutAutoEnabled && !preserveQuickResumeTimeoutOn) {
    SETTINGS.quickResumeSleepScreen = CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_NEVER;
    quickResumeTimeoutAutoEnabled = false;
  }
}

void SettingRowEditor::openSleepTimeoutPicker() {
  activity.startActivityForResult(
      std::make_unique<IntervalSelectionActivity>(
          renderer, mappedInput, "SleepTimeoutInterval", StrId::STR_TIME_TO_SLEEP, SETTINGS.sleepTimeoutMinutes,
          CrossPointSettings::MIN_SLEEP_TIMEOUT_MINUTES, CrossPointSettings::MAX_SLEEP_TIMEOUT_MINUTES, 1, 5,
          StrId::STR_SLEEP_TIMER_VALUE_FORMAT, false, StrId::STR_SLEEP_NEVER),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          SETTINGS.sleepTimeoutMinutes = static_cast<uint8_t>(std::get<IntervalResult>(result.data).value);
          host.saveSettings();
        }
        activity.requestUpdate();
      });
}

std::string SettingRowEditor::valueText(const SettingInfo& setting) {
  if (setting.action == SettingAction::HomeButton) return tr(STR_CONFIGURE);
  if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
    // Guard like the valueGetter branch below: a corrupt/migrated settings
    // byte must not index past the enum table.
    const uint8_t value = SETTINGS.*(setting.valuePtr);
    const auto enumLabels = setting.enumLabels();
    if (value >= enumLabels.size()) return "";
    return I18N.get(enumLabels[value]);
  }
  if (setting.type == SettingType::ENUM && setting.valueGetter) {
    const uint8_t value = setting.valueGetter();
    if (!setting.enumStringValues.empty() && value < setting.enumStringValues.size()) {
      return setting.enumStringValues[value];
    }
    const auto enumLabels = setting.enumLabels();
    if (value < enumLabels.size()) {
      return I18N.get(enumLabels[value]);
    }
    return "";
  }
  if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
    if (setting.nameId == StrId::STR_TIME_TO_SLEEP) {
      if (SETTINGS.sleepTimeoutMinutes >= CrossPointSettings::SLEEP_TIMEOUT_NEVER_MINUTES) {
        return tr(STR_SLEEP_NEVER);
      }
      char valueBuffer[32];
      snprintf(valueBuffer, sizeof(valueBuffer), tr(STR_SLEEP_TIMER_VALUE_FORMAT),
               static_cast<unsigned int>(SETTINGS.*(setting.valuePtr)));
      return valueBuffer;
    }
    return std::to_string(SETTINGS.*(setting.valuePtr));
  }
  return "";
}
