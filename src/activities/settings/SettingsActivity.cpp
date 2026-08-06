#include "SettingsActivity.h"

#include <BoardConfig.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

#include "ButtonRemapActivity.h"
#include "ClearCacheActivity.h"
#include "CrossPointSettings.h"
#include "FontDownloadActivity.h"
#include "KOReaderSettingsActivity.h"
#include "LanguageSelectActivity.h"
#include "MappedInputManager.h"
#include "OpdsServerListActivity.h"
#include "OtaUpdateActivity.h"
#include "SdCardFontSystem.h"
#include "SdFirmwareUpdateActivity.h"
#include "SettingsList.h"
#include "StatusBarSettingsActivity.h"
#include "TextSettingsActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/IntervalSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

const StrId SettingsActivity::categoryNames[categoryCount] = {StrId::STR_CAT_DISPLAY, StrId::STR_CAT_READER,
                                                              StrId::STR_CAT_CONTROLS, StrId::STR_CAT_SYSTEM};

void SettingsActivity::saveSettings() {
  // Reader Settings keeps the EPUB, laid-out page and SD fonts resident. A font
  // preview can leave less contiguous heap than settings JSON needs, so reclaim
  // renderer-owned font tables before serializing. The reader restores them when
  // the settings activity returns.
  if (finishOnBack) {
    if (auto* fcm = renderer.getFontCacheManager()) fcm->releaseAllFontMemory();
  }
  SETTINGS.saveToFile();
}

