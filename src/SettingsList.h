#pragma once

#include <BoardConfig.h>
#include <DictIndex.h>
#include <HalClock.h>
#include <HalFrontlight.h>
#include <HalTiltSensor.h>
#include <I18n.h>
#include <SdCardFontRegistry.h>

#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "CrossPointSettings.h"
#include "HomeButtonSettings.h"
#include "KOReaderCredentialStore.h"
#include "ReaderFontSizes.h"
#include "ReaderLineSpacing.h"
#include "SdCardFontSystem.h"
#include "activities/settings/SettingInfo.h"
#include "components/UITheme.h"
#include "util/DictionaryRegistry.h"
#include "util/SlideSwitch.h"

// Build the font family setting dynamically. When registry is non-null, SD card fonts
// are appended after the built-in fonts. Otherwise only built-in fonts are listed.
inline SettingInfo buildFontFamilySetting(const SdCardFontRegistry* registry) {
  // Built-in font labels (StrId)
  std::vector<StrId> enumValues = {StrId::STR_NOTO_SERIF};
  // Runtime string labels for SD card fonts
  std::vector<std::string> enumStringValues;
  // Family names in the same order: what the setting stores, where a row's label may differ.
  std::vector<std::string> sdFamilyNames;

  // Reserve: first CrossPointSettings::BUILTIN_FONT_COUNT entries use StrId, rest use strings
  if (registry) {
    const auto& families = registry->getFamilies();
    enumStringValues.reserve(families.size());
    sdFamilyNames.reserve(families.size());
    for (const auto& f : families) {
      // Hidden everywhere the picker hides them -- see SdCardFontSystem::isHiddenCjkExtension.
      if (SdCardFontSystem::isHiddenCjkExtension(f.name, registry)) continue;
      // Likewise for a wider-coverage cut of a family already listed: the base row stands for
      // both -- see SdCardFontSystem::isCoverageVariant.
      if (SdCardFontSystem::isCoverageVariant(f.name, registry)) continue;
      enumStringValues.push_back(SdCardFontSystem::pickerLabel(f.name, registry));
      sdFamilyNames.push_back(f.name);
    }
  }

  // Capture the SD font count for the lambdas
  const int sdFontCount = static_cast<int>(enumStringValues.size());

  // Total option count = built-in + SD card families
  // For the combined enumStringValues: we need all entries as strings (built-in names + SD names)
  // The render code checks enumStringValues first, then enumValues. So we build enumStringValues
  // with all options when SD fonts are present.
  std::vector<std::string> allStringValues;
  if (sdFontCount > 0) {
    allStringValues.push_back(I18N.get(StrId::STR_NOTO_SERIF));
    allStringValues.insert(allStringValues.end(), enumStringValues.begin(), enumStringValues.end());
  }

  SettingInfo s;
  s.nameId = StrId::STR_FONT_FAMILY;
  s.type = SettingType::ENUM;
  s.enumValues = std::move(enumValues);
  s.enumStringValues = std::move(allStringValues);
  s.key = "fontFamily";
  s.category = StrId::STR_CAT_READER;
  s.inTextSettings = true;  // matches the static font-family entry it replaces

  s.valueGetter = [sdFamilyNames]() -> uint8_t {
    // If an SD card font is selected, find its index
    if (SETTINGS.sdFontFamilyName[0] != '\0') {
      for (int i = 0; i < static_cast<int>(sdFamilyNames.size()); i++) {
        if (sdFamilyNames[i] == SETTINGS.sdFontFamilyName) {
          return static_cast<uint8_t>(CrossPointSettings::BUILTIN_FONT_COUNT + i);
        }
      }
      // SD font name not found in registry — fall through to built-in
    }
    return SETTINGS.fontFamily < CrossPointSettings::BUILTIN_FONT_COUNT ? SETTINGS.fontFamily : 0;
  };

  s.valueSetter = [sdFamilyNames](uint8_t v) {
    if (v < CrossPointSettings::BUILTIN_FONT_COUNT) {
      SETTINGS.fontFamily = v;
      SETTINGS.sdFontFamilyName[0] = '\0';
    } else {
      int sdIdx = v - CrossPointSettings::BUILTIN_FONT_COUNT;
      if (sdIdx < static_cast<int>(sdFamilyNames.size())) {
        strncpy(SETTINGS.sdFontFamilyName, sdFamilyNames[sdIdx].c_str(), sizeof(SETTINGS.sdFontFamilyName) - 1);
        SETTINGS.sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName) - 1] = '\0';
      }
    }
  };

  return s;
}

