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
#include "EnumSettingPopup.h"
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
#include "TextProfileMenuActivity.h"
#include "TranslationSettingsActivity.h"
#include "activities/flashcards/AnkiSyncActivity.h"
#include "activities/home/StatsSyncActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/IntervalSelectionActivity.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "flashcards/FlashcardStore.h"
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
      SETTINGS.shortPwrBtn != CrossPointSettings::SHORT_PWRBTN::FOOTNOTES &&
      !(BoardConfig::isX4Pro() && SETTINGS.doublePwrBtn == CrossPointSettings::FOOTNOTES)) {
    return false;
  }
  // The per-direction gestures only apply while Touch Reader Controls is on.
  if ((setting.valuePtr == &CrossPointSettings::pageTurnGesture ||
       setting.valuePtr == &CrossPointSettings::previousPageGesture) &&
      SETTINGS.touchReaderControls == CrossPointSettings::TOUCH_READER_OFF) {
    return false;
  }
  // Only the language tabs have an Unknown one to hide.
  if (setting.valuePtr == &CrossPointSettings::hideUnknownLanguages && !SETTINGS.showLanguageStats) return false;
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
// Shortcuts rows sit in the Controls groups; in a book, Reader Settings lists them alone.
SettingsSection sectionFor(const SettingInfo& setting) {
  if (setting.category == StrId::STR_CAT_DISPLAY) return SettingsSection::Display;
  if (setting.category == StrId::STR_CAT_READER) return SettingsSection::Reader;
  if (setting.category == StrId::STR_CAT_CONTROLS) return SettingsSection::Controls;
  if (setting.category == StrId::STR_CAT_SYSTEM) return SettingsSection::System;
  if (setting.category == StrId::STR_CAT_LIBRARY) return SettingsSection::Library;
  if (setting.category == StrId::STR_CAT_SLEEP) return SettingsSection::Sleep;
  if (setting.category == StrId::STR_STATS) return SettingsSection::Insights;
  if (setting.category == StrId::STR_FLASHCARDS) return SettingsSection::Flashcards;
  if (setting.category == StrId::STR_CAT_SHORTCUTS) return SettingsSection::Controls;
  return SettingsSection::End;
}

// Position of a row on the Dictionary Settings sub-screen, or -1 for a row it does not show.
// These Reader rows move there from the Reader page but keep their Reader category, so Reader
// Settings in a book and the web settings page are unchanged.
int dictionaryRowOrder(const SettingInfo& setting) {
  switch (setting.nameId) {
    case StrId::STR_WORD_LOOKUP_FONT_SIZE:
      return 0;
    case StrId::STR_DICT_STATUS_JA:
      return 1;
    case StrId::STR_DICT_STATUS_ZH:
      return 2;
    case StrId::STR_DICT_STATUS_YUE:
      return 3;
    case StrId::STR_FALLBACK_DICTIONARY:
      return 4;
    default:
      return -1;
  }
}

// The shared long-press setting deliberately appears in both button groups.
bool belongsToControlsGroup(const SettingInfo& setting, ControlsGroup group) {
  const auto field = setting.valuePtr;
  const bool power = field == &CrossPointSettings::shortPwrBtn || field == &CrossPointSettings::powerUpAction ||
                     (setting.key && std::strcmp(setting.key, "doublePwrBtn") == 0);
  const bool menu = setting.key && std::strcmp(setting.key, "longPressMenuFunction") == 0;
  const bool shared = field == &CrossPointSettings::longPressButtonBehavior;
  switch (group) {
    case ControlsGroup::Power:
      return power || field == &CrossPointSettings::pwrBtnFootnoteBack;
    case ControlsGroup::Front:
      return shared || menu || field == &CrossPointSettings::frontButtonFollowOrientation ||
             field == &CrossPointSettings::backShortToFileBrowser;
    case ControlsGroup::Side:
      return shared || field == &CrossPointSettings::upperSideButtonAction ||
             field == &CrossPointSettings::lowerSideButtonAction;
    case ControlsGroup::Touch:
      return field == &CrossPointSettings::touchReaderControls || field == &CrossPointSettings::pageTurnGesture ||
             field == &CrossPointSettings::tiltPageTurn || field == &CrossPointSettings::previousPageGesture ||
             field == &CrossPointSettings::showReaderMenu;
    case ControlsGroup::Root:
      return !belongsToControlsGroup(setting, ControlsGroup::Power) &&
             !belongsToControlsGroup(setting, ControlsGroup::Front) &&
             !belongsToControlsGroup(setting, ControlsGroup::Side) &&
             !belongsToControlsGroup(setting, ControlsGroup::Touch);
  }
  return false;
}

