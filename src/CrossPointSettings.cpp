#include "CrossPointSettings.h"

#include <BlePageTurnerJson.h>
#include <I18n.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <limits>
#include <string>

#include "I18nKeys.h"
#include "ReaderFontSizes.h"
#include "ReaderLineSpacing.h"
#include "SettingsList.h"
#include "fontIds.h"
#include "util/ParagraphIndentMigration.h"
#include "util/PluginLocations.h"

namespace {

// Stack buffer for "<key>_obf" key construction — avoids a std::string
// allocation per obfuscated setting on every save and load.
constexpr size_t OBF_KEY_BUF = 64;

// Null-terminated copy into a fixed-size settings field.
void copyToField(char* dest, const char* src, const size_t maxLen) {
  strncpy(dest, src, maxLen - 1);
  dest[maxLen - 1] = '\0';
}

// settings.json keys of textProfiles, indexed by TextProfileId. Latin is stored in the top-level
// fields, so its entry is never written.
constexpr const char* TEXT_PROFILE_KEYS[] = {"latin", "ja", "zh-Hant", "zh-Hans", "ko"};
static_assert(std::size(TEXT_PROFILE_KEYS) == static_cast<size_t>(CrossPointSettings::TextProfileId::Count));

// Pre-percentage saves: Tight/Normal/Wide/Extra Wide (0..3) become 1.1..1.45.
uint8_t legacyLineSpacing(JsonVariantConst value) {
  const int legacy = value.as<uint8_t>();
  return legacy + 1 < CrossPointSettings::LINE_COMPRESSION_COUNT ? static_cast<uint8_t>(legacy + 1)
                                                                 : CrossPointSettings::LINE_SPACING_120;
}

}  // namespace

CrossPointSettings::TextProfile CrossPointSettings::liveTextProfile() const {
  TextProfile p{fontPointSize, lineSpacing, characterSpacing, wordSpacing, fontFamily, {}};
  copyToField(p.sdFontFamilyName, sdFontFamilyName, sizeof(p.sdFontFamilyName));
  return p;
}

void CrossPointSettings::setLiveTextProfile(const TextProfile& p) {
  fontPointSize = p.fontPointSize;
  lineSpacing = p.lineSpacing;
  characterSpacing = p.characterSpacing;
  wordSpacing = p.wordSpacing;
  fontFamily = p.fontFamily;
  copyToField(sdFontFamilyName, p.sdFontFamilyName, sizeof(sdFontFamilyName));
}

void CrossPointSettings::holdLatinTextProfile(const TextProfile& latin) {
  if (latinIsHeld) {
    LOG_ERR("CPS", "Latin text profile already held; keeping the first");
    return;
  }
  latinHeld = latin;
  latinIsHeld = true;
}

CrossPointSettings::TextProfile CrossPointSettings::textProfile(const TextProfileId id) const {
  if (id != TextProfileId::Latin) return scriptTextProfiles[static_cast<int>(id) - 1];
  return latinIsHeld ? latinHeld : liveTextProfile();
}

void CrossPointSettings::setTextProfile(const TextProfileId id, const TextProfile& p) {
  if (id == TextProfileId::Latin) {
    if (latinIsHeld) {
      latinHeld = p;
    } else {
      setLiveTextProfile(p);
    }
  } else {
    scriptTextProfiles[static_cast<int>(id) - 1] = p;
  }
}

void CrossPointSettings::validateFrontButtonMapping(CrossPointSettings& settings) {
  const uint8_t mapping[] = {settings.frontButtonBack, settings.frontButtonConfirm, settings.frontButtonLeft,
                             settings.frontButtonRight};
  for (size_t i = 0; i < 4; i++) {
    for (size_t j = i + 1; j < 4; j++) {
      if (mapping[i] == mapping[j]) {
        settings.frontButtonBack = FRONT_HW_BACK;
        settings.frontButtonConfirm = FRONT_HW_CONFIRM;
        settings.frontButtonLeft = FRONT_HW_LEFT;
        settings.frontButtonRight = FRONT_HW_RIGHT;
        return;
      }
    }
  }
}

