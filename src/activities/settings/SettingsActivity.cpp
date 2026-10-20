#include "SettingsActivity.h"

#include <BlePageTurner.h>
#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <LibraryBuilder.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "AboutActivity.h"
#include "BleSettingsActivity.h"
#include "ButtonRemapActivity.h"
#include "ClearCacheActivity.h"
#include "ClockSettingsActivity.h"
#include "CrossPointSettings.h"
#include "FontDownloadActivity.h"
#include "HomeButtonSettingsActivity.h"
#include "KOReaderSettingsActivity.h"
#include "KeyboardLayoutsActivity.h"
#include "LanguageSelectActivity.h"
#include "MappedInputManager.h"
#include "OpdsServerListActivity.h"
#include "OtaUpdateActivity.h"
#include "SdCardFontSystem.h"
#include "SdFirmwareUpdateActivity.h"
#include "SettingsList.h"
#include "SilentRestart.h"
#include "StatusBarSettingsActivity.h"
#include "TextSettingsActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/IntervalSelectionActivity.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"
#include "util/PluginEvents.h"

namespace fui = freeink::ui;

namespace {
// Shared-list filtering for this board; false drops the row from the device UI
// (it stays in the shared list for the web settings API).
SettingsSection sectionFor(const SettingInfo& setting);

bool showOnDevice(const SettingInfo& setting) {
  if (sectionFor(setting) == SettingsSection::End || home_button::isSetting(setting.valuePtr)) return false;
  // The sunlight fading fix is a grayscale-waveform compensation that does
  // not apply on the X4 Pro / X4 Classic (plain OTP waveform, same panels).
  if (setting.valuePtr == &CrossPointSettings::fadingFix && (BoardConfig::isX4Pro() || BoardConfig::isX4Classic())) {
    return false;
  }
  // Merged into the Text Settings screen.
  if (setting.category == StrId::STR_CAT_READER && setting.inTextSettings) return false;
  if (setting.valuePtr == &CrossPointSettings::pwrBtnFootnoteBack &&
      SETTINGS.shortPwrBtn != CrossPointSettings::SHORT_PWRBTN::FOOTNOTES) {
    return false;
  }
  // The per-direction gestures only apply while Touch Reader Controls is on.
  if ((setting.valuePtr == &CrossPointSettings::pageTurnGesture ||
       setting.valuePtr == &CrossPointSettings::previousPageGesture) &&
      SETTINGS.touchReaderControls == CrossPointSettings::TOUCH_READER_OFF) {
    return false;
  }
  // Both only feed the CrossPoint list view: the cover grid keeps its own index and lists every
  // book the card scan finds, so a book dropped from recents is simply found again.
  if ((setting.valuePtr == &CrossPointSettings::libraryUseMetadata ||
       setting.valuePtr == &CrossPointSettings::removeReadBooksFromRecents) &&
      SETTINGS.libraryView != CrossPointSettings::LIBRARY_VIEW_LIST) {
    return false;
  }
  return true;
}

// End = not a row here: other categories (KOReader Sync, Status Bar, ...)
// only group the web UI; their rows live on their own sub-screens.
// Shortcuts belongs to Controls but opens as a sub-screen from a row in it
// (isSubScreenCategory); a new sub-screen category goes in both functions.
SettingsSection sectionFor(const SettingInfo& setting) {
  if (setting.category == StrId::STR_CAT_DISPLAY) return SettingsSection::Display;
  if (setting.category == StrId::STR_CAT_READER) return SettingsSection::Reader;
  if (setting.category == StrId::STR_CAT_CONTROLS) return SettingsSection::Controls;
  if (setting.category == StrId::STR_CAT_SYSTEM) return SettingsSection::System;
  if (setting.category == StrId::STR_CAT_LIBRARY) return SettingsSection::Library;
  if (setting.category == StrId::STR_CAT_SLEEP) return SettingsSection::Sleep;
  if (setting.category == StrId::STR_CAT_SHORTCUTS) return SettingsSection::Controls;
  return SettingsSection::End;
}

bool isSubScreenCategory(const StrId category) { return category == StrId::STR_CAT_SHORTCUTS; }

// Device-only action rows added per section, beyond the shared list (System has the most).
constexpr size_t EXTRA_ROWS_PER_SECTION = 8;
}  // namespace