// Build the font size setting dynamically: the options are the point sizes this row can
// actually be rendered at — the active family's, plus those of the hidden families that stand
// in for it — so an SD family built at 10/12/14 offers three sizes and a family built at 8..18
// offers six. The selected point size persists in SETTINGS.fontPointSize (saved/loaded manually
// in CrossPointSettings::toJson/fromJson — the generic loop skips dynamic entries), while the
// ENUM contract shared with the web UI stays index-based.
inline SettingInfo buildFontSizeSetting(const SdCardFontRegistry* registry) {
  // `sizes` is captured by copy below: getSettingsList() returns by value and the lambdas
  // outlive this call, so they must not reference the registry.
  const SdCardFontFamilyInfo* standIns[SdCardFontSystem::MAX_STAND_INS];
  const uint8_t standInCount = SdCardFontSystem::readerStandInFamilies(
      registry, SETTINGS.sdFontFamilyName, SETTINGS.fontFamily, standIns, SdCardFontSystem::MAX_STAND_INS);
  const std::vector<uint8_t> sizes = readerFontPointSizes(registry, SETTINGS.sdFontFamilyName, standIns, standInCount);

  // "pt" is deliberately not translated — see the matching note in
  // TextSettingsActivity::rebuildSizeList().
  std::vector<std::string> labels;
  labels.reserve(sizes.size());
  for (const uint8_t pt : sizes) {
    labels.push_back(std::to_string(pt) + " pt");
  }

  SettingInfo s;
  s.nameId = StrId::STR_FONT_SIZE;
  s.type = SettingType::ENUM;
  s.enumStringValues = std::move(labels);
  s.key = "fontSize";
  s.category = StrId::STR_CAT_READER;
  s.inTextSettings = true;  // matches the static font-size entry it replaces

  s.valueGetter = [sizes]() -> uint8_t {
    const uint8_t pt = snapToNearestPointSize(sizes, SETTINGS.fontPointSize);
    for (int i = 0; i < static_cast<int>(sizes.size()); i++) {
      if (sizes[i] == pt) return static_cast<uint8_t>(i);
    }
    return 0;
  };

  s.valueSetter = [sizes](uint8_t v) {
    if (v < sizes.size()) SETTINGS.fontPointSize = sizes[v];
  };

  return s;
}

