#include "SettingsList.h"

void forEachBaseSetting(const SettingVisitor visit, void* context) {
  // Load and save walk this list, so every condition here must be fixed before
  // settings load. Runtime probes belong in isHiddenOnDevice().

  std::vector<StrId> sleepScreenValues(CrossPointSettings::SLEEP_SCREEN_MODE_COUNT);
  sleepScreenValues[CrossPointSettings::DARK] = StrId::STR_DARK;
  sleepScreenValues[CrossPointSettings::LIGHT] = StrId::STR_LIGHT;
  sleepScreenValues[CrossPointSettings::CUSTOM] = StrId::STR_CUSTOM;
  sleepScreenValues[CrossPointSettings::COVER] = StrId::STR_COVER;
  sleepScreenValues[CrossPointSettings::COVER_CUSTOM] = StrId::STR_COVER_CUSTOM;
  sleepScreenValues[CrossPointSettings::BLANK] = StrId::STR_NONE_OPT;
  sleepScreenValues[CrossPointSettings::QUICK_RESUME] = StrId::STR_QUICK_RESUME;
  sleepScreenValues[CrossPointSettings::TRANSPARENT_CUSTOM] = StrId::STR_TRANSPARENT;
  sleepScreenValues[CrossPointSettings::STATS] = StrId::STR_SLEEP_STATS;
  sleepScreenValues[CrossPointSettings::COVER_TRANSPARENT] = StrId::STR_COVER_TRANSPARENT;

  std::vector<StrId> statusBarClockValues(CrossPointSettings::STATUS_BAR_CLOCK_MODE_COUNT);
  statusBarClockValues[CrossPointSettings::STATUS_BAR_CLOCK_HIDE] = StrId::STR_HIDE;
  statusBarClockValues[CrossPointSettings::STATUS_BAR_CLOCK_RIGHT] = StrId::STR_DIR_RIGHT;
  statusBarClockValues[CrossPointSettings::STATUS_BAR_CLOCK_LEFT] = StrId::STR_DIR_LEFT;

  const auto emit = [visit, context](SettingInfo info) { visit(context, info); };
  // --- Sleep ---
  // STR_CAT_SLEEP is not one of the four sections: these rows are reached through the Sleep row
  // in Display, which opens SettingsActivity on this category alone. Listed in the order
  // they appear there.
  emit(SettingInfo::Enum(StrId::STR_SLEEP_SCREEN, &CrossPointSettings::sleepScreen, std::move(sleepScreenValues),
                         "sleepScreen", StrId::STR_CAT_SLEEP));
  emit(SettingInfo::Enum(StrId::STR_SLEEP_COVER_MODE, &CrossPointSettings::sleepScreenCoverMode,
                         {StrId::STR_FIT, StrId::STR_CROP}, "sleepScreenCoverMode", StrId::STR_CAT_SLEEP));
  emit(SettingInfo::Enum(StrId::STR_SLEEP_COVER_FILTER, &CrossPointSettings::sleepScreenCoverFilter,
                         {StrId::STR_NONE_OPT, StrId::STR_FILTER_CONTRAST, StrId::STR_INVERTED},
                         "sleepScreenCoverFilter", StrId::STR_CAT_SLEEP));
  emit(SettingInfo::Enum(StrId::STR_QUICK_RESUME_TIMEOUT, &CrossPointSettings::quickResumeSleepScreen,
                         {StrId::STR_STATE_OFF, StrId::STR_STATE_ON}, "quickResumeSleepScreen", StrId::STR_CAT_SLEEP));
  emit(SettingInfo::Value(
      StrId::STR_TIME_TO_SLEEP, &CrossPointSettings::sleepTimeoutMinutes,
      {CrossPointSettings::MIN_SLEEP_TIMEOUT_MINUTES, CrossPointSettings::MAX_SLEEP_TIMEOUT_MINUTES, 1},
      "sleepTimeoutMinutes", StrId::STR_CAT_SLEEP));
  emit(SettingInfo::Toggle(StrId::STR_RESTORE_LIGHT_ON_WAKE, &CrossPointSettings::frontlightRestoreOnWake,
                           "frontlightRestoreOnWake", StrId::STR_CAT_SLEEP));

  // --- Display ---
  emit(SettingInfo::Enum(StrId::STR_HIDE_BATTERY, &CrossPointSettings::hideBatteryPercentage,
                         {StrId::STR_NEVER, StrId::STR_IN_READER, StrId::STR_ALWAYS}, "hideBatteryPercentage",
                         StrId::STR_CAT_DISPLAY));
  emit(SettingInfo::Enum(StrId::STR_REFRESH_FREQ, &CrossPointSettings::refreshFrequency,
                         {StrId::STR_PAGES_1, StrId::STR_PAGES_5, StrId::STR_PAGES_10, StrId::STR_PAGES_15,
                          StrId::STR_PAGES_30, StrId::STR_NEVER},
                         "refreshFrequency", StrId::STR_CAT_DISPLAY));
  emit(SettingInfo::Enum(StrId::STR_UI_THEME, &CrossPointSettings::uiTheme, homeThemeValues(), "uiTheme",
                         StrId::STR_CAT_DISPLAY));
  emit(SettingInfo::Toggle(StrId::STR_SUNLIGHT_FADING_FIX, &CrossPointSettings::fadingFix, "fadingFix",
                           StrId::STR_CAT_DISPLAY));
#if FREEINK_CAP_FRONTLIGHT
#endif
  // Night mode = inverted output polarity everywhere (ActivityManager
  // applies it to every activity), so it lives in the Display category.
  emit(SettingInfo::Toggle(StrId::STR_NIGHT_MODE, &CrossPointSettings::screenInverted, "screenInverted",
                           StrId::STR_CAT_DISPLAY));
  // The Insights section of Settings.
  emit(SettingInfo::Toggle(StrId::STR_SHOW_LANGUAGE_STATS, &CrossPointSettings::showLanguageStats, "showLanguageStats",
                           StrId::STR_STATS));
  emit(SettingInfo::Toggle(StrId::STR_HIDE_UNKNOWN_LANGUAGES, &CrossPointSettings::hideUnknownLanguages,
                           "hideUnknownLanguages", StrId::STR_STATS));
  emit(SettingInfo::Enum(StrId::STR_INSIGHTS_DATA, &CrossPointSettings::insightsAllDevices,
                         {StrId::STR_INSIGHTS_THIS_DEVICE, StrId::STR_INSIGHTS_ALL_DEVICES}, "insightsAllDevices",
                         StrId::STR_STATS));

  // --- Reader ---
  // Built-in font-family entry. Replaced per-call with a registry-aware
  // version when SD fonts are installed.
  emit(SettingInfo::Enum(StrId::STR_FONT_FAMILY, &CrossPointSettings::fontFamily, {StrId::STR_NOTO_SERIF}, "fontFamily",
                         StrId::STR_CAT_READER)
           .withTextSettings());
  // Placeholder: the selectable sizes depend on the active font family, so
  // this entry is always replaced by buildFontSizeSetting() below. It only
  // fixes the setting's position in the Reader category.
  emit(SettingInfo::Enum(StrId::STR_FONT_SIZE, nullptr, {}, "fontSize", StrId::STR_CAT_READER).withTextSettings());
  // "lineHeight": fromJson() moves older "lineSpacing" saves up one slot for the added 1.0.
  emit(SettingInfo::StaticEnum(StrId::STR_LINE_SPACING, &CrossPointSettings::lineSpacing, LINE_SPACING_IDS,
                               "lineHeight", StrId::STR_CAT_READER)
           .withTextSettings());
  emit(SettingInfo::Value(StrId::STR_WORD_SPACING, &CrossPointSettings::wordSpacing,
                          {CrossPointSettings::WORD_SPACING_MIN, CrossPointSettings::WORD_SPACING_MAX,
                           CrossPointSettings::WORD_SPACING_STEP},
                          "wordSpacing", StrId::STR_CAT_READER)
           .withTextSettings());
  emit(SettingInfo::Enum(StrId::STR_CHARACTER_SPACING, &CrossPointSettings::characterSpacing,
                         {StrId::STR_SPACING_MINUS_2, StrId::STR_SPACING_MINUS_1, StrId::STR_SPACING_ZERO,
                          StrId::STR_SPACING_PLUS_1, StrId::STR_SPACING_PLUS_2},
                         "characterSpacing", StrId::STR_CAT_READER)
           .withTextSettings());
  emit(SettingInfo::Value(StrId::STR_SCREEN_MARGIN, &CrossPointSettings::screenMargin,
                          {CrossPointSettings::SCREEN_MARGIN_MIN, CrossPointSettings::SCREEN_MARGIN_MAX,
                           CrossPointSettings::SCREEN_MARGIN_STEP},
                          "screenMargin", StrId::STR_CAT_READER)
           .withTextSettings());
  emit(SettingInfo::Toggle(StrId::STR_BOOK_CSS_MARGINS, &CrossPointSettings::bookCssMargins, "bookCssMargins",
                           StrId::STR_CAT_READER)
           .withTextSettings());
  emit(SettingInfo::Enum(StrId::STR_PARA_ALIGNMENT, &CrossPointSettings::paragraphAlignment,
                         {StrId::STR_JUSTIFY, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT,
                          StrId::STR_BOOK_S_STYLE},
                         "paragraphAlignment", StrId::STR_CAT_READER)
           .withTextSettings());
  emit(SettingInfo::Toggle(StrId::STR_EMBEDDED_STYLE, &CrossPointSettings::embeddedStyle, "embeddedStyle",
                           StrId::STR_CAT_READER)
           .withTextSettings());
  emit(SettingInfo::Toggle(StrId::STR_FOCUS_READING, &CrossPointSettings::focusReadingEnabled, "focusReadingEnabled",
                           StrId::STR_CAT_READER)
           .withTextSettings());
  emit(SettingInfo::Toggle(StrId::STR_HYPHENATION, &CrossPointSettings::hyphenationEnabled, "hyphenationEnabled",
                           StrId::STR_CAT_READER)
           .withTextSettings());
  emit(SettingInfo::Enum(
      StrId::STR_ORIENTATION, &CrossPointSettings::orientation,
      {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_ORIENTATION_INVERTED, StrId::STR_LANDSCAPE_CCW},
      "orientation", StrId::STR_CAT_READER));
  emit(SettingInfo::Enum(StrId::STR_TATEGAKI, &CrossPointSettings::tategaki,
                         {StrId::STR_VERTICAL_AUTOMATIC, StrId::STR_STATE_ON, StrId::STR_STATE_OFF}, "tategaki",
                         StrId::STR_CAT_READER)
           .withTextSettings());
  emit(SettingInfo::Enum(StrId::STR_ZHIPAI, &CrossPointSettings::zhipai,
                         {StrId::STR_VERTICAL_AUTOMATIC, StrId::STR_STATE_ON, StrId::STR_STATE_OFF}, "zhipai",
                         StrId::STR_CAT_READER)
           .withTextSettings());
  emit(SettingInfo::Enum(StrId::STR_ZHIPAI, &CrossPointSettings::zhipaiSimplified,
                         {StrId::STR_VERTICAL_AUTOMATIC, StrId::STR_STATE_ON, StrId::STR_STATE_OFF}, "zhipaiSimplified",
                         StrId::STR_CAT_READER)
           .withTextSettings());
  emit(SettingInfo::Toggle(StrId::STR_ROTATE_MANGA_PANELS, &CrossPointSettings::rotateMangaPanels, "rotateMangaPanels",
                           StrId::STR_CAT_READER));
  emit(SettingInfo::Toggle(StrId::STR_EXTRA_SPACING, &CrossPointSettings::extraParagraphSpacing,
                           "extraParagraphSpacing", StrId::STR_CAT_READER)
           .withTextSettings());
  emit(SettingInfo::Value(StrId::STR_PARAGRAPH_INDENTATION, &CrossPointSettings::paragraphIndentSpaces, {0, 5, 1},
                          "paragraphIndentSpaces", StrId::STR_CAT_READER)
           .withTextSettings());
  emit(SettingInfo::Toggle(StrId::STR_TEXT_AA, &CrossPointSettings::textAntiAliasing, "textAntiAliasing",
                           StrId::STR_CAT_READER)
           .withTextSettings());
  emit(SettingInfo::Enum(StrId::STR_IMAGES, &CrossPointSettings::imageRendering,
                         {StrId::STR_IMAGES_DISPLAY, StrId::STR_IMAGES_PLACEHOLDER, StrId::STR_IMAGES_SUPPRESS,
                          StrId::STR_IMAGES_DISPLAY_BW},
                         "imageRendering", StrId::STR_CAT_READER));
  emit(SettingInfo::Enum(StrId::STR_READER_MENU_STYLE, &CrossPointSettings::readerMenuStyle,
                         {StrId::STR_MENU_STYLE_LIST, StrId::STR_MENU_STYLE_TOOLBAR}, "readerMenuStyle",
                         StrId::STR_CAT_READER));
  // --- Controls ---
  // Front buttons first, then the side buttons, then the touch equivalents. The Shortcuts
  // and Remap rows are actions, inserted ahead of these in SettingsActivity. No Side Button
  // Layout row: the per-button upper/lowerSideButtonAction settings replaced it, and cover
  // its Next/Next and Prev/Prev options by setting both buttons to the same action.
#if FREEINK_CAP_HAPTIC
  emit(SettingInfo::Enum(StrId::STR_VIBRATION, &CrossPointSettings::vibration,
                         {StrId::STR_VIBRATION_TOUCH, StrId::STR_VIBRATION_TOUCH_PAGE, StrId::STR_STATE_OFF},
                         "vibration", StrId::STR_CAT_CONTROLS));
  emit(SettingInfo::Enum(StrId::STR_HAPTIC_INTENSITY, &CrossPointSettings::hapticIntensity,
                         {StrId::STR_HAPTIC_LOW, StrId::STR_HAPTIC_MEDIUM, StrId::STR_HAPTIC_HIGH}, "hapticIntensity",
                         StrId::STR_CAT_CONTROLS));
#endif
  emit(SettingInfo::Toggle(StrId::STR_FRONT_BTN_FOLLOW_ORIENTATION, &CrossPointSettings::frontButtonFollowOrientation,
                           "frontButtonFollowOrientation", StrId::STR_CAT_CONTROLS));
  emit(SettingInfo::Toggle(StrId::STR_REVERSED_PAGE_TURN, &CrossPointSettings::reversePageTurn, "reversePageTurn",
                           StrId::STR_CAT_CONTROLS));
  if (CrossPointSettings::homeBackResumesSupported())
    emit(SettingInfo::Toggle(StrId::STR_HOME_BACK_RESUMES, &CrossPointSettings::homeBackResumes, "homeBackResumes",
                             StrId::STR_CAT_CONTROLS));
  emit(SettingInfo::Toggle(StrId::STR_TOUCH_READER_CONTROLS, &CrossPointSettings::touchReaderControls,
                           "touchReaderControls", StrId::STR_CAT_CONTROLS));
#if FREEINK_DEVICE_PICCO
  emit(SettingInfo::Enum(StrId::STR_SLIDE_SWITCH, &CrossPointSettings::slideSwitchAction,
                         {StrId::STR_FRONTLIGHT, StrId::STR_NIGHT_MODE, StrId::STR_TOUCH_READER_CONTROLS},
                         "slideSwitchAction", StrId::STR_CAT_CONTROLS));
#endif
  // Inverted Swipe is Matcha-only, for right-to-left vertical reading. It is stored after
  // Disabled (append-only indices) but offered beside Inverted Tap, which is what the
  // enumOrder below does -- presentation only, stored values unchanged.
  emit(SettingInfo::Enum(StrId::STR_NEXT_PAGE_GESTURE, &CrossPointSettings::pageTurnGesture,
                         {StrId::STR_TAP_AND_SWIPE, StrId::STR_TAP_ONLY, StrId::STR_SWIPE_ONLY, StrId::STR_INVERTED_TAP,
                          StrId::STR_DISABLED, StrId::STR_STATE_INVERTED_SWIPE},
                         "pageTurnGesture", StrId::STR_CAT_CONTROLS)
           .withEnumOrder({CrossPointSettings::TAP_AND_SWIPE, CrossPointSettings::TAP_ONLY,
                           CrossPointSettings::SWIPE_ONLY, CrossPointSettings::INVERTED_TAP,
                           CrossPointSettings::INVERTED_SWIPE, CrossPointSettings::PAGE_TURN_GESTURE_DISABLED}));
  emit(SettingInfo::Enum(StrId::STR_PREV_PAGE_GESTURE, &CrossPointSettings::previousPageGesture,
                         {StrId::STR_TAP_AND_SWIPE, StrId::STR_TAP_ONLY, StrId::STR_SWIPE_ONLY, StrId::STR_INVERTED_TAP,
                          StrId::STR_DISABLED, StrId::STR_STATE_INVERTED_SWIPE},
                         "previousPageGesture", StrId::STR_CAT_CONTROLS)
           .withEnumOrder({CrossPointSettings::TAP_AND_SWIPE, CrossPointSettings::TAP_ONLY,
                           CrossPointSettings::SWIPE_ONLY, CrossPointSettings::INVERTED_TAP,
                           CrossPointSettings::INVERTED_SWIPE, CrossPointSettings::PAGE_TURN_GESTURE_DISABLED}));
  // Persisted under the legacy "tapForReaderMenu" key: old saves map
  // 0 = Off, 1 = Tap.
  emit(SettingInfo::Enum(StrId::STR_SHOW_READER_MENU, &CrossPointSettings::showReaderMenu,
                         {StrId::STR_STATE_OFF, StrId::STR_STATE_TAP, StrId::STR_STATE_SWIPE_UP}, "tapForReaderMenu",
                         StrId::STR_CAT_CONTROLS));

  // --- Shortcuts ---
  // STR_CAT_SHORTCUTS is not one of the four sections: these rows are reached through the
  // Shortcuts row in Controls, which opens SettingsActivity on this category alone.
  emit(SettingInfo::Enum(StrId::STR_LONG_PRESS_BEHAVIOR, &CrossPointSettings::longPressButtonBehavior,
                         {StrId::STR_LONG_PRESS_BEHAVIOR_OFF, StrId::STR_LONG_PRESS_BEHAVIOR_SKIP,
                          StrId::STR_LONG_PRESS_BEHAVIOR_ORIENTATION},
                         "longPressButtonBehavior", StrId::STR_CAT_SHORTCUTS));
  emit(buildLongPressMenuSetting());
  emit(SettingInfo::Enum(StrId::STR_POWER_UP_ACTION, &CrossPointSettings::powerUpAction, sideButtonActionLabels(),
                         "powerUpAction", StrId::STR_CAT_SHORTCUTS)
           .withEnumOrder(sideButtonOrder(/*powerUp=*/true)));
  // Erased below unless the board is an X4 Pro.
  // Double-click power frontlight shortcut only exists on the X4 Pro.
  if (BoardConfig::isX4Pro())
    emit(SettingInfo::Toggle(StrId::STR_DBL_CLICK_PWR_LIGHT, &CrossPointSettings::doubleClickPwrLight,
                             "doubleClickPwrLight", StrId::STR_CAT_SHORTCUTS));
  // Word Lookup keeps index 5 on every board -- it is Matcha's and already persisted.
  // Confirm is appended at 6 (upstream put it at 5); Previous Page at 7; Save Clipping at 8.
  // The indices are identical on touch and button boards so a stored value keeps its
  // meaning across them; Confirm simply has no handler where a front Confirm key exists.
  // Labels stay indexed BY STORED VALUE; withEnumOrder() only decides what the menu offers
  // first. Previous Page sits at 7 because appending was the only safe place for it, and
  // reading it seven rows below Next Page was confusing -- so the two are offered together.
  emit(SettingInfo::Enum(StrId::STR_SHORT_PWR_BTN, &CrossPointSettings::shortPwrBtn,
                         {StrId::STR_STATE_OFF, StrId::STR_SLEEP, StrId::STR_NEXT_PAGE_OPT, StrId::STR_FORCE_REFRESH,
                          StrId::STR_FOOTNOTES, StrId::STR_WORD_LOOKUP, StrId::STR_CONFIRM, StrId::STR_PREVIOUS_PAGE,
                          StrId::STR_SAVE_CLIPPING, StrId::STR_BOOKMARK_OPTION, StrId::STR_KOSYNC,
                          StrId::STR_READER_MENU, StrId::STR_SYNC_STATS_CLOCK, StrId::STR_CLOCK_SYNC},
                         "shortPwrBtn", StrId::STR_CAT_SHORTCUTS)
           .withEnumOrder(shortPowerButtonOrder()));
  // Erased below unless the QMI8658 IMU is present (X3).
  emit(SettingInfo::Enum(StrId::STR_TILT_PAGE_TURN, &CrossPointSettings::tiltPageTurn,
                         {StrId::STR_STATE_OFF, StrId::STR_NORMAL, StrId::STR_INVERTED}, "tiltPageTurn",
                         StrId::STR_CAT_SHORTCUTS));
  emit(SettingInfo::Toggle(StrId::STR_PWR_BTN_FOOTNOTE_BACK, &CrossPointSettings::pwrBtnFootnoteBack,
                           "pwrBtnFootnoteBack", StrId::STR_CAT_SHORTCUTS));
  // Last in the Shortcuts sub-screen: fixed physical mapping (Upper = BTN_UP,
  // Lower = BTN_DOWN). Every board profile defines that pair and sideActionFired() reads
  // those two keys directly, so the rows are offered everywhere. Labels are indexed by
  // stored value; withEnumOrder() decides the menu order.
  emit(SettingInfo::Enum(upperSideButtonLabel(), &CrossPointSettings::upperSideButtonAction, sideButtonActionLabels(),
                         "upperSideButtonAction", StrId::STR_CAT_SHORTCUTS)
           .withEnumOrder(sideButtonOrder(/*powerUp=*/false)));
  emit(SettingInfo::Enum(lowerSideButtonLabel(), &CrossPointSettings::lowerSideButtonAction, sideButtonActionLabels(),
                         "lowerSideButtonAction", StrId::STR_CAT_SHORTCUTS)
           .withEnumOrder(sideButtonOrder(/*powerUp=*/false)));
  // Last row in Shortcuts.
  emit(SettingInfo::Toggle(StrId::STR_BACK_SHORT_TO_FILE_BROWSER, &CrossPointSettings::backShortToFileBrowser,
                           "backShortToFileBrowser", StrId::STR_CAT_SHORTCUTS));

  // Persisted here; set from the Plugins popup.
  emit(SettingInfo::Toggle(StrId::STR_PLUGINS, &CrossPointSettings::pluginsEnabled, "pluginsEnabled").withWebHidden());
  emit(SettingInfo::Toggle(StrId::STR_PLUGIN_HUB, &CrossPointSettings::pluginHubPromptHidden, "pluginHubPromptHidden")
           .withWebHidden());

  // --- System ---
  emit(SettingInfo::Toggle(StrId::STR_SHOW_HIDDEN_FILES, &CrossPointSettings::showHiddenFiles, "showHiddenFiles",
                           StrId::STR_CAT_DISPLAY));
  // STR_CAT_LIBRARY is not one of the four sections: these rows are reached through the
  // Library row in Display, which opens SettingsActivity on this category alone. Listed
  // in the order they appear there, with the Rebuild action injected after the first.
  // Which screen the Library entry opens; see CrossPointSettings::LIBRARY_VIEW.
  emit(SettingInfo::Enum(StrId::STR_LIBRARY_VIEW, &CrossPointSettings::libraryView,
                         {StrId::STR_LIBRARY_VIEW_COVERS, StrId::STR_LIBRARY_VIEW_LIST}, "libraryView",
                         StrId::STR_CAT_LIBRARY));
  emit(SettingInfo::Toggle(StrId::STR_REMOVE_READ_FROM_RECENTS, &CrossPointSettings::removeReadBooksFromRecents,
                           "removeReadBooksFromRecents", StrId::STR_CAT_LIBRARY));
  emit(SettingInfo::Toggle(StrId::STR_MOVE_FINISHED_TO_READ, &CrossPointSettings::moveFinishedToReadFolder,
                           "moveFinishedToReadFolder", StrId::STR_CAT_LIBRARY));
  emit(SettingInfo::Toggle(StrId::STR_LIBRARY_USE_METADATA, &CrossPointSettings::libraryUseMetadata,
                           "libraryUseMetadata", StrId::STR_CAT_LIBRARY));

  // OPDS download folder: persisted + web-exposed, but category-less so it
  // is hidden from the on-device Settings screen (edited via OPDS UI).
  emit(SettingInfo::String(StrId::STR_OPDS_DOWNLOAD_FOLDER, &SETTINGS.opdsDownloadFolder[0],
                           sizeof(SETTINGS.opdsDownloadFolder), "opdsDownloadFolder"));
  // OPDS download filename format: persisted + web-exposed, category-less so it
  // is hidden from the on-device Settings screen (cycled from the OPDS UI).
  emit(SettingInfo::Enum(StrId::STR_OPDS_FILENAME_FORMAT, &CrossPointSettings::opdsFilenameFormat,
                         {StrId::STR_FMT_AUTHOR_TITLE, StrId::STR_FMT_TITLE_AUTHOR, StrId::STR_FMT_TITLE},
                         "opdsFilenameFormat"));

  // Frontlight quick-panel state: persisted and web-exposed, but hidden
  // from the on-device Settings screen because the swipe panel owns it.
  emit(SettingInfo::Value(StrId::STR_BRIGHTNESS, &CrossPointSettings::frontlightBrightness, {0, 100, 5},
                          "frontlightBrightness"));
#if FREEINK_CAP_WARMLIGHT
  emit(SettingInfo::Value(StrId::STR_WARMTH, &CrossPointSettings::frontlightWarmth, {0, 100, 5}, "frontlightWarmth"));
#endif
  emit(SettingInfo::Toggle(StrId::STR_FRONTLIGHT, &CrossPointSettings::frontlightOn, "frontlightOn"));

  // --- KOReader Sync (web-only, uses KOReaderCredentialStore) ---
  emit(SettingInfo::DynamicString(
      StrId::STR_KOREADER_USERNAME, [] { return KOREADER_STORE.getUsername(); },
      [](const std::string& v) {
        KOREADER_STORE.setCredentials(v, KOREADER_STORE.getPassword());
        KOREADER_STORE.saveToFile();
      },
      "koUsername", StrId::STR_KOREADER_SYNC));
  emit(SettingInfo::DynamicString(
           StrId::STR_KOREADER_PASSWORD, [] { return KOREADER_STORE.getPassword(); },
           [](const std::string& v) {
             KOREADER_STORE.setCredentials(KOREADER_STORE.getUsername(), v);
             KOREADER_STORE.saveToFile();
           },
           "koPassword", StrId::STR_KOREADER_SYNC)
           .withObfuscated());
  emit(SettingInfo::DynamicString(
      StrId::STR_SYNC_SERVER_URL, [] { return KOREADER_STORE.getServerUrl(); },
      [](const std::string& v) {
        KOREADER_STORE.setServerUrl(v);
        KOREADER_STORE.saveToFile();
      },
      "koServerUrl", StrId::STR_KOREADER_SYNC));
  emit(SettingInfo::DynamicString(
      StrId::STR_KOREADER_CUSTOM_HEADER_1_NAME, [] { return KOREADER_STORE.getCustomHeaders()[0].name; },
      [](const std::string& v) {
        KOREADER_STORE.setCustomHeader(0, v, KOREADER_STORE.getCustomHeaders()[0].value);
        KOREADER_STORE.saveToFile();
      },
      "koHeader1Name", StrId::STR_KOREADER_SYNC));
  emit(SettingInfo::DynamicString(
           StrId::STR_KOREADER_CUSTOM_HEADER_1_VALUE, [] { return KOREADER_STORE.getCustomHeaders()[0].value; },
           [](const std::string& v) {
             KOREADER_STORE.setCustomHeader(0, KOREADER_STORE.getCustomHeaders()[0].name, v);
             KOREADER_STORE.saveToFile();
           },
           "koHeader1Value", StrId::STR_KOREADER_SYNC)
           .withObfuscated());
  emit(SettingInfo::DynamicString(
      StrId::STR_KOREADER_CUSTOM_HEADER_2_NAME, [] { return KOREADER_STORE.getCustomHeaders()[1].name; },
      [](const std::string& v) {
        KOREADER_STORE.setCustomHeader(1, v, KOREADER_STORE.getCustomHeaders()[1].value);
        KOREADER_STORE.saveToFile();
      },
      "koHeader2Name", StrId::STR_KOREADER_SYNC));
  emit(SettingInfo::DynamicString(
           StrId::STR_KOREADER_CUSTOM_HEADER_2_VALUE, [] { return KOREADER_STORE.getCustomHeaders()[1].value; },
           [](const std::string& v) {
             KOREADER_STORE.setCustomHeader(1, KOREADER_STORE.getCustomHeaders()[1].name, v);
             KOREADER_STORE.saveToFile();
           },
           "koHeader2Value", StrId::STR_KOREADER_SYNC)
           .withObfuscated());
  emit(SettingInfo::DynamicEnum(
      StrId::STR_SERVER_TYPE, {StrId::STR_SERVER_TYPE_CROSSPOINT, StrId::STR_KOSYNC_SERVER, StrId::STR_OTHER},
      [] { return static_cast<uint8_t>(KOREADER_STORE.getServerType()); },
      [](uint8_t v) {
        KOREADER_STORE.setServerType(static_cast<KOReaderServerType>(v));
        KOREADER_STORE.saveToFile();
      },
      "koServerType", StrId::STR_KOREADER_SYNC));
  emit(SettingInfo::DynamicEnum(
      StrId::STR_DOCUMENT_MATCHING, {StrId::STR_FILENAME, StrId::STR_BINARY},
      [] { return static_cast<uint8_t>(KOREADER_STORE.getMatchMethod()); },
      [](uint8_t v) {
        KOREADER_STORE.setMatchMethod(static_cast<DocumentMatchMethod>(v));
        KOREADER_STORE.saveToFile();
      },
      "koMatchMethod", StrId::STR_KOREADER_SYNC));
  emit(SettingInfo::DynamicEnum(
      StrId::STR_SEND_METADATA, {StrId::STR_STATE_OFF, StrId::STR_STATE_ON},
      [] { return static_cast<uint8_t>(KOREADER_STORE.getSendMetadata()); },
      [](uint8_t v) {
        KOREADER_STORE.setSendMetadata(v != 0);
        KOREADER_STORE.saveToFile();
      },
      "koSendMetadata", StrId::STR_KOREADER_SYNC));
  emit(SettingInfo::DynamicEnum(
      StrId::STR_SYNC_BEHAVIOR, {StrId::STR_ASK_EVERY_TIME, StrId::STR_SMART_SYNC},
      [] { return static_cast<uint8_t>(KOREADER_STORE.getSyncBehavior()); },
      [](uint8_t v) {
        KOREADER_STORE.setSyncBehavior(static_cast<KOReaderSyncBehavior>(v));
        KOREADER_STORE.saveToFile();
      },
      "koSyncBehavior", StrId::STR_KOREADER_SYNC));
  emit(SettingInfo::DynamicEnum(
      StrId::STR_SYNC_CLIPPINGS, {StrId::STR_STATE_OFF, StrId::STR_STATE_ON},
      [] { return static_cast<uint8_t>(KOREADER_STORE.getSyncClippings()); },
      [](uint8_t v) {
        if (KOREADER_STORE.getSyncClippings() == (v != 0)) return;
        KOREADER_STORE.setSyncClippings(v != 0);
        KOREADER_STORE.saveToFile();
      },
      "koSyncClippings", StrId::STR_KOREADER_SYNC));
  // --- Status Bar Settings (web-only, uses StatusBarSettingsActivity) ---
  emit(SettingInfo::Toggle(StrId::STR_CHAPTER_PAGE_COUNT, &CrossPointSettings::statusBarChapterPageCount,
                           "statusBarChapterPageCount", StrId::STR_CUSTOMISE_STATUS_BAR));
  emit(SettingInfo::Toggle(StrId::STR_BOOK_PROGRESS_PERCENTAGE, &CrossPointSettings::statusBarBookProgressPercentage,
                           "statusBarBookProgressPercentage", StrId::STR_CUSTOMISE_STATUS_BAR));
  emit(SettingInfo::Enum(StrId::STR_TIME_LEFT, &CrossPointSettings::statusBarTimeLeft,
                         {StrId::STR_CHAPTER, StrId::STR_BOOK, StrId::STR_HIDE}, "statusBarTimeLeft",
                         StrId::STR_CUSTOMISE_STATUS_BAR));
  emit(SettingInfo::Enum(StrId::STR_PROGRESS_BAR, &CrossPointSettings::statusBarProgressBar,
                         {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE}, "statusBarProgressBar",
                         StrId::STR_CUSTOMISE_STATUS_BAR));
  emit(SettingInfo::Enum(StrId::STR_PROGRESS_BAR_THICKNESS, &CrossPointSettings::statusBarProgressBarThickness,
                         {StrId::STR_PROGRESS_BAR_THIN, StrId::STR_PROGRESS_BAR_MEDIUM, StrId::STR_PROGRESS_BAR_THICK},
                         "statusBarProgressBarThickness", StrId::STR_CUSTOMISE_STATUS_BAR));
  emit(SettingInfo::Enum(StrId::STR_TITLE, &CrossPointSettings::statusBarTitle,
                         {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE}, "statusBarTitle",
                         StrId::STR_CUSTOMISE_STATUS_BAR));
  emit(SettingInfo::Toggle(StrId::STR_BATTERY, &CrossPointSettings::statusBarBattery, "statusBarBattery",
                           StrId::STR_CUSTOMISE_STATUS_BAR));
  emit(SettingInfo::Enum(StrId::STR_XTC_STATUS_BAR, &CrossPointSettings::xtcStatusBarMode,
                         {StrId::STR_HIDE, StrId::STR_BOTTOM, StrId::STR_TOP}, "xtcStatusBarMode",
                         StrId::STR_CUSTOMISE_STATUS_BAR));
  // Clock entries (persistence + web settings; the device UI is
  // ClockSettingsActivity under System settings).
  emit(SettingInfo::Enum(StrId::STR_CLOCK, &CrossPointSettings::statusBarClock, std::move(statusBarClockValues),
                         "statusBarClock", StrId::STR_CUSTOMISE_STATUS_BAR));
  // LEGACY: retired quarter-hour UTC offset (biased by 48). Persisted so
  // timezones::activeIndex() can migrate it into clockTimezone.
  emit(SettingInfo::Value(StrId::STR_CLOCK, &CrossPointSettings::clockUtcOffsetQ, {0, 104, 1}, "clockUtcOffsetQ",
                          StrId::STR_CUSTOMISE_STATUS_BAR)
           .withWebHidden());
  emit(SettingInfo::Enum(StrId::STR_CLOCK_FORMAT, &CrossPointSettings::clockFormat,
                         {StrId::STR_CLOCK_FORMAT_24H, StrId::STR_CLOCK_FORMAT_12H}, "clockFormat",
                         StrId::STR_CUSTOMISE_STATUS_BAR));
  // Index into the append-only table in src/util/Timezones.cpp; 255 = unset.
  // Web-hidden: a raw index is unusable there; the device has a picker.
  emit(SettingInfo::Value(StrId::STR_TIMEZONE, &CrossPointSettings::clockTimezone, {0, 255, 1}, "clockTimezone",
                          StrId::STR_CUSTOMISE_STATUS_BAR)
           .withWebHidden());
  emit(SettingInfo::Enum(StrId::STR_CLOCK_DST, &CrossPointSettings::clockDst,
                         {StrId::STR_CLOCK_DST_AUTO, StrId::STR_STATE_ON, StrId::STR_STATE_OFF}, "clockDst",
                         StrId::STR_CUSTOMISE_STATUS_BAR));
  emit(SettingInfo::Toggle(StrId::STR_CLOCK_IN_HEADER, &CrossPointSettings::clockShowInHeader, "clockShowHeader",
                           StrId::STR_CUSTOMISE_STATUS_BAR));
  // Persistence flag for NTP debounce. Resetting from the web UI forces a re-sync
  // on next WiFi connect, which is useful when crossing time zones.
  emit(SettingInfo::Toggle(StrId::STR_CLOCK_SYNCED, &CrossPointSettings::clockHasBeenSynced, "clockHasBeenSynced",
                           StrId::STR_CUSTOMISE_STATUS_BAR));
}

size_t countBaseSettings() {
  size_t count = 0;
  forEachBaseSetting([](void* context, SettingInfo&) { ++*static_cast<size_t*>(context); }, &count);
  return count;
}