void SettingsActivity::rebuildLibraryIndex() {
  // Prevent SD-backed fonts from opening a second reader while EPUB metadata is scanned.
  // Keep the popup static because an e-ink refresh per folder would dominate the rebuild.
  RenderLock lock(*this);
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
  requestUpdate(true);
}

void SettingsActivity::rebuildSettings() {
  // Pick up any fonts uploaded/deleted over the web server since the last
  // reader activity ran — otherwise the font-family picker shows stale list.
  sdFontSystem.refreshIfDirty();

  // Rescan /dictionaries on every rebuild of Reader: cheap (one directory listing) and
  // picks up dictionaries copied to the SD card since the last visit.
  std::vector<DictionaryEntry> dictionaries;
  if (section == SettingsSection::Reader) DictionaryRegistry::discover(dictionaries);

  // Copy only the rows of the one category shown here; each section's title is its category
  // (Connectivity's matches none: it has only action rows).
  const StrId category =
      subCategory != StrId::STR_NONE_OPT ? subCategory : SETTINGS_SECTION_TITLES[static_cast<size_t>(section)];
  auto all = getSettingsList(&sdFontSystem.registry(), &dictionaries, category);

  const auto belongsHere = [this](const SettingInfo& setting) {
    if (!showOnDevice(setting)) return false;
    if (subCategory != StrId::STR_NONE_OPT) return setting.category == subCategory;
    return sectionFor(setting) == section && !isSubScreenCategory(setting.category);
  };
  const size_t count = std::count_if(all.begin(), all.end(), belongsHere);
  settings.clear();
  settings.reserve(count + EXTRA_ROWS_PER_SECTION);

  const auto addAction = [this](SettingsSection target, StrId name, SettingAction action) {
    if (target == section && subCategory == StrId::STR_NONE_OPT) {
      settings.push_back(SettingInfo::Action(name, action));
    }
  };

  // Sub-screens and per-board configurators lead their section, above its own rows.
  addAction(SettingsSection::Reader, StrId::STR_TEXT_SETTINGS, SettingAction::TextSettings);
  addAction(SettingsSection::Controls, StrId::STR_CAT_SHORTCUTS, SettingAction::ShortcutsSettings);
  if (!BoardConfig::hasTouch()) {
    addAction(SettingsSection::Controls, StrId::STR_REMAP_FRONT_BUTTONS, SettingAction::RemapFrontButtons);
  }
  // The Home key's gestures lead the Shortcuts sub-screen.
  if (subCategory == StrId::STR_CAT_SHORTCUTS && BoardConfig::hasHomeKey()) {
    settings.push_back(SettingInfo::Action(StrId::STR_HOME_BUTTON, SettingAction::HomeButton));
  }

  for (auto& setting : all) {
    if (!belongsHere(setting)) continue;
    settings.push_back(std::move(setting));
  }

  // Rebuild sits directly under the view picker. It rebuilds the CLX1 index, so it is offered
  // only when that index is what the Library entry opens.
  if (section == SettingsSection::Library && !settings.empty() &&
      SETTINGS.libraryView == CrossPointSettings::LIBRARY_VIEW_LIST) {
    settings.insert(settings.begin() + 1,
                    SettingInfo::Action(StrId::STR_LIBRARY_REBUILD, SettingAction::RebuildLibraryIndex));
  }

  // No Manage Fonts row: it sits at the bottom of the font list inside Text Settings.
  addAction(SettingsSection::Reader, StrId::STR_CUSTOMISE_STATUS_BAR, SettingAction::CustomiseStatusBar);

  addAction(SettingsSection::System, StrId::STR_ABOUT, SettingAction::About);
  // On every board: Matcha keeps a system clock without an RTC, and the zone picked here decides
  // reading-stats day boundaries.
  addAction(SettingsSection::System, StrId::STR_CLOCK, SettingAction::ClockSettings);
  addAction(SettingsSection::System, StrId::STR_CLEAR_READING_CACHE, SettingAction::ClearCache);
  addAction(SettingsSection::System, StrId::STR_LANGUAGE, SettingAction::Language);
  addAction(SettingsSection::System, StrId::STR_KEYBOARD_LAYOUTS, SettingAction::KeyboardLayouts);
  addAction(SettingsSection::System, StrId::STR_PLUGINS, SettingAction::Plugins);
  // Firmware updates last: they replace the running image. OTA fetches this board's own release
  // asset (see OtaUpdater). The Picco's stock layout has one app slot (partitions_picco.csv), so
  // it cannot update itself either way.
  if (!BoardConfig::isPicco()) {
    addAction(SettingsSection::System, StrId::STR_CHECK_UPDATES, SettingAction::CheckForUpdates);
    addAction(SettingsSection::System, StrId::STR_SD_FIRMWARE_UPDATE, SettingAction::SdFirmwareUpdate);
  }

  addAction(SettingsSection::Connectivity, StrId::STR_WIFI_NETWORKS, SettingAction::Network);
  if (bleturner::status().compiledIn) {
    addAction(SettingsSection::Connectivity, StrId::STR_BT_PAGE_TURNER, SettingAction::Bluetooth);
  }
  addAction(SettingsSection::Connectivity, StrId::STR_KOREADER_SYNC, SettingAction::KOReaderSync);
  addAction(SettingsSection::Connectivity, StrId::STR_OPDS_SERVERS, SettingAction::OPDSBrowser);
  rebuildRowItems();
}