// Build the dictionary setting dynamically from the folders discovered under /dictionaries.
// Global settings edit the fallback; reader settings show the language-selected dictionary.
inline SettingInfo buildDictionarySetting(const std::vector<DictionaryEntry>& dictionaries,
                                          const std::string& bookLanguage, const bool showAppliedDictionary) {
  std::vector<std::string> folderNames;
  folderNames.reserve(dictionaries.size());
  std::transform(dictionaries.begin(), dictionaries.end(), std::back_inserter(folderNames),
                 [](const DictionaryEntry& d) { return d.name; });

  SettingInfo s;
  s.nameId = showAppliedDictionary ? StrId::STR_DICTIONARY : StrId::STR_FALLBACK_DICTIONARY;
  s.key = "dictionaryName";  // web settings API; persisted by name, not by the generic loop
  s.type = SettingType::ENUM;
  s.enumStringValues.reserve(folderNames.size() + 1);
  s.enumStringValues.push_back(I18N.get(StrId::STR_NONE_OPT));
  s.enumStringValues.insert(s.enumStringValues.end(), folderNames.begin(), folderNames.end());
  s.category = StrId::STR_CAT_READER;

  if (showAppliedDictionary) {
    // Reader settings are informational: the book language wins over the global fallback. Shows
    // the dictionary a lookup reads first; the language's others follow it.
    std::vector<std::string> applied;
    DictionaryRegistry::foldersForLanguage(bookLanguage, SETTINGS.dictionaryName, 1, applied);
    const std::string appliedFolder = applied.empty() ? std::string() : applied.front();
    s.valueGetter = [folderNames, appliedFolder]() -> uint8_t {
      for (size_t i = 0; i < folderNames.size(); i++) {
        // The fallback is persisted in a bounded field, so match it the same way as the editor.
        if (strncmp(folderNames[i].c_str(), appliedFolder.c_str(), sizeof(SETTINGS.dictionaryName) - 1) == 0) {
          return static_cast<uint8_t>(i + 1);
        }
      }
      return 0;  // "None", also when the selected folder no longer exists
    };
  } else {
    s.valueGetter = [folderNames]() -> uint8_t {
      for (size_t i = 0; i < folderNames.size(); i++) {
        // Compare within the settings field capacity: an over-long folder name is
        // stored truncated, and must still match its list entry.
        if (strncmp(folderNames[i].c_str(), SETTINGS.dictionaryName, sizeof(SETTINGS.dictionaryName) - 1) == 0) {
          return static_cast<uint8_t>(i + 1);
        }
      }
      return 0;  // "None", also when the stored folder no longer exists
    };

    s.valueSetter = [folderNames](uint8_t v) {
      if (v == 0 || v > folderNames.size()) {
        SETTINGS.dictionaryName[0] = '\0';
        return;
      }
      strncpy(SETTINGS.dictionaryName, folderNames[v - 1].c_str(), sizeof(SETTINGS.dictionaryName) - 1);
      SETTINGS.dictionaryName[sizeof(SETTINGS.dictionaryName) - 1] = '\0';
    };
  }

  return s;
}

// Whether a Japanese or Chinese dictionary was found on the card. They never appear in the
// fallback picker (they serve their own language's books only), so without this row a working
// install looks missing. Read-only: a getter and no setter.
inline SettingInfo buildDictionaryStatusSetting(const StrId nameId, const char* folder) {
  SettingInfo s;
  s.nameId = nameId;
  s.type = SettingType::ENUM;
  s.enumValues = {StrId::STR_DICT_NOT_INSTALLED, StrId::STR_DICT_INSTALLED};
  const bool installed = DictIndex::isInstalled(folder);
  s.valueGetter = [installed]() -> uint8_t { return installed ? 1 : 0; };
  s.category = StrId::STR_CAT_READER;
  return s;
}

inline SettingInfo buildWordLookupFontSizeSetting() {
  return SettingInfo::Enum(StrId::STR_WORD_LOOKUP_FONT_SIZE, &CrossPointSettings::wordLookupFontSize,
                           {StrId::STR_TINY, StrId::STR_SMALL, StrId::STR_MEDIUM, StrId::STR_LARGE},
                           "wordLookupFontSize", StrId::STR_CAT_READER);
}

// Long-press menu choices as stored LONG_PRESS_MENU_FUNCTION values, in menu order: Off first.
// Reader Menu only on boards with a Home key, which have no other way to it from the long press.
inline std::vector<uint8_t> longPressMenuOrder() {
  std::vector<uint8_t> order = {CrossPointSettings::LP_MENU_DISABLED, CrossPointSettings::LP_MENU_KOSYNC,
                                CrossPointSettings::LP_MENU_BOOKMARK, CrossPointSettings::LP_MENU_DICTIONARY};
  order.reserve(CrossPointSettings::LONG_PRESS_MENU_FUNCTION_COUNT);
  if (BoardConfig::hasHomeKey()) order.push_back(CrossPointSettings::LP_MENU_READER_MENU);
  order.push_back(CrossPointSettings::LP_MENU_CREATE_CLIPPING);
  order.push_back(CrossPointSettings::LP_MENU_FOOTNOTES);
  return order;
}