uint8_t CrossPointSettings::sleepTimeoutEnumToMinutes(const uint8_t legacyValue) {
  switch (legacyValue) {
    case SLEEP_1_MIN:
      return 1;
    case SLEEP_5_MIN:
      return 5;
    case SLEEP_15_MIN:
      return 15;
    case SLEEP_30_MIN:
      return 30;
    case SLEEP_10_MIN:
    default:
      return 10;
  }
}

void CrossPointSettings::toJson(JsonDocument& doc) const {
  const CrossPointSettings& s = *this;

  // forEachPersistableSetting(), not getSettingsList(): copying the menu table to iterate it aborts
  // the firmware on a fragmented heap, and this runs while the reader is tearing down. The walk
  // covers the rows the menu copy adds as well, so their values are written like any other.
  forEachPersistableSetting([&](const SettingInfo& info) {
    if (!info.key) return;
    // Dynamic entries (KOReader etc.) are stored in their own files — skip.
    if (!info.valuePtr && !info.stringOffset) return;

    if (info.stringOffset) {
      const char* strPtr = (const char*)&s + info.stringOffset;
      if (info.obfuscated) {
        char obfKey[OBF_KEY_BUF];
        snprintf(obfKey, sizeof(obfKey), "%s_obf", info.key);
        doc[obfKey] = obfuscation::obfuscateToBase64(strPtr);
      } else {
        doc[info.key] = strPtr;
      }
    } else {
      doc[info.key] = s.*(info.valuePtr);
    }
  });

  // Front button remap — managed by RemapFrontButtons sub-activity, not in SettingsList.
  doc["frontButtonBack"] = frontButtonBack;
  doc["frontButtonConfirm"] = frontButtonConfirm;
  doc["frontButtonLeft"] = frontButtonLeft;
  doc["frontButtonRight"] = frontButtonRight;
  // Font family and size — both use dynamic getter/setters in SettingsList (the
  // option lists depend on the SD font registry), so the generic loop skips them.
  doc["fontFamily"] = fontFamily;
  doc["fontSize"] = fontPointSize;
  // SD card font family name — not in SettingsList, save manually
  if (sdFontFamilyName[0] != '\0') {
    doc["sdFontFamilyName"] = sdFontFamilyName;
  }
  // A swapped-in profile is in the live fields; the Latin settings are the held ones.
  if (latinIsHeld) {
    doc["fontFamily"] = latinHeld.fontFamily;
    doc["fontSize"] = latinHeld.fontPointSize;
    doc["lineHeight"] = latinHeld.lineSpacing;
    doc["characterSpacing"] = latinHeld.characterSpacing;
    doc["wordSpacing"] = latinHeld.wordSpacing;
    if (latinHeld.sdFontFamilyName[0] != '\0') {
      doc["sdFontFamilyName"] = latinHeld.sdFontFamilyName;
    } else {
      doc.remove("sdFontFamilyName");
    }
  }
  JsonObject profiles = doc["textProfiles"].to<JsonObject>();
  for (int i = 1; i < static_cast<int>(TextProfileId::Count); i++) {
    const TextProfile& p = scriptTextProfiles[i - 1];
    JsonObject o = profiles[TEXT_PROFILE_KEYS[i]].to<JsonObject>();
    o["fontSize"] = p.fontPointSize;
    o["lineHeight"] = p.lineSpacing;
    o["characterSpacing"] = p.characterSpacing;
    o["wordSpacing"] = p.wordSpacing;
    o["fontFamily"] = p.fontFamily;
    if (p.sdFontFamilyName[0] != '\0') o["sdFontFamilyName"] = p.sdFontFamilyName;
  }
  // Dictionary folder name — uses dynamic getter/setter in SettingsList, save manually
  if (dictionaryName[0] != '\0') {
    doc["dictionaryName"] = dictionaryName;
  }
  // The displayed option list varies by board, so this setting uses a dynamic
  // display-index mapping and is skipped by the generic persistence loop.
  doc["longPressMenuFunction"] = longPressMenuFunction;
  doc["shortPwrBtn"] = shortPwrBtn;

  // Language -- managed by LanguageSelectActivity, not in SettingsList.
  // Stored as ISO code string ("EN", "DE", ...) for stability across enum reorders.
  doc["language"] = (language < getLanguageCount()) ? LANGUAGE_CODES[language] : "EN";

  // A uint16_t mask, so it does not fit the uint8_t generic loop. Omitted while
  // unconfigured, so the default keeps following the UI language.
  if (keyboardLayouts != 0) {
    doc["keyboardLayouts"] = keyboardLayouts;
  }

  bleturner::writeJson(ble, doc);
}