SettingsActivity::SettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                   const SettingsSection section, const StrId subCategory)
    : UiListActivity("Settings", renderer, mappedInput), section(section), subCategory(subCategory) {}

void SettingsActivity::onEnter() {
  UiListActivity::onEnter();

  preserveQuickResumeTimeoutOn =
      SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
  quickResumeTimeoutAutoEnabled = false;
  syncQuickResumeTimeoutForSleepScreen(/*sleepScreenChanged=*/true, /*quickResumeTimeoutChanged=*/false);

  rebuildSettings();
}

// Rebuilds rowValues_/rowItems_ (label + actionValue) for settings.
// Structural — call only when the setting list changes, never from
// buildScreen(), which only refreshes rowValues_ content and rowItems_[].value
// pointers in place.
void SettingsActivity::rebuildRowItems() {
  rowValues_.assign(settings.size(), std::string());
  rowItems_.clear();
  rowItems_.reserve(settings.size());
  for (size_t i = 0; i < settings.size(); i++) {
    fui::ListItem item;
    item.label = I18N.get(settings[i].nameId);
    item.actionValue = static_cast<int16_t>(i);
    rowItems_.push_back(item);
  }
}

const char* SettingsActivity::headerTitle() const {
  if (subCategory != StrId::STR_NONE_OPT) return I18N.get(subCategory);
  return I18N.get(SETTINGS_SECTION_TITLES[static_cast<size_t>(section)]);
}

void SettingsActivity::activateIndex(const int index) {
  if (optionPopup.isActive()) return;
  // Most rows repaint a different surface (popup, sub-activity, new value);
  // a lingering tap flash would gray an unrelated element.
  app.clearTapFlash();
  nav.selected = index;
  toggleSelectedSetting();
  requestUpdate();
}

void SettingsActivity::applyUiSettingChange(uint8_t CrossPointSettings::* valuePtr) {
  // Theme changes take effect immediately, on this screen — reload the theme
  // and re-derive the app's tokens so the very next repaint is in the new look.
  if (valuePtr != &CrossPointSettings::uiTheme) {
    return;
  }
  UITheme::getInstance().reload();
  // Re-derive the shared tokens for the new look; the gate stays closed until
  // the repaint that rebuilds the interaction table in the new layout.
  resetUi();
}

bool SettingsActivity::handleCustomInput() {
  return optionPopup.handleInput(mappedInput, [this] { requestUpdate(); });
}