inline std::vector<StrId> buildLongPressMenuValues() {
  static constexpr StrId LABELS[] = {StrId::STR_KOSYNC,     StrId::STR_STATE_OFF,   StrId::STR_BOOKMARK_OPTION,
                                     StrId::STR_DICTIONARY, StrId::STR_READER_MENU, StrId::STR_SAVE_CLIPPING,
                                     StrId::STR_FOOTNOTES};
  static_assert(std::size(LABELS) == CrossPointSettings::LONG_PRESS_MENU_FUNCTION_COUNT);
  std::vector<StrId> values;
  for (const uint8_t stored : longPressMenuOrder()) values.push_back(LABELS[stored]);
  return values;
}

inline uint8_t longPressMenuDisplayValue() {
  const auto order = longPressMenuOrder();
  const auto it = std::find(order.begin(), order.end(), SETTINGS.longPressMenuFunction);
  return it != order.end() ? static_cast<uint8_t>(it - order.begin()) : 0;
}

inline void setLongPressMenuFromDisplayValue(const uint8_t displayValue) {
  const auto order = longPressMenuOrder();
  SETTINGS.longPressMenuFunction = displayValue < order.size() ? order[displayValue] : order[0];
}

inline SettingInfo buildLongPressMenuSetting() {
  return SettingInfo::DynamicEnum(StrId::STR_LONG_PRESS_MENU, buildLongPressMenuValues(), longPressMenuDisplayValue,
                                  setLongPressMenuFromDisplayValue, "longPressMenuFunction", StrId::STR_CAT_SHORTCUTS);
}

// Side-button and Power + Up actions, indexed by stored SIDE_BUTTON_ACTION value.
inline std::vector<StrId> sideButtonActionLabels() {
  return {StrId::STR_DEFAULT_VALUE,    StrId::STR_SLEEP,         StrId::STR_PREVIOUS_PAGE,
          StrId::STR_NEXT_PAGE_OPT,    StrId::STR_FORCE_REFRESH, StrId::STR_FOOTNOTES,
          StrId::STR_WORD_LOOKUP,      StrId::STR_STATE_OFF,     StrId::STR_SAVE_CLIPPING,
          StrId::STR_SYNC_STATS_CLOCK, StrId::STR_CLOCK_SYNC};
}

// Default first where offered, then Off. Default (the side buttons' page turn) is not a role
// Power + Up can have.
inline std::vector<uint8_t> sideButtonOrder(const bool powerUp) {
  std::vector<uint8_t> order;
  if (!powerUp) order.push_back(CrossPointSettings::SIDE_BTN_DEFAULT);
  order.push_back(CrossPointSettings::SIDE_BTN_NONE);
  order.insert(order.end(), {CrossPointSettings::SIDE_BTN_PREV_PAGE, CrossPointSettings::SIDE_BTN_NEXT_PAGE,
                             CrossPointSettings::SIDE_BTN_SLEEP, CrossPointSettings::SIDE_BTN_REFRESH,
                             CrossPointSettings::SIDE_BTN_FOOTNOTES, CrossPointSettings::SIDE_BTN_WORD_LOOKUP,
                             CrossPointSettings::SIDE_BTN_CREATE_CLIPPING,
                             CrossPointSettings::SIDE_BTN_SYNC_STATS_CLOCK, CrossPointSettings::SIDE_BTN_SYNC_CLOCK});
  return order;
}

// Confirm is offered only on touch boards, where the power button stands in for it (button
// boards have their own). Bookmark, Sync and Reader Menu are offered only on touch boards
// without a Home key, which have no other button for them.
inline std::vector<uint8_t> shortPowerButtonOrder() {
  std::vector<uint8_t> order = {
      CrossPointSettings::IGNORE,        CrossPointSettings::PWR_PREV_PAGE,   CrossPointSettings::PAGE_TURN,
      CrossPointSettings::SLEEP,         CrossPointSettings::FORCE_REFRESH,   CrossPointSettings::FOOTNOTES,
      CrossPointSettings::WORD_LOOKUP,   CrossPointSettings::CREATE_CLIPPING, CrossPointSettings::PWR_SYNC_STATS_CLOCK,
      CrossPointSettings::PWR_SYNC_CLOCK};
  if (BoardConfig::hasTouch()) order.push_back(CrossPointSettings::PWR_CONFIRM);
  if (BoardConfig::hasTouch() && !BoardConfig::hasHomeKey()) {
    order.insert(order.end(),
                 {CrossPointSettings::PWR_BOOKMARK, CrossPointSettings::PWR_SYNC, CrossPointSettings::PWR_READER_MENU});
  }
  return order;
}