bool CrossPointSettings::fromJson(JsonVariantConst doc) {
  CrossPointSettings& s = *this;
  bool needsResave = false;

  auto clamp = [](uint8_t val, uint8_t maxVal, uint8_t def) -> uint8_t { return val < maxVal ? val : def; };

  // forEachPersistableSetting(), not getSettingsList(): copying the menu table to iterate it aborts
  // the firmware on a fragmented heap, and this runs while the reader is tearing down. The walk
  // covers the rows the menu copy adds as well, so their values are written like any other.
  forEachPersistableSetting([&](const SettingInfo& info) {
    if (!info.key) return;
    // Dynamic entries (KOReader etc.) are stored in their own files — skip.
    if (!info.valuePtr && !info.stringOffset) return;

    if (info.stringOffset) {
      // destPtr starts out holding the struct-initializer default; it stays that
      // way unless the document actually carries a value for this key.
      char* destPtr = (char*)&s + info.stringOffset;
      if (info.stringMaxLen == 0) {
        LOG_ERR("CPS", "Misconfigured SettingInfo: stringMaxLen is 0 for key '%s'", info.key);
        destPtr[0] = '\0';
        needsResave = true;
        return;
      }

      bool loaded = false;
      if (info.obfuscated) {
        char obfKey[OBF_KEY_BUF];
        snprintf(obfKey, sizeof(obfKey), "%s_obf", info.key);
        bool ok = false;
        bool tooLong = false;
        const std::string decoded =
            obfuscation::deobfuscateFromBase64(doc[obfKey] | "", info.stringMaxLen - 1, &ok, &tooLong);
        if (tooLong) {
          LOG_ERR("CPS", "Oversized obfuscated value for key '%s'", info.key);
          needsResave = true;
        }
        if (ok && !decoded.empty()) {
          copyToField(destPtr, decoded.c_str(), info.stringMaxLen);
          loaded = true;
        }
      }
      if (!loaded) {
        // Read as const char*, never `| std::string(...)`: ArduinoJson's
        // std::string converter drags a per-TU copy of the serializer into
        // flash. See the note in PersistableStore.h.
        const char* raw = doc[info.key].is<const char*>() ? doc[info.key].as<const char*>() : nullptr;
        if (raw) {
          // Obfuscated field recovered from a legacy plaintext value -> resave.
          if (info.obfuscated && strcmp(raw, destPtr) != 0) needsResave = true;
          copyToField(destPtr, raw, info.stringMaxLen);
        }
      }
    } else {
      const uint8_t fieldDefault = s.*(info.valuePtr);  // struct-initializer default, read before we overwrite it
      uint8_t v = doc[info.key] | fieldDefault;
      if (info.type == SettingType::ENUM) {
        v = clamp(v, (uint8_t)info.enumLabels().size(), fieldDefault);
      } else if (info.type == SettingType::TOGGLE) {
        v = clamp(v, (uint8_t)2, fieldDefault);
      } else if (info.type == SettingType::VALUE) {
        if (v < info.valueRange.min)
          v = info.valueRange.min;
        else if (v > info.valueRange.max)
          v = info.valueRange.max;
      }
      s.*(info.valuePtr) = v;
    }
  });

  // Plugin System did not exist in older settings files. Preserve the behavior
  // of users who already have SD plugins installed, while keeping the new
  // default disabled on clean installs.
  if (doc["pluginsEnabled"].isNull()) {
    pluginsEnabled = PluginLocations::anyPluginInstalledOnDisk() ? 1 : 0;
    needsResave = true;
  }

  const auto indentSpaces = doc["paragraphIndentSpaces"];
  const bool hasSavedWidth = indentSpaces.is<int>();
  const int savedWidth = hasSavedWidth ? indentSpaces.as<int>() : 0;
  paragraphIndentSpaces = migrateParagraphIndentSpaces(hasSavedWidth, savedWidth, extraParagraphSpacing != 0);
  if (!hasSavedWidth || savedWidth < 0 || savedWidth > 5) needsResave = true;

  // Older files stored one combined touch mode under "touchReaderControls": 0=off, 1=tap,
  // 2=swipe, 3=inverted tap, 4=inverted swipe (Matcha-only, for right-to-left vertical text).
  // Split it into the master toggle plus the per-direction gesture pair (the generic loop above
  // already folded out-of-range toggle values back to the On default).
  if (doc["pageTurnGesture"].isNull() && doc["previousPageGesture"].isNull() &&
      doc["touchReaderControls"].is<uint8_t>()) {
    const uint8_t mode = doc["touchReaderControls"].as<uint8_t>();
    if (mode >= LEGACY_TOUCH_TAP && mode <= LEGACY_TOUCH_INVERTED_SWIPE) {
      touchReaderControls = TOUCH_READER_ON;
      switch (mode) {
        case LEGACY_TOUCH_TAP:
          pageTurnGesture = TAP_ONLY;
          break;
        case LEGACY_TOUCH_INVERTED_TAP:
          pageTurnGesture = INVERTED_TAP;
          break;
        case LEGACY_TOUCH_INVERTED_SWIPE:
          pageTurnGesture = INVERTED_SWIPE;
          break;
        default:
          pageTurnGesture = SWIPE_ONLY;
          break;
      }
      previousPageGesture = pageTurnGesture;
      needsResave = true;
    }
  }

  if (doc["sleepTimeoutMinutes"].isNull() && !doc["sleepTimeout"].isNull()) {
    const uint8_t legacyValue =
        clamp(doc["sleepTimeout"] | (uint8_t)SLEEP_10_MIN, SLEEP_TIMEOUT_COUNT, (uint8_t)SLEEP_10_MIN);
    sleepTimeoutMinutes = sleepTimeoutEnumToMinutes(legacyValue);
    needsResave = true;
  }
  // Pre-1.5 "Side Button Layout" -> the per-button actions. Only when the new keys are absent,
  // so a device that has already been through the new screen is never overwritten. Prev/Next was
  // the default and needs nothing; Next/Prev becomes an explicit swap, Disabled becomes Off on
  // both buttons. Dropping the key without this silently reset every non-default side layout.
  // Board-independent, like the rows themselves: a card carrying the legacy key reads the same
  // on whichever device it is put into.
  if (doc["upperSideButtonAction"].isNull() && doc["lowerSideButtonAction"].isNull() &&
      !doc["sideButtonLayout"].isNull()) {
    switch (doc["sideButtonLayout"] | (uint8_t)LEGACY_PREV_NEXT) {
      case LEGACY_NEXT_PREV:
        upperSideButtonAction = SIDE_BTN_NEXT_PAGE;
        lowerSideButtonAction = SIDE_BTN_PREV_PAGE;
        needsResave = true;
        break;
      case LEGACY_SIDE_DISABLED:
        upperSideButtonAction = SIDE_BTN_NONE;
        lowerSideButtonAction = SIDE_BTN_NONE;
        needsResave = true;
        break;
      default:
        break;
    }
  }

  // "lineSpacing" saves predate the 1.0 option: Tight/Normal/Wide/Extra Wide (0..3) become 1.1..1.45.
  if (doc["lineHeight"].isNull() && doc["lineSpacing"].is<uint8_t>()) {
    lineSpacing = legacyLineSpacing(doc["lineSpacing"]);
    needsResave = true;
  }
  // Front button remap — managed by RemapFrontButtons sub-activity, not in SettingsList.
  frontButtonBack = clamp(doc["frontButtonBack"] | (uint8_t)FRONT_HW_BACK, FRONT_BUTTON_HARDWARE_COUNT, FRONT_HW_BACK);
  frontButtonConfirm =
      clamp(doc["frontButtonConfirm"] | (uint8_t)FRONT_HW_CONFIRM, FRONT_BUTTON_HARDWARE_COUNT, FRONT_HW_CONFIRM);
  frontButtonLeft = clamp(doc["frontButtonLeft"] | (uint8_t)FRONT_HW_LEFT, FRONT_BUTTON_HARDWARE_COUNT, FRONT_HW_LEFT);
  frontButtonRight =
      clamp(doc["frontButtonRight"] | (uint8_t)FRONT_HW_RIGHT, FRONT_BUTTON_HARDWARE_COUNT, FRONT_HW_RIGHT);
  validateFrontButtonMapping(s);

  // Reader font size — an actual point size since 1.5. Files written by 1.4 and
  // earlier hold the old SMALL/MEDIUM/LARGE/EXTRA_LARGE slot in 0..3; no font is
  // renderable at those sizes, so the range is unambiguous and folds to the
  // point sizes those slots used to mean. Drop this once 1.4 upgrades are done.
  uint8_t storedFontSize = doc["fontSize"] | DEFAULT_FONT_POINT_SIZE;
  if (storedFontSize <= LEGACY_FONT_SIZE_MAX) {
    storedFontSize = 12 + storedFontSize * 2;  // 0,1,2,3 -> 12,14,16,18
    needsResave = true;
  }
  fontPointSize = storedFontSize;

  // Font family — uses dynamic getter/setter in SettingsList so the generic loop skips it.
  const uint8_t storedFontFamily = doc["fontFamily"] | (uint8_t)0;
  fontFamily = clamp(storedFontFamily, BUILTIN_FONT_COUNT, 0);
  // These controls use board-specific display-index mappings and are skipped by
  // the generic settings loop. Persisted action IDs remain stable across boards.
  longPressMenuFunction = clamp(doc["longPressMenuFunction"] | (uint8_t)LP_MENU_DISABLED,
                                (uint8_t)LONG_PRESS_MENU_FUNCTION_COUNT, (uint8_t)LP_MENU_DISABLED);
  shortPwrBtn = clamp(doc["shortPwrBtn"] | (uint8_t)IGNORE, (uint8_t)SHORT_PWRBTN_COUNT, (uint8_t)IGNORE);
  if (BoardConfig::hasHomeKey() && doc["homeButtonLongPressAction"].isNull() &&
      !doc["longPressMenuFunction"].isNull()) {
    static constexpr HomeButtonAction LEGACY[] = {HomeButtonAction::Sync,       HomeButtonAction::Ignore,
                                                  HomeButtonAction::Bookmark,   HomeButtonAction::Dictionary,
                                                  HomeButtonAction::ReaderMenu, HomeButtonAction::CreateClipping};
    if (s.longPressMenuFunction < sizeof(LEGACY) / sizeof(LEGACY[0])) {
      s.homeButtonLongPressAction = static_cast<uint8_t>(LEGACY[s.longPressMenuFunction]);
      needsResave = true;
    }
  }

  // SD card font family name — not in SettingsList, load manually
  const char* sfn = doc["sdFontFamilyName"] | "";
  strncpy(sdFontFamilyName, sfn, sizeof(sdFontFamilyName) - 1);
  sdFontFamilyName[sizeof(sdFontFamilyName) - 1] = '\0';
  if (sdFontFamilyName[0] == '\0') {
    switch (storedFontFamily) {
      case LEGACY_NOTOSANS:
        copyToField(sdFontFamilyName, "NotoSans", sizeof(sdFontFamilyName));
        break;
      case LEGACY_OPENDYSLEXIC:
        copyToField(sdFontFamilyName, "OpenDyslexic", sizeof(sdFontFamilyName));
        break;
    }
  }
  if (storedFontFamily >= BUILTIN_FONT_COUNT) needsResave = true;
  // Dictionary folder name — uses dynamic getter/setter in SettingsList, load manually
  copyToField(dictionaryName, doc["dictionaryName"] | "", sizeof(dictionaryName));

  // Text profiles. Before there were five, Japanese and Chinese shared one CJK profile without a
  // font: those three start from it with the current font, Korean from the Latin settings.
  {
    const TextProfile latin = liveTextProfile();
    TextProfile cjk = latin;
    if (!doc["cjkFontPointSize"].isNull()) {
      cjk.fontPointSize = doc["cjkFontPointSize"] | latin.fontPointSize;
      // Saved before the 1.0 option, so one slot up, as for "lineSpacing" above.
      if (doc["cjkLineSpacing"].is<uint8_t>()) cjk.lineSpacing = legacyLineSpacing(doc["cjkLineSpacing"]);
      cjk.characterSpacing = doc["cjkCharacterSpacing"] | latin.characterSpacing;
      cjk.wordSpacing = doc["cjkWordSpacing"] | latin.wordSpacing;
    }
    const JsonVariantConst profiles = doc["textProfiles"];
    if (profiles.isNull()) needsResave = true;
    for (int i = 1; i < static_cast<int>(TextProfileId::Count); i++) {
      const auto id = static_cast<TextProfileId>(i);
      TextProfile p = id == TextProfileId::Korean ? latin : cjk;
      const JsonVariantConst o = profiles[TEXT_PROFILE_KEYS[i]];
      if (!o.isNull()) {
        const uint8_t size = o["fontSize"] | p.fontPointSize;
        if (size >= 6 && size <= 72) p.fontPointSize = size;
        if (o["lineHeight"].is<uint8_t>()) {
          p.lineSpacing = clamp(o["lineHeight"].as<uint8_t>(), (uint8_t)LINE_COMPRESSION_COUNT, p.lineSpacing);
        } else if (o["lineSpacing"].is<uint8_t>()) {
          p.lineSpacing = legacyLineSpacing(o["lineSpacing"]);
          needsResave = true;
        }
        p.characterSpacing = clamp(o["characterSpacing"] | p.characterSpacing,
                                   (uint8_t)(2 * CHARACTER_SPACING_OFFSET + 1), p.characterSpacing);
        const uint8_t words = o["wordSpacing"] | p.wordSpacing;
        if (words >= WORD_SPACING_MIN && words <= WORD_SPACING_MAX) p.wordSpacing = words;
        p.fontFamily = clamp(o["fontFamily"] | p.fontFamily, BUILTIN_FONT_COUNT, (uint8_t)0);
        copyToField(p.sdFontFamilyName, o["sdFontFamilyName"] | "", sizeof(p.sdFontFamilyName));
      }
      scriptTextProfiles[i - 1] = p;
    }
  }
  // Language -- stored as code string for stability across enum reorders.
  if (doc["language"].is<const char*>()) {
    language = static_cast<uint8_t>(I18n::languageFromCode(doc["language"].as<const char*>()));
  }

  // Absent means unconfigured, which is the default.
  if (doc["keyboardLayouts"].is<uint16_t>()) {
    keyboardLayouts = doc["keyboardLayouts"].as<uint16_t>();
  }

  if (!bleturner::readJson(ble, doc)) {
    needsResave = true;
  }

  if (needsResave) {
    LOG_DBG("CPS", "Resaving settings to update format");
    requestResave();
  }

  LOG_DBG("CPS", "Settings loaded from file");

  return true;
}