void SettingsActivity::rebuildSettingsLists() {
  displaySettings.clear();
  readerSettings.clear();
  controlsSettings.clear();
  systemSettings.clear();

  // Pick up any fonts uploaded/deleted over the web server since the last
  // reader activity ran — otherwise the font-family picker shows stale list.
  sdFontSystem.refreshIfDirty();

  // Japanese books use their own dictionary flow, so omit the regular picker there.
  std::vector<DictionaryEntry> dictionaries;
  if (!japaneseBook && (!finishOnBack || selectedCategoryIndex == 1)) DictionaryRegistry::discover(dictionaries);

  // Reader-launched settings lock the UI to one category while the book remains
  // resident. Avoid materializing every web/device setting in that low-heap path.
  const StrId categoryFilter =
      finishOnBack ? categoryNames[selectedCategoryIndex] : StrId::STR_NONE_OPT;
  auto settings = getSettingsList(&sdFontSystem.registry(), &dictionaries, categoryFilter,
                                  /*includeTextSettingsEntries=*/!finishOnBack,
                                  dictionaryLanguage, finishOnBack);
  if (finishOnBack) {
    switch (selectedCategoryIndex) {
      case 0:
        displaySettings.reserve(settings.size());
        break;
      case 1:
        readerSettings.reserve(settings.size() + 4);
        break;
      case 2:
        controlsSettings.reserve(settings.size() + 1);
        break;
      case 3:
        systemSettings.reserve(settings.size() + 8);
        break;
    }
  }

  for (auto& setting : settings) {
    if (hideMangaOnlySettings &&
        setting.valuePtr == &CrossPointSettings::rotateMangaPanels) {
      continue;
    }
    if (setting.category == StrId::STR_NONE_OPT) continue;
    if (setting.category == StrId::STR_CAT_DISPLAY) {
      displaySettings.push_back(std::move(setting));
    } else if (setting.category == StrId::STR_CAT_READER) {
      // Settings merged into "Text Settings"
      // (they stay in the shared list for the web settings API)
      if (setting.inTextSettings) continue;
      // Manga pages ARE images -- the Display/Placeholder/Suppress image rendering mode has
      // nothing to render for a manga book, so it's hidden along with Text Settings below.
      if (mangaMode && setting.nameId == StrId::STR_IMAGES) continue;
      readerSettings.push_back(std::move(setting));
    } else if (setting.category == StrId::STR_CAT_CONTROLS) {
      if (setting.valuePtr == &CrossPointSettings::pwrBtnFootnoteBack &&
          SETTINGS.shortPwrBtn != CrossPointSettings::SHORT_PWRBTN::FOOTNOTES) {
        continue;
      }
      controlsSettings.push_back(std::move(setting));
    } else if (setting.category == StrId::STR_CAT_SYSTEM) {
      systemSettings.push_back(std::move(setting));
    }
  }

  // Append device-only ACTION items
  if ((!finishOnBack || selectedCategoryIndex == 2) && !BoardConfig::hasTouch()) {
    controlsSettings.insert(controlsSettings.begin(),
                            SettingInfo::Action(StrId::STR_REMAP_FRONT_BUTTONS, SettingAction::RemapFrontButtons));
  }
  if (!finishOnBack || selectedCategoryIndex == 3) {
    systemSettings.push_back(SettingInfo::Action(StrId::STR_WIFI_NETWORKS, SettingAction::Network));
    systemSettings.push_back(SettingInfo::Action(StrId::STR_KOREADER_SYNC, SettingAction::KOReaderSync));
    systemSettings.push_back(SettingInfo::Action(StrId::STR_OPDS_SERVERS, SettingAction::OPDSBrowser));
    systemSettings.push_back(SettingInfo::Action(StrId::STR_CLEAR_READING_CACHE, SettingAction::ClearCache));
    // TODO: Touch devices need their own firmware update path/artifacts before OTA is exposed.
    if (!BoardConfig::hasTouch()) {
      systemSettings.push_back(SettingInfo::Action(StrId::STR_CHECK_UPDATES, SettingAction::CheckForUpdates));
    }
    systemSettings.push_back(SettingInfo::Action(StrId::STR_SD_FIRMWARE_UPDATE, SettingAction::SdFirmwareUpdate));
    systemSettings.push_back(SettingInfo::Action(StrId::STR_LANGUAGE, SettingAction::Language));
  }
  if (!finishOnBack || selectedCategoryIndex == 1) {
    // Text Settings (font/margin/line-layout) has nothing to apply to manga, whose pages are
    // pre-rendered images -- hidden along with STR_IMAGES above, leaving Rotate Panels/Reading
    // Orientation/Customise Status Bar as the manga Reader Settings screen.
    if (!mangaMode) {
      readerSettings.insert(readerSettings.begin(),
                            SettingInfo::Action(StrId::STR_TEXT_SETTINGS, SettingAction::TextSettings));
    }
    // Vertical Text / Furigana: per-book overrides that live on the reader activity that pushed
    // this screen, not in CrossPointSettings -- so they only make sense (and only get injected)
    // when there IS such a book (finishOnBack) and the book is one they apply to
    // (showReaderToggles, the same condition the reader's quick menu used before these moved
    // here). Opening Settings from Home never shows them: there is no book to apply them to.
    // Also never for manga (mangaMode implies !showReaderToggles in practice, but this makes the
    // exclusivity explicit rather than relying on the caller never combining the two) -- manga
    // has no Japanese-vertical-text concept, and the +1/+2 insert positions below assume Text
    // Settings just landed at index 0, which mangaMode skips.
    if (!mangaMode && finishOnBack && showReaderToggles) {
      readerSettings.insert(readerSettings.begin() + 1,
                            SettingInfo::DynamicToggle(
                                StrId::STR_VERTICAL_TEXT_LABEL, [this] { return verticalTextState; },
                                [this](const bool v) { verticalTextState = v; }, StrId::STR_CAT_READER));
      readerSettings.insert(readerSettings.begin() + 2,
                            SettingInfo::DynamicToggle(
                                StrId::STR_FURIGANA_LABEL, [this] { return furiganaState; },
                                [this](const bool v) { furiganaState = v; }, StrId::STR_CAT_READER));
    }
    // No STR_MANAGE_FONTS entry here: it lives at the bottom of the font list inside Text
    // Settings, where the pre-1.5.0 picker had it. Upstream moved it up when it replaced
    // FontSelectionActivity; that costs the "this font is missing -> install it" shortcut.
    readerSettings.push_back(SettingInfo::Action(StrId::STR_CUSTOMISE_STATUS_BAR, SettingAction::CustomiseStatusBar));
  }

  // Update currentSettings pointer and count for the active category
  switch (selectedCategoryIndex) {
    case 0:
      currentSettings = &displaySettings;
      break;
    case 1:
      currentSettings = &readerSettings;
      break;
    case 2:
      currentSettings = &controlsSettings;
      break;
    case 3:
      currentSettings = &systemSettings;
      break;
  }
  settingsCount = static_cast<int>(currentSettings->size());
}