inline std::vector<StrId> homeThemeValues() {
  static constexpr StrId VALUES[] = {StrId::STR_THEME_CLASSIC, StrId::STR_THEME_LYRA, StrId::STR_THEME_LYRA_EXTENDED,
                                     StrId::STR_THEME_ROUNDEDRAFF, StrId::STR_THEME_COVER_GRID};
  const size_t count = UITheme::supportsCoverGrid() ? std::size(VALUES) : std::size(VALUES) - 1;
  return {VALUES, VALUES + count};
}

// Shared settings list used by both the device settings UI and the web settings API.
// Each entry has a key (for JSON API) and category (for grouping).
// ACTION-type entries and entries without a key are device-only.
// The list lives in SettingsList.cpp and is built row by row as it is walked, so no
// copy of it stays in memory.

// The two side keys sit one above the other on most boards -- including the plain X4 and the X3 --
// but the X4 Pro and the X4 Classic wear them to the left and the right of the screen, where
// "upper" and "lower" name nothing the reader can see. Only the label changes: the same GPIOs, the
// same settings keys, the same order (the upper row is BTN_UP, which is the left key on those two
// boards). Read once, when the settings list is first built -- by then the board is known, and it
// does not change under a running device.
inline bool sideButtonsReadLeftRight() {
  const auto board = BoardConfig::ACTIVE.board;
  return board == BoardConfig::Board::XteinkX4Pro || board == BoardConfig::Board::XteinkX4Classic;
}
inline StrId upperSideButtonLabel() {
  return sideButtonsReadLeftRight() ? StrId::STR_LEFT_SIDE_BUTTON : StrId::STR_UPPER_SIDE_BUTTON;
}
inline StrId lowerSideButtonLabel() {
  return sideButtonsReadLeftRight() ? StrId::STR_RIGHT_SIDE_BUTTON : StrId::STR_LOWER_SIDE_BUTTON;
}

using SettingVisitor = void (*)(void* context, SettingInfo& info);

// Visits synchronously in list order. Borrowed references are valid only during
// the callback; move from the descriptor to retain ownership. Its board conditions
// must be fixed before settings load, or load and save would see different keys.
void forEachBaseSetting(SettingVisitor visit, void* context);

template <typename Fn>
void forEachBaseSetting(Fn& fn) {
  forEachBaseSetting([](void* context, SettingInfo& info) { (*static_cast<Fn*>(context))(info); }, &fn);
}

// Number of entries forEachBaseSetting() visits.
size_t countBaseSettings();