CrossPointSettings::StatusBarSpec CrossPointSettings::statusBarSpec() const {
  StatusBarSpec spec;
  spec.showChapterPageCount = statusBarChapterPageCount != 0;
  spec.showBookProgressPercent = statusBarBookProgressPercentage != 0;
  spec.titleMode = statusBarTitle;
  spec.showBattery = statusBarBattery != 0;
  spec.showBatteryPercent = hideBatteryPercentage == HIDE_NEVER;
  spec.clockMode = statusBarClock;
  spec.clock12h = clockFormat == 1;
  spec.progressBarMode = statusBarProgressBar;
  spec.progressBarHeightPx =
      statusBarProgressBar != HIDE_PROGRESS ? static_cast<uint8_t>((statusBarProgressBarThickness + 1) * 2) : 0;
  spec.xtcMode = xtcStatusBarMode;
  spec.timeLeftMode = statusBarTimeLeft;
  return spec;
}

ReaderRenderSpec CrossPointSettings::readerRenderSpec(const uint16_t viewportWidth,
                                                      const uint16_t viewportHeight) const {
  ReaderRenderSpec spec;
  spec.fontId = getReaderFontId();
  spec.lineCompression = getReaderLineCompression();
  // Vertical keeps its three column gaps: 100-110% tight, 120% normal, 130% and up wide.
  spec.lineSpacingLevel = static_cast<uint8_t>(std::clamp(lineSpacing - 1, 0, 2));
  spec.characterSpacing = getCharacterSpacing();
  spec.wordSpacingPercent = wordSpacing;
  spec.extraParagraphSpacing = extraParagraphSpacing != 0;
  spec.paragraphIndentSpaces = paragraphIndentSpaces;
  spec.paragraphAlignment = paragraphAlignment;
  spec.viewportWidth = viewportWidth;
  spec.viewportHeight = viewportHeight;
  spec.hyphenationEnabled = hyphenationEnabled != 0;
  spec.embeddedStyle = embeddedStyle != 0;
  // Same layout as Display, so switching between them keeps the section cache.
  spec.imageRendering = imageRendering == IMAGES_DISPLAY_BW ? static_cast<uint8_t>(IMAGES_DISPLAY) : imageRendering;
  spec.focusReadingEnabled = focusReadingEnabled != 0;
  spec.honorBookInsets = bookCssMargins != 0;
  return spec;
}