// Device-only action rows added per section, beyond the shared list (System has the most).
constexpr size_t EXTRA_ROWS_PER_SECTION = 8;
}  // namespace

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
  const bool dictionaryPage = subCategory == StrId::STR_DICTIONARY_SETTINGS;
  // Controls takes the Shortcuts rows too: its groups sort both categories.
  const bool controlsPage = section == SettingsSection::Controls && subCategory == StrId::STR_NONE_OPT;
  const StrId category = subCategory != StrId::STR_NONE_OPT && !dictionaryPage ? subCategory
                         : controlsPage                                        ? StrId::STR_NONE_OPT
                                        : SETTINGS_SECTION_TITLES[static_cast<size_t>(section)];
  auto all = getSettingsList(&sdFontSystem.registry(), &dictionaries, category);

  const auto belongsHere = [this, dictionaryPage](const SettingInfo& setting) {
    // Anki Sync's rows are kept off the Connectivity list (sectionFor() has no section for them)
    // and shown on their own page.
    if (subCategory == StrId::STR_ANKI_SYNC) return setting.category == StrId::STR_ANKI_SYNC;
    if (!showOnDevice(setting)) return false;
    if (dictionaryPage) return dictionaryRowOrder(setting) >= 0;
    if (subCategory != StrId::STR_NONE_OPT) return setting.category == subCategory;
    if (sectionFor(setting) != section || dictionaryRowOrder(setting) >= 0) return false;
    // Controls shows its rows by group; in a book, Reader Settings opens the Shortcuts rows alone.
    if (section == SettingsSection::Controls) return belongsToControlsGroup(setting, controlsGroup);
    return true;
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
  addAction(SettingsSection::Reader, StrId::STR_DICTIONARY_SETTINGS, SettingAction::DictionarySettings);
  addAction(SettingsSection::Reader, StrId::STR_TRANSLATION_SETTINGS, SettingAction::TranslationSettings);
  if (section == SettingsSection::Controls && subCategory == StrId::STR_NONE_OPT) {
    if (controlsGroup == ControlsGroup::Root) {
      if (BoardConfig::hasHomeKey()) {
        addAction(section, StrId::STR_HOME_BUTTON, SettingAction::HomeButton);
      }
      const auto addGroup = [&](ControlsGroup group, StrId title, SettingAction action) {
        if (std::any_of(all.begin(), all.end(),
                        [&](const SettingInfo& setting) {
                          return sectionFor(setting) == section && showOnDevice(setting) &&
                                 belongsToControlsGroup(setting, group);
                        }) ||
            (group == ControlsGroup::Front && !BoardConfig::hasTouch())) {
          addAction(section, title, action);
        }
      };
      addGroup(ControlsGroup::Power, StrId::STR_POWER_BUTTON, SettingAction::ControlsPower);
      if (!BoardConfig::hasTouch()) {
        addGroup(ControlsGroup::Front, StrId::STR_FRONT_BUTTONS, SettingAction::ControlsFront);
      }
      addGroup(ControlsGroup::Side, StrId::STR_SIDE_BUTTONS, SettingAction::ControlsSide);
      addGroup(ControlsGroup::Touch, StrId::STR_TAPS_AND_GESTURES, SettingAction::ControlsTouch);
    } else if (controlsGroup == ControlsGroup::Front && !BoardConfig::hasTouch()) {
      addAction(section, StrId::STR_REMAP_FRONT_BUTTONS, SettingAction::RemapFrontButtons);
    }
  }
  // The Home key's gestures lead the Shortcuts sub-screen.
  if (subCategory == StrId::STR_CAT_SHORTCUTS && BoardConfig::hasHomeKey()) {
    settings.push_back(SettingInfo::Action(StrId::STR_HOME_BUTTON, SettingAction::HomeButton));
  }

  for (auto& setting : all) {
    if (!belongsHere(setting)) continue;
    if (section == SettingsSection::Controls && subCategory == StrId::STR_NONE_OPT &&
        setting.valuePtr == &CrossPointSettings::longPressButtonBehavior) {
      setting.nameId = StrId::STR_PAGE_TURN_LONG_PRESS;
    }
    settings.push_back(std::move(setting));
  }
  if (section == SettingsSection::Controls && controlsGroup == ControlsGroup::Front) {
    for (const char* key : {"longPressButtonBehavior", "longPressMenuFunction"}) {
      const auto row = std::find_if(settings.begin(), settings.end(), [key](const SettingInfo& setting) {
        return setting.key && std::strcmp(setting.key, key) == 0;
      });
      if (row != settings.end()) std::rotate(row, row + 1, settings.end());
    }
  }
  if (dictionaryPage) {
    std::sort(settings.begin(), settings.end(),
              [](const SettingInfo& a, const SettingInfo& b) { return dictionaryRowOrder(a) < dictionaryRowOrder(b); });
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

  addAction(SettingsSection::Insights, StrId::STR_SYNC_STATS, SettingAction::SyncStats);
  // Right under Enable Flashcards: works in every theme, with or without the tab.
  if (section == SettingsSection::Flashcards) {
    settings.insert(settings.begin() + (settings.empty() ? 0 : 1),
                    SettingInfo::Action(StrId::STR_FLASH_OPEN, SettingAction::OpenFlashcards));
  }
  addAction(SettingsSection::Connectivity, StrId::STR_WIFI_NETWORKS, SettingAction::Network);
  if (bleturner::status().compiledIn) {
    addAction(SettingsSection::Connectivity, StrId::STR_BT_PAGE_TURNER, SettingAction::Bluetooth);
  }
  addAction(SettingsSection::Connectivity, StrId::STR_KOREADER_SYNC, SettingAction::KOReaderSync);
  addAction(SettingsSection::Connectivity, StrId::STR_OPDS_SERVERS, SettingAction::OPDSBrowser);
  addAction(SettingsSection::Connectivity, StrId::STR_ANKI_SYNC, SettingAction::AnkiSync);
  rebuildRowItems();
}

SettingsActivity::SettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                   const SettingsSection section, const StrId subCategory,
                                   const ControlsGroup controlsGroup)
    : UiListActivity("Settings", renderer, mappedInput),
      section(section),
      subCategory(subCategory),
      controlsGroup(controlsGroup) {}