// True if this board hides the setting at runtime. UI-only: the probes have not
// run at load time, so the base list keeps these keys persisted.
inline bool isHiddenOnDevice(const SettingInfo& s) {
  switch (s.nameId) {
    // The reader menu style stays available on button boards (the toolbar
    // chrome is button-navigable); only the touch controls are hidden.
    case StrId::STR_NEXT_PAGE_GESTURE:
    case StrId::STR_PREV_PAGE_GESTURE:
      return !BoardConfig::hasTouch();
    case StrId::STR_TOUCH_READER_CONTROLS:
      return !BoardConfig::hasTouch() || slideSwitchOwns(CrossPointSettings::SLIDE_SWITCH_TOUCH_CONTROLS);
    // The reader-menu gesture choice only makes sense where the menu stays
    // reachable without the tap and the bottom edge is free (the capacitive
    // Home key); everywhere else the bottom-edge up-swipe is Home and the
    // center tap is the primary path, so the setting stays at its Tap default.
    case StrId::STR_SHOW_READER_MENU:
      return !BoardConfig::hasHomeKey();
    case StrId::STR_FRONT_BTN_FOLLOW_ORIENTATION:
    case StrId::STR_SUNLIGHT_FADING_FIX:
    case StrId::STR_BACK_SHORT_TO_FILE_BROWSER:
      return BoardConfig::hasTouch();
    // Frontlit and lightless variants of a board share one binary (EEGO A4);
    // presence is the I2C probe result from Frontlight.begin(). The slide
    // switch's position owns its bound function (the light at boot too).
    case StrId::STR_RESTORE_LIGHT_ON_WAKE:
      return !Frontlight.present() || slideSwitchOwnsLight();
    case StrId::STR_NIGHT_MODE:
      return slideSwitchOwns(CrossPointSettings::SLIDE_SWITCH_NIGHT_MODE);
    // Tilt page turn needs an IMU with a gyroscope. A failed probe on one boot
    // must not drop the saved value.
    case StrId::STR_TILT_PAGE_TURN:
      return !halTiltSensor.isAvailable();
    // Clock settings need the RTC, as on the device (ClockSettingsActivity).
    case StrId::STR_CLOCK:
    case StrId::STR_CLOCK_FORMAT:
    case StrId::STR_TIMEZONE:
    case StrId::STR_CLOCK_DST:
    case StrId::STR_CLOCK_IN_HEADER:
    case StrId::STR_CLOCK_SYNCED:
      return !halClock.isAvailable();
    // Hidden on touch boards: the Home button actions (X4 Pro) or the short
    // power click (other touch boards) cover these shortcuts.
    case StrId::STR_LONG_PRESS_MENU:
      return BoardConfig::hasTouch();
    default:
      return false;
  }
}

// Replaces the font family and size entries with options built from the fonts
// present; other entries are left unchanged.
inline void buildFontOptions(SettingInfo& s, const SdCardFontRegistry* registry) {
  if (s.nameId == StrId::STR_FONT_FAMILY && registry && registry->getFamilyCount() > 0) {
    s = buildFontFamilySetting(registry);
  } else if (s.nameId == StrId::STR_FONT_SIZE) {
    // Unconditional: even with no SD fonts installed the sizes come from the
    // built-in family rather than a fixed Small/Medium/Large/XL enum.
    s = buildFontSizeSetting(registry);
  }
}

// Which rows a settings screen shows. category STR_NONE_OPT means every category;
// includeTextSettingsEntries false leaves out the rows the Text Settings screen owns.
struct SettingsListOptions {
  StrId category = StrId::STR_NONE_OPT;
  bool includeTextSettingsEntries = true;
  std::string bookLanguage;
  bool showAppliedDictionary = false;
};