void SettingsActivity::onEnter() {
  Activity::onEnter();

  // Start on the requested category (0 unless a caller like the reader menu asks otherwise)
  selectedCategoryIndex = std::clamp(initialCategory, 0, categoryCount - 1);
  selectedSettingIndex = 0;
  if (finishOnBack) selectedSettingIndex = 1;  // category row is locked in embedded mode
  preserveQuickResumeTimeoutOn =
      SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
  quickResumeTimeoutAutoEnabled = false;
  syncQuickResumeTimeoutForSleepScreen(/*sleepScreenChanged=*/true, /*quickResumeTimeoutChanged=*/false);

  rebuildSettingsLists();

  // Trigger first update
  requestUpdate();
}

void SettingsActivity::onExit() {
  Activity::onExit();

  UITheme::getInstance().reload();  // Re-apply theme in case it was changed
}

void SettingsActivity::loop() {
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  bool hasChangedCategory = false;

  auto applyCategorySelection = [this] {
    switch (selectedCategoryIndex) {
      case 0:
        currentSettings = &displaySettings;
        break;
      case 1:
        currentSettings = &readerSettings;
        break;
      case 2:
        currentSettings = &controlsSettings;
        break;
      case 3:
        currentSettings = &systemSettings;
        break;
    }
    settingsCount = static_cast<int>(currentSettings->size());
  };

  // Handle actions with early return
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (selectedSettingIndex == 0) {
      // Embedded single-category mode (reader menu): the category row is locked.
      if (!finishOnBack) {
        selectedCategoryIndex = (selectedCategoryIndex < categoryCount - 1) ? (selectedCategoryIndex + 1) : 0;
        hasChangedCategory = true;
        requestUpdate();
      }
    } else {
      toggleCurrentSetting();
      requestUpdate();
      return;
    }
  }

  // Embedded mode finishes on the RELEASE: finishing on the press would leave the release
  // for the activity underneath (the reader), which would treat it as its own Back and exit
  // the book -- the same stray-event trap the reader menu's ignoreNextConfirmRelease guards.
  if (finishOnBack) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      saveSettings();
      // The reader's READER_SETTINGS handler only reads verticalOverride/furiganaOverride --
      // the same two fields the old in-menu toggles rode back on -- via std::get_if<MenuResult>,
      // so when this screen never showed the toggles (showReaderToggles false) it's fine to set
      // no result at all: the ActivityResult stays std::monostate, get_if returns null, and the
      // handler skips applying anything. When it IS set, action/orientation/pageTurnOption are
      // explicitly -1/0/0 (unused by that handler) rather than MenuResult's own defaults
      // (action=-1, but orientation/pageTurnOption default to 0 already -- see ActivityResult.h)
      // -- spelled out here so a value doesn't get silently relied on either way.
      if (showReaderToggles) {
        setResult(MenuResult{-1, 0, 0, static_cast<int8_t>(verticalTextState ? 1 : 0),
                             static_cast<int8_t>(furiganaState ? 1 : 0)});
      }
      finish();
      return;
    }
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (selectedSettingIndex > 0) {
      selectedSettingIndex = 0;
      requestUpdate();
    } else {
      saveSettings();
      onGoHome();
    }
    return;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  int tx = 0;
  int ty = 0;
  const int tabTop = metrics.topPadding + metrics.headerHeight;
  const int listTop = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing;
  const int listHeight =
      renderer.getScreenHeight() - (metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight +
                                    metrics.buttonHintsHeight + metrics.verticalSpacing * 2);
  auto buildTabs = [&]() {
    std::vector<TabInfo> tabs;
    tabs.reserve(categoryCount);
    for (int i = 0; i < categoryCount; i++) {
      tabs.push_back({I18N.get(categoryNames[i]), selectedCategoryIndex == i});
    }
    return tabs;
  };
  auto settingIndexFromPoint = [&](const int x, const int y, int& settingIndex) {
    (void)x;
    if (settingsCount <= 0 || y < listTop || y >= listTop + listHeight) return false;
    const int rowStep = GUI.getListRowStep(false);
    if (rowStep <= 0) return false;
    const int pageItems = GUI.getListPageItems(listHeight, false);
    const int selectedRow = std::max(0, selectedSettingIndex - 1);
    const int pageStart = selectedRow / pageItems * pageItems;
    const int row = (y - listTop) / rowStep;
    const int touched = pageStart + row;
    if (row < 0 || row >= pageItems || touched < 0 || touched >= settingsCount) return false;
    settingIndex = touched + 1;
    return true;
  };

  if (mappedInput.wasScreenTouchDown(tx, ty)) {
    int touchedCategory = -1;
    const auto tabs = buildTabs();
    if (GUI.tabIndexFromPoint(renderer, Rect{0, tabTop, renderer.getScreenWidth(), metrics.tabBarHeight}, tabs, tx, ty,
                              touchedCategory)) {
      if (selectedCategoryIndex != touchedCategory || selectedSettingIndex != 0) {
        selectedCategoryIndex = touchedCategory;
        selectedSettingIndex = 0;
        applyCategorySelection();
        requestUpdate();
      }
      return;
    }

    int touchedSetting = -1;
    if (settingIndexFromPoint(tx, ty, touchedSetting)) {
      if (selectedSettingIndex != touchedSetting) {
        selectedSettingIndex = touchedSetting;
        requestUpdate();
      }
      return;
    }
  }

  if (mappedInput.wasScreenTapped(tx, ty)) {
    int tappedCategory = -1;
    const auto tabs = buildTabs();
    if (GUI.tabIndexFromPoint(renderer, Rect{0, tabTop, renderer.getScreenWidth(), metrics.tabBarHeight}, tabs, tx, ty,
                              tappedCategory)) {
      selectedCategoryIndex = tappedCategory;
      selectedSettingIndex = 0;
      applyCategorySelection();
      requestUpdate();
      return;
    }

    int tappedSetting = -1;
    if (settingIndexFromPoint(tx, ty, tappedSetting)) {
      selectedSettingIndex = tappedSetting;
      toggleCurrentSetting();
      requestUpdate();
      return;
    }
  }

  // Handle navigation
  const auto& navMetrics = UITheme::getInstance().getMetrics();
  const int settingsListHeight =
      renderer.getScreenHeight() - (navMetrics.topPadding + navMetrics.headerHeight + navMetrics.tabBarHeight +
                                    navMetrics.buttonHintsHeight + navMetrics.verticalSpacing * 2);
  const int settingsPageItems = GUI.getListPageItems(settingsListHeight, false);
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) {
    selectedSettingIndex = selectedSettingIndex == 0 ? 1
                                                     : ButtonNavigator::nextPageIndex(
                                                           selectedSettingIndex, settingsCount + 1, settingsPageItems);
    requestUpdate();
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    selectedSettingIndex =
        ButtonNavigator::previousPageIndex(selectedSettingIndex, settingsCount + 1, settingsPageItems);
    requestUpdate();
    return;
  }

  buttonNavigator.onNextRelease([this] {
    selectedSettingIndex = ButtonNavigator::nextIndex(selectedSettingIndex, settingsCount + 1);
    if (finishOnBack && selectedSettingIndex == 0) selectedSettingIndex = settingsCount > 0 ? 1 : 0;
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this] {
    selectedSettingIndex = ButtonNavigator::previousIndex(selectedSettingIndex, settingsCount + 1);
    if (finishOnBack && selectedSettingIndex == 0) selectedSettingIndex = settingsCount;
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, &hasChangedCategory] {
    hasChangedCategory = true;
    selectedCategoryIndex = ButtonNavigator::nextIndex(selectedCategoryIndex, categoryCount);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, &hasChangedCategory] {
    hasChangedCategory = true;
    selectedCategoryIndex = ButtonNavigator::previousIndex(selectedCategoryIndex, categoryCount);
    requestUpdate();
  });

  if (hasChangedCategory) {
    selectedSettingIndex = (selectedSettingIndex == 0) ? 0 : 1;
    applyCategorySelection();
  }
}