void SettingsActivity::toggleSelectedSetting() {
  mappedInput.resetHomeButtonInput();
  const int selectedSetting = nav.selected;
  if (selectedSetting < 0 || selectedSetting >= static_cast<int>(settings.size())) {
    return;
  }

  const auto& setting = settings[selectedSetting];
  const bool sleepScreenChanged = setting.valuePtr == &CrossPointSettings::sleepScreen;
  const bool quickResumeTimeoutChanged = setting.valuePtr == &CrossPointSettings::quickResumeSleepScreen;

  if (setting.nameId == StrId::STR_TIME_TO_SLEEP) {
    openSleepTimeoutPicker();
    return;
  }

  if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
    // Toggle the boolean value using the member pointer
    const bool currentValue = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = !currentValue;
  } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
    const uint8_t currentValue = SETTINGS.*(setting.valuePtr);
    // The popup works in menu slots, which are stored values only when the row asked for no
    // custom order -- see SettingInfo::enumOrder.
    const auto orderedLabels = setting.orderedEnumLabels();
    if (orderedLabels.size() > 2) {
      const auto valuePtr = setting.valuePtr;
      const auto order = setting.enumOrder;
      optionPopup.show(setting.nameId, orderedLabels.data(), static_cast<int>(orderedLabels.size()),
                       setting.slotFromStored(currentValue),
                       [this, valuePtr, order, sleepScreenChanged, quickResumeTimeoutChanged](int idx) {
                         SETTINGS.*valuePtr = order.empty() ? static_cast<uint8_t>(idx)
                                                            : order[idx < static_cast<int>(order.size()) ? idx : 0];
                         syncQuickResumeTimeoutForSleepScreen(sleepScreenChanged, quickResumeTimeoutChanged);
                         SETTINGS.saveToFile();
                         rebuildSettings();
                         applyUiSettingChange(valuePtr);
                       });
      requestUpdate();
      return;
    }
    SETTINGS.*(setting.valuePtr) =
        setting.storedFromSlot((setting.slotFromStored(currentValue) + 1) % static_cast<uint8_t>(orderedLabels.size()));
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
        SETTINGS.saveToFile();
        rebuildSettings();
      };
      if (!setting.enumStringValues.empty()) {
        optionPopup.show(setting.nameId, setting.enumStringValues, cur, std::move(onSelect));
      } else {
        const auto enumLabels = setting.enumLabels();
        optionPopup.show(setting.nameId, enumLabels.data(), static_cast<int>(enumLabels.size()), cur,
                         std::move(onSelect));
      }
      requestUpdate();
      return;
    }
    setting.valueSetter((cur + 1) % totalValues);
  } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
    const int8_t currentValue = SETTINGS.*(setting.valuePtr);
    if (currentValue + setting.valueRange.step > setting.valueRange.max) {
      SETTINGS.*(setting.valuePtr) = setting.valueRange.min;
    } else {
      SETTINGS.*(setting.valuePtr) = currentValue + setting.valueRange.step;
    }
  } else if (setting.type == SettingType::ACTION) {
    auto resultHandler = [this](const ActivityResult&) { SETTINGS.saveToFile(); };

    switch (setting.action) {
      case SettingAction::ShortcutsSettings: {
        auto activity = makeUniqueNoThrow<SettingsActivity>(renderer, mappedInput, section, StrId::STR_CAT_SHORTCUTS);
        if (!activity) {
          LOG_ERR("SET", "OOM: settings sub-screen");
          return;
        }
        startActivityForResult(std::move(activity), [this](const ActivityResult&) { requestUpdate(); });
        return;
      }
      case SettingAction::HomeButton: {
        // Activities must outlive this call and are owned by the activity stack.
        auto activity = makeUniqueNoThrow<HomeButtonSettingsActivity>(renderer, mappedInput);
        if (!activity) {
          LOG_ERR("SET", "OOM: Home button settings");
          return;
        }
        startActivityForResult(std::move(activity), [this](const ActivityResult&) { requestUpdate(); });
        return;
      }
      case SettingAction::RemapFrontButtons:
        startActivityForResult(std::make_unique<ButtonRemapActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::CustomiseStatusBar:
        startActivityForResult(std::make_unique<StatusBarSettingsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::ClockSettings:
        if (auto activity = makeUniqueNoThrow<ClockSettingsActivity>(renderer, mappedInput)) {
          startActivityForResult(std::move(activity), resultHandler);
        } else {
          LOG_ERR("SETTINGS", "OOM: ClockSettingsActivity");
        }
        break;
      case SettingAction::KOReaderSync:
        startActivityForResult(std::make_unique<KOReaderSettingsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::OPDSBrowser:
        startActivityForResult(std::make_unique<OpdsServerListActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::Network: {
        auto activity = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput, false);
        if (!activity) {
          LOG_ERR("SETTINGS", "OOM: WifiSelectionActivity");
          return;
        }
        startActivityForResult(std::move(activity), [](const ActivityResult&) {
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
        break;
      }
      case SettingAction::Bluetooth: {
        auto activity = makeUniqueNoThrow<BleSettingsActivity>(renderer, mappedInput);
        if (!activity) {
          LOG_ERR("SETTINGS", "OOM: BleSettingsActivity");
          return;
        }
        startActivityForResult(std::move(activity), resultHandler);
        break;
      }
      case SettingAction::RebuildLibraryIndex:
        rebuildLibraryIndex();
        return;
      case SettingAction::ClearCache:
        startActivityForResult(std::make_unique<ClearCacheActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::CheckForUpdates:
        startActivityForResult(std::make_unique<OtaUpdateActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::SdFirmwareUpdate:
        startActivityForResult(std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::DownloadFonts:
        startActivityForResult(std::make_unique<FontDownloadActivity>(renderer, mappedInput),
                               [this](const ActivityResult&) {
                                 SETTINGS.saveToFile();
                                 rebuildSettings();
                               });
        break;
      case SettingAction::TextSettings:
        // No book here, so the Size and Layout tabs offer Edit For: the Latin or the CJK profile.
        startActivityForResult(std::make_unique<TextSettingsActivity>(renderer, mappedInput, &sdFontSystem.registry(),
                                                                      TextSettingsActivity::Tab::Family,
                                                                      CjkScript::None, /*verticalText=*/false,
                                                                      /*profileChoice=*/true),
                               [this](const ActivityResult&) {
                                 // Not every row there saves, and leaving with the CJK profile on
                                 // screen swaps the profiles back only in memory.
                                 SETTINGS.saveToFile();
                                 rebuildSettings();
                               });
        break;
      case SettingAction::Language:
        // Row labels are translated once in rebuildRowItems() and don't
        // re-run on Pop (see ActivityManager::loop()), so a language switch
        // needs an explicit rebuild here rather than the generic resultHandler.
        startActivityForResult(std::make_unique<LanguageSelectActivity>(renderer, mappedInput),
                               [this](const ActivityResult&) {
                                 SETTINGS.saveToFile();
                                 rebuildSettings();
                               });
        break;
      case SettingAction::Plugins:
        // A switch row: flips in place.
        SETTINGS.pluginsEnabled = SETTINGS.pluginsEnabled ? 0 : 1;
        // Turning plugins on brings back a dismissed Plugin Hub install row.
        if (SETTINGS.pluginsEnabled) SETTINGS.pluginHubPromptHidden = 0;
        SETTINGS.saveToFile();
        // Applies to event delivery now, not after a reboot.
        pluginevents::refreshSubscriptions();
        return;
      case SettingAction::KeyboardLayouts:
        if (auto activity = makeUniqueNoThrow<KeyboardLayoutsActivity>(renderer, mappedInput)) {
          startActivityForResult(std::move(activity), nullptr);
        } else {
          LOG_ERR("SETTINGS", "OOM: KeyboardLayoutsActivity");
        }
        break;
      case SettingAction::About:
        if (auto activity = makeUniqueNoThrow<AboutActivity>(renderer, mappedInput)) {
          startActivityForResult(std::move(activity), nullptr);
        } else {
          LOG_ERR("SETTINGS", "OOM: AboutActivity");
        }
        break;
      case SettingAction::None:
        // Do nothing
        break;
    }
    return;  // Results will be handled in the result handler, so we can return early here
  } else {
    return;
  }

  syncQuickResumeTimeoutForSleepScreen(sleepScreenChanged, quickResumeTimeoutChanged);
  SETTINGS.saveToFile();
  const auto valuePtr = setting.valuePtr;  // rebuildSettings() replaces `setting`
  rebuildSettings();
  applyUiSettingChange(valuePtr);
  // Rows can appear/disappear with a change (e.g. the footnote-back toggle).
  nav.selected = std::min(selectedSetting, static_cast<int>(settings.size()) - 1);
}

void SettingsActivity::syncQuickResumeTimeoutForSleepScreen(bool sleepScreenChanged, bool quickResumeTimeoutChanged) {
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

void SettingsActivity::openSleepTimeoutPicker() {
  startActivityForResult(
      std::make_unique<IntervalSelectionActivity>(
          renderer, mappedInput, "SleepTimeoutInterval", StrId::STR_TIME_TO_SLEEP, SETTINGS.sleepTimeoutMinutes,
          CrossPointSettings::MIN_SLEEP_TIMEOUT_MINUTES, CrossPointSettings::MAX_SLEEP_TIMEOUT_MINUTES, 1, 5,
          StrId::STR_SLEEP_TIMER_VALUE_FORMAT, false, StrId::STR_SLEEP_NEVER),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          SETTINGS.sleepTimeoutMinutes = static_cast<uint8_t>(std::get<IntervalResult>(result.data).value);
          SETTINGS.saveToFile();
        }
        requestUpdate();
      });
}

std::string SettingsActivity::settingValueText(const SettingInfo& setting) {
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

void SettingsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the GUI.drawHeader band, above the button hints.
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  // rowItems_ (label/actionValue) was built by rebuildRowItems(); only the
  // live value text needs refreshing here, by assigning into the existing
  // rowValues_ strings (no vector growth).
  for (size_t i = 0; i < settings.size(); i++) {
    const auto& setting = settings[i];
    const auto labels = setting.enumLabels();
    const bool checkbox = setting.type == SettingType::TOGGLE ||
                          (setting.type == SettingType::ENUM && setting.enumStringValues.empty() &&
                           labels.size() == 2 && labels[0] == StrId::STR_STATE_OFF && labels[1] == StrId::STR_STATE_ON);
    if (setting.action == SettingAction::Plugins) {
      rowValues_[i].clear();
      GUI.setCheckboxRow(rowItems_[i], SETTINGS.pluginsEnabled != 0);
    } else if (checkbox && (setting.valuePtr || setting.valueGetter)) {
      const bool checked = setting.valuePtr ? SETTINGS.*(setting.valuePtr) != 0 : setting.valueGetter() != 0;
      rowValues_[i].clear();
      GUI.setCheckboxRow(rowItems_[i], checked);
    } else {
      rowValues_[i] = settingValueText(setting);
      rowItems_[i].value = rowValues_[i].empty() ? nullptr : rowValues_[i].c_str();
    }
  }

  fui::ListProps props;
  props.items = rowItems_.data();
  props.count = static_cast<uint16_t>(rowItems_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;               // air between the value and the row edge
  applySwitchStyle(props);            // pill switches, as everywhere else
  // Touch uses the standard list sizing, as in Bluetooth; buttons keep compact labels.
  // maxLines also marks smallText as explicit so the list does not replace it with bodyText.
  props.labelText = mappedInput.hasTouch() ? screen.theme().bodyText : screen.theme().smallText;
  props.labelText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}

void SettingsActivity::drawFooter() {
  const int selected = nav.selected;
  const bool picker =
      selected >= 0 && selected < static_cast<int>(settings.size()) &&
      ((settings[selected].type == SettingType::ACTION && settings[selected].action != SettingAction::Plugins) ||
       settings[selected].nameId == StrId::STR_TIME_TO_SLEEP);
  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), picker ? tr(STR_SELECT) : tr(STR_TOGGLE), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void SettingsActivity::render(RenderLock&& lock) {
  if (optionPopup.processRender(renderer, mappedInput)) return;
  UiListActivity::render(std::move(lock));
}