// Visits the settings shown on this device in list order, one at a time, for
// callers that need not hold the whole list in memory. Entries whose key
// wants(key) rejects are skipped; wants gets nullptr for entries without a key.
// The visited entry is valid only during the callback; move from it to keep it.
// The dictionary, its install status and the word lookup font size are emitted
// together just before the first Controls row (or last, with none); the Home
// button rows follow the base list.
// A row added here whose value is saved belongs in forEachPersistableSetting() too.
template <typename Wants, typename Visit>
void forEachSetting(const SdCardFontRegistry* registry, const std::vector<DictionaryEntry>* dictionaries, Wants wants,
                    Visit visit, const SettingsListOptions& options = {}) {
  const bool hasDictionaries = dictionaries && !dictionaries->empty();
  const bool readerRows = options.category == StrId::STR_NONE_OPT || options.category == StrId::STR_CAT_READER;
  bool insertPending = hasDictionaries || readerRows;
  const auto offer = [&](SettingInfo& s) {
    if (!wants(s.key)) return;
    buildFontOptions(s, registry);
    visit(s);
  };
  const auto insertDictionaryRows = [&] {
    insertPending = false;
    if (hasDictionaries) {
      SettingInfo dictionary =
          buildDictionarySetting(*dictionaries, options.bookLanguage, options.showAppliedDictionary);
      const bool global = dictionary.nameId == StrId::STR_FALLBACK_DICTIONARY;
      offer(dictionary);
      // Global settings only: in a book the dictionary row already names what that book uses.
      if (global && readerRows) {
        static constexpr std::pair<StrId, const char*> STATUSES[] = {
            {StrId::STR_DICT_STATUS_JA, "jp"}, {StrId::STR_DICT_STATUS_ZH, "zh"}, {StrId::STR_DICT_STATUS_YUE, "yue"}};
        for (const auto& [nameId, folder] : STATUSES) {
          SettingInfo status = buildDictionaryStatusSetting(nameId, folder);
          offer(status);
        }
      }
    }
    if (readerRows) {
      SettingInfo fontSize = buildWordLookupFontSizeSetting();
      offer(fontSize);
    }
  };
  const auto each = [&](SettingInfo& s) {
    if (insertPending && s.category == StrId::STR_CAT_CONTROLS) insertDictionaryRows();
    offer(s);
  };
  auto base = [&](SettingInfo& s) {
    if (options.category != StrId::STR_NONE_OPT && s.category != options.category) return;
    if (!options.includeTextSettingsEntries && s.inTextSettings) return;
    if (isHiddenOnDevice(s)) return;
    each(s);
  };
  forEachBaseSetting(base);
  if (BoardConfig::hasHomeKey()) {
    for (unsigned i = 0; i < 3; ++i) {
      SettingInfo gesture = home_button::setting(i);
      each(gesture);
    }
  }
  if (insertPending) insertDictionaryRows();
}

// UI callers own the list for runtime filtering and font/dictionary options.
inline std::vector<SettingInfo> getSettingsList(const SdCardFontRegistry* registry = nullptr,
                                                const std::vector<DictionaryEntry>* dictionaries = nullptr,
                                                const StrId categoryFilter = StrId::STR_NONE_OPT,
                                                const bool includeTextSettingsEntries = true,
                                                const std::string& bookLanguage = {},
                                                const bool showAppliedDictionary = false) {
  const SettingsListOptions options{categoryFilter, includeTextSettingsEntries, bookLanguage, showAppliedDictionary};
  const auto all = [](const char*) { return true; };
  // Counted first and reserved exactly, so the one allocation is as small as the screen allows:
  // embedded settings screens run while the reader still owns its page, EPUB and font caches.
  size_t count = 0;
  forEachSetting(registry, dictionaries, all, [&count](SettingInfo&) { ++count; }, options);
  std::vector<SettingInfo> v;
  v.reserve(count);
  forEachSetting(registry, dictionaries, all, [&v](SettingInfo& s) { v.push_back(std::move(s)); }, options);
  return v;
}

// Every row whose value belongs in the settings file, without materializing the menu copy (which
// aborts the firmware on a fragmented heap, and persistence runs while the reader is tearing
// down). That is the base list PLUS the rows getSettingsList() only adds to its copy: the
// home-button gestures and the word-lookup font size. Those two were menu-only, so their keys
// were never written and came back as defaults on the next boot -- the Home long-press in
// particular fell back to the pre-1.5 longPressMenuFunction migration, which is why it kept
// reverting to Dictionary. Every row is persisted whatever the board: a value is not the UI's to
// drop. Filtering this walk by board once cost the Reader Menu Style its key on button boards --
// the row was shown and applied, then reverted to List on every boot.
// A saved row added outside the base list belongs in forEachSetting() too.
template <typename Fn>
inline void forEachPersistableSetting(Fn&& fn) {
  auto each = [&fn](SettingInfo& info) { fn(static_cast<const SettingInfo&>(info)); };
  forEachBaseSetting(each);
  if (BoardConfig::hasHomeKey()) {
    for (unsigned i = 0; i < 3; ++i) {
      fn(home_button::setting(i));
    }
  }
  fn(buildWordLookupFontSizeSetting());
}