void SettingsActivity::toggleCurrentSetting() {
  int selectedSetting = selectedSettingIndex - 1;
  if (selectedSetting < 0 || selectedSetting >= settingsCount) {
    return;
  }

  const auto& setting = (*currentSettings)[selectedSetting];
  // Applied dictionary rows in reader settings are informational: they have a getter but no setter.
  if (setting.valueGetter && !setting.valueSetter) return;
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
  } else if (setting.type == SettingType::TOGGLE && setting.valueGetter && setting.valueSetter) {
    // Backed by state outside CrossPointSettings (SettingInfo::DynamicToggle) -- e.g. the
    // per-book Vertical Text / Furigana overrides. Returns immediately instead of falling
    // through to the shared tail below: that tail's saveSettings()/rebuildSettingsLists() is
    // for CrossPointSettings changes, and this state isn't part of that singleton -- the
    // caller reads it back from this screen's finish() result instead. Falling through would
    // write the settings file on every toggle for no reason.
    setting.valueSetter(setting.valueGetter() == 0 ? 1 : 0);
    return;
  } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
    const uint8_t currentValue = SETTINGS.*(setting.valuePtr);
    if (setting.enumValues.size() > 2) {
      const auto valuePtr = setting.valuePtr;
      optionPopup.show(setting.nameId, setting.enumValues.data(), static_cast<int>(setting.enumValues.size()),
                       currentValue, [this, valuePtr, sleepScreenChanged, quickResumeTimeoutChanged](int idx) {
                         SETTINGS.*valuePtr = idx;
                         syncQuickResumeTimeoutForSleepScreen(sleepScreenChanged, quickResumeTimeoutChanged);
                         saveSettings();
                         rebuildSettingsLists();
                       });
      requestUpdate();
      return;
    }
    SETTINGS.*(setting.valuePtr) = (currentValue + 1) % static_cast<uint8_t>(setting.enumValues.size());
  } else if (setting.type == SettingType::ENUM && setting.valueGetter && setting.valueSetter) {
    const uint8_t totalValues = setting.enumStringValues.empty()
                                    ? static_cast<uint8_t>(setting.enumValues.size())
                                    : static_cast<uint8_t>(setting.enumStringValues.size());
    const uint8_t cur = setting.valueGetter();
    if (totalValues > 2) {
      const auto valueSetter = setting.valueSetter;
      auto onSelect = [this, valueSetter, sleepScreenChanged, quickResumeTimeoutChanged](int idx) {
        valueSetter(idx);
        syncQuickResumeTimeoutForSleepScreen(sleepScreenChanged, quickResumeTimeoutChanged);
        saveSettings();
        rebuildSettingsLists();
      };
      if (!setting.enumStringValues.empty()) {
        optionPopup.show(setting.nameId, setting.enumStringValues, cur, std::move(onSelect));
      } else {
        optionPopup.show(setting.nameId, setting.enumValues.data(), static_cast<int>(setting.enumValues.size()), cur,
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
    auto resultHandler = [this](const ActivityResult&) { saveSettings(); };

    switch (setting.action) {
      case SettingAction::RemapFrontButtons:
        startActivityForResult(std::make_unique<ButtonRemapActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::CustomiseStatusBar:
        startActivityForResult(std::make_unique<StatusBarSettingsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::KOReaderSync:
        startActivityForResult(std::make_unique<KOReaderSettingsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::OPDSBrowser:
        startActivityForResult(std::make_unique<OpdsServerListActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::Network:
        startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput, false), resultHandler);
        break;
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
                                 saveSettings();
                                 rebuildSettingsLists();
                               });
        break;
      case SettingAction::TextSettings:
        startActivityForResult(std::make_unique<TextSettingsActivity>(renderer, mappedInput, &sdFontSystem.registry(),
                                                                      TextSettingsActivity::Tab::Family, japaneseBook),
                               [this](const ActivityResult&) {
                                 saveSettings();
                                 rebuildSettingsLists();
                               });
        break;
      case SettingAction::Language:
        startActivityForResult(std::make_unique<LanguageSelectActivity>(renderer, mappedInput), resultHandler);
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
  saveSettings();
  rebuildSettingsLists();
  selectedSettingIndex = std::min(selectedSettingIndex, settingsCount);
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
          StrId::STR_SLEEP_TIMER_VALUE_FORMAT, false, true, StrId::STR_SLEEP_NEVER),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          SETTINGS.sleepTimeoutMinutes = static_cast<uint8_t>(std::get<IntervalResult>(result.data).value);
          saveSettings();
        }
        requestUpdate();
      });
}