float CrossPointSettings::getReaderLineCompression() const {
  return LINE_SPACING_MULTIPLIERS[lineSpacing < LINE_COMPRESSION_COUNT ? lineSpacing : LINE_SPACING_120];
}

unsigned long CrossPointSettings::getSleepTimeoutMs() const {
  if (sleepTimeoutMinutes >= SLEEP_TIMEOUT_NEVER_MINUTES) return 0UL;
  const uint8_t minutes =
      std::clamp(sleepTimeoutMinutes, MIN_SLEEP_TIMEOUT_MINUTES, static_cast<uint8_t>(SLEEP_TIMEOUT_NEVER_MINUTES - 1));
  return static_cast<unsigned long>(minutes) * 60UL * 1000UL;
}

int CrossPointSettings::getRefreshFrequency() const {
  switch (refreshFrequency) {
    case REFRESH_1:
      return 1;
    case REFRESH_5:
      return 5;
    case REFRESH_10:
      return 10;
    case REFRESH_15:
    default:
      return 15;
    case REFRESH_30:
      return 30;
    case REFRESH_NEVER:
      // Effectively disables the periodic full refresh; the page counter
      // counts down from here and never reaches the threshold in practice.
      return std::numeric_limits<int>::max();
  }
}

void CrossPointSettings::clearSdFontFamily() {
  sdFontFamilyName[0] = '\0';
  saveToFile();
}

int CrossPointSettings::getBuiltinReaderFontId() const { return getBuiltinSerifReaderFontId(); }

int CrossPointSettings::getBuiltinSerifReaderFontId() const {
  const uint8_t pt =
      snapToNearestPointSize(BUILTIN_READER_POINT_SIZES, std::size(BUILTIN_READER_POINT_SIZES), fontPointSize);
  switch (pt) {
    case 12:
      return NOTOSERIF_12_FONT_ID;
    case 16:
      return NOTOSERIF_16_FONT_ID;
    case 18:
      return NOTOSERIF_18_FONT_ID;
    case 14:
    default:
      return NOTOSERIF_14_FONT_ID;
  }
}

int CrossPointSettings::getReaderFontId() const {
  // Asked unconditionally, not just when an SD family is named: a built-in selection whose
  // picker row collapsed a wider-coverage variant (NotoSerif + NotoSerifExtended) resolves to
  // that variant, and the resolver answers 0 when nothing stands in.
  if (sdFontIdResolver) {
    int id = sdFontIdResolver(sdFontResolverCtx, sdFontFamilyName, fontPointSize);
    if (id != 0) return id;
  }

  return getBuiltinReaderFontId();
}