void SettingsActivity::onEnter() {
  UiListActivity::onEnter();

  editor.beginQuickResumeSync();

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
  switch (controlsGroup) {
    case ControlsGroup::Power:
      return tr(STR_POWER_BUTTON);
    case ControlsGroup::Front:
      return tr(STR_FRONT_BUTTONS);
    case ControlsGroup::Side:
      return tr(STR_SIDE_BUTTONS);
    case ControlsGroup::Touch:
      return tr(STR_TAPS_AND_GESTURES);
    case ControlsGroup::Root:
      break;
  }
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

void SettingsActivity::saveSettings() { SETTINGS.saveToFile(); }

void SettingsActivity::commitSettingChange(uint8_t CrossPointSettings::* valuePtr) {
  SETTINGS.saveToFile();
  rebuildSettings();
  applyUiSettingChange(valuePtr);
}

void SettingsActivity::toggleSelectedSetting() {
  mappedInput.resetHomeButtonInput();
  const int selectedSetting = nav.selected;
  if (selectedSetting < 0 || selectedSetting >= static_cast<int>(settings.size())) {
    return;
  }

  const auto& setting = settings[selectedSetting];
  if (setting.type != SettingType::ACTION) {
    if (editor.editValue(setting) == SettingRowEditor::EditResult::Changed) {
      if (setting.key && std::strcmp(setting.key, "flashcardVacation") == 0) {
        flashcards::syncVacation();
      }
      // Rows can appear/disappear with a change (e.g. the footnote-back toggle).
      nav.selected = std::min(selectedSetting, static_cast<int>(settings.size()) - 1);
    }
    return;
  }

  switch (setting.action) {
    case SettingAction::ControlsPower:
    case SettingAction::ControlsFront:
    case SettingAction::ControlsSide:
    case SettingAction::ControlsTouch: {
      ControlsGroup group = ControlsGroup::Root;
      switch (setting.action) {
        case SettingAction::ControlsPower:
          group = ControlsGroup::Power;
          break;
        case SettingAction::ControlsFront:
          group = ControlsGroup::Front;
          break;
        case SettingAction::ControlsSide:
          group = ControlsGroup::Side;
          break;
        case SettingAction::ControlsTouch:
          group = ControlsGroup::Touch;
          break;
        default:
          break;  // Only the four group actions reach this branch.
      }
      // The activity stack owns the child until Back returns to this menu.
      auto activity = makeUniqueNoThrow<SettingsActivity>(renderer, mappedInput, SettingsSection::Controls,
                                                          StrId::STR_NONE_OPT, group);
      if (!activity) {
        LOG_ERR("SET", "OOM: Controls settings (%u bytes)", static_cast<unsigned>(sizeof(SettingsActivity)));
        return;
      }
      startActivityForResult(std::move(activity), [this](const ActivityResult&) { requestUpdate(); });
      return;
    }
    case SettingAction::ShortcutsSettings:
    case SettingAction::DictionarySettings: {
      const StrId page = setting.action == SettingAction::ShortcutsSettings ? StrId::STR_CAT_SHORTCUTS
                                                                            : StrId::STR_DICTIONARY_SETTINGS;
      auto activity = makeUniqueNoThrow<SettingsActivity>(renderer, mappedInput, section, page);
      if (!activity) {
        LOG_ERR("SET", "OOM: settings sub-screen");
        return;
      }
      startActivityForResult(std::move(activity), [this](const ActivityResult&) { requestUpdate(); });
      return;
    }
    case SettingAction::TranslationSettings: {
      auto activity = makeUniqueNoThrow<TranslationSettingsActivity>(renderer, mappedInput);
      if (!activity) {
        LOG_ERR("SET", "OOM: translation settings");
        return;
      }
      startActivityForResult(std::move(activity), [this](const ActivityResult&) { requestUpdate(); });
      return;
    }
    case SettingAction::AnkiSync: {
      auto activity = makeUniqueNoThrow<SettingsActivity>(renderer, mappedInput, SettingsSection::Connectivity,
                                                          StrId::STR_ANKI_SYNC);
      if (!activity) {
        LOG_ERR("SET", "OOM: Anki settings");
        return;
      }
      startActivityForResult(std::move(activity), [this](const ActivityResult&) { requestUpdate(); });
      return;
    }
    case SettingAction::AnkiTest: {
      auto test = makeUniqueNoThrow<AnkiSyncActivity>(renderer, mappedInput, AnkiSyncActivity::Mode::Test);
      if (!test) {
        LOG_ERR("SET", "OOM: Anki connection test");
        return;
      }
      startActivityForResult(std::move(test), [this](const ActivityResult&) { requestUpdate(); });
      return;
    }
    case SettingAction::OpenFlashcards:
      activityManager.goToFlashcards();
      return;
    case SettingAction::SyncStats:
      startActivityForResult(
          std::make_unique<StatsSyncActivity>(renderer, mappedInput, StatsSyncActivity::Origin::Settings),
          [](const ActivityResult&) { SETTINGS.saveToFile(); });
      return;
    case SettingAction::Bluetooth: {
      auto activity = makeUniqueNoThrow<BleSettingsActivity>(renderer, mappedInput);
      if (!activity) {
        LOG_ERR("SETTINGS", "OOM: BleSettingsActivity");
        return;
      }
      startActivityForResult(std::move(activity), [](const ActivityResult&) { SETTINGS.saveToFile(); });
      return;
    }
    case SettingAction::TextSettings:
      startActivityForResult(std::make_unique<TextProfileMenuActivity>(renderer, mappedInput),
                             [this](const ActivityResult&) { rebuildSettings(); });
      return;
    case SettingAction::Plugins:
      // A switch row: flips in place.
      SETTINGS.pluginsEnabled = SETTINGS.pluginsEnabled ? 0 : 1;
      // Turning plugins on brings back a dismissed Plugin Hub install row.
      if (SETTINGS.pluginsEnabled) SETTINGS.pluginHubPromptHidden = 0;
      SETTINGS.saveToFile();
      // Applies to event delivery now, not after a reboot.
      pluginevents::refreshSubscriptions();
      return;
    default:
      editor.launchSharedAction(setting.action);
      return;
  }
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
      rowValues_[i] = SettingRowEditor::valueText(setting);
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
  // A row with a value but no setter (the dictionary status rows) does nothing on Confirm, so it
  // gets no hint.
  const bool readOnly = selected >= 0 && selected < static_cast<int>(settings.size()) &&
                        settings[selected].type == SettingType::ENUM && !settings[selected].valuePtr &&
                        settings[selected].valueGetter && !settings[selected].valueSetter;
  const char* confirmLabel = readOnly ? "" : picker ? tr(STR_SELECT) : tr(STR_TOGGLE);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void SettingsActivity::render(RenderLock&& lock) {
  if (optionPopup.processRender(renderer, mappedInput)) return;
  UiListActivity::render(std::move(lock));
}