void SettingsActivity::render(RenderLock&&) {
  if (optionPopup.processRender(renderer, mappedInput)) return;

  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  const auto& metrics = UITheme::getInstance().getMetrics();

  // Embedded single-category mode (opened from the reader menu): title the view after the
  // category and hide the tab bar entirely -- the other categories are not reachable here.
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 finishOnBack ? tr(STR_READER_SETTINGS) : tr(STR_SETTINGS_TITLE), CROSSPOINT_VERSION);

  if (!finishOnBack) {
    std::vector<TabInfo> tabs;
    tabs.reserve(categoryCount);
    for (int i = 0; i < categoryCount; i++) {
      tabs.push_back({I18N.get(categoryNames[i]), selectedCategoryIndex == i});
    }
    GUI.drawTabBar(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight}, tabs,
                   selectedSettingIndex == 0);
  }

  const auto& settings = *currentSettings;
  const bool selectedSettingReadOnly = selectedSettingIndex > 0 && settings[selectedSettingIndex - 1].valueGetter &&
                                       !settings[selectedSettingIndex - 1].valueSetter;
  GUI.drawList(
      renderer,
      Rect{0, metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing, pageWidth,
           pageHeight - (metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.buttonHintsHeight +
                         metrics.verticalSpacing * 2)},
      settingsCount, selectedSettingIndex - 1,
      [&settings](int index) { return std::string(I18N.get(settings[index].nameId)); }, nullptr, nullptr,
      [&settings](int i) {
        const auto& setting = settings[i];
        std::string valueText = "";
        if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
          const bool value = SETTINGS.*(setting.valuePtr);
          valueText = value ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
        } else if (setting.type == SettingType::TOGGLE && setting.valueGetter) {
          valueText = setting.valueGetter() != 0 ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
        } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
          const uint8_t value = SETTINGS.*(setting.valuePtr);
          valueText = I18N.get(setting.enumValues[value]);
        } else if (setting.type == SettingType::ENUM && setting.valueGetter) {
          const uint8_t value = setting.valueGetter();
          if (!setting.enumStringValues.empty() && value < setting.enumStringValues.size()) {
            valueText = setting.enumStringValues[value];
          } else if (value < setting.enumValues.size()) {
            valueText = I18N.get(setting.enumValues[value]);
          }
        } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
          if (setting.nameId == StrId::STR_TIME_TO_SLEEP) {
            char valueBuffer[32];
            if (SETTINGS.sleepTimeoutMinutes >= CrossPointSettings::SLEEP_TIMEOUT_NEVER_MINUTES) {
              valueText = tr(STR_SLEEP_NEVER);
            } else {
              snprintf(valueBuffer, sizeof(valueBuffer), tr(STR_SLEEP_TIMER_VALUE_FORMAT),
                       static_cast<unsigned int>(SETTINGS.*(setting.valuePtr)));
              valueText = valueBuffer;
            }
          } else {
            valueText = std::to_string(SETTINGS.*(setting.valuePtr));
          }
        }
        return valueText;
      },
      !selectedSettingReadOnly);

  // Draw help text
  const auto confirmLabel =
      (selectedSettingIndex == 0)
          ? I18N.get(categoryNames[(selectedCategoryIndex + 1) % categoryCount])
          : (selectedSettingReadOnly
                 ? tr(STR_READ_ONLY)
                 : (selectedSettingIndex > 0 &&
                            (*currentSettings)[selectedSettingIndex - 1].nameId == StrId::STR_TIME_TO_SLEEP
                        ? tr(STR_SELECT)
                        : tr(STR_TOGGLE)));

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Always use standard refresh for settings screen
  renderer.displayBuffer();
}
