#include "I18n.h"

#include <cstddef>
#include <cstring>

#include "I18nStrings.h"

using namespace i18n_strings;

I18n& I18n::getInstance() {
  static I18n instance;
  return instance;
}

const char* I18n::get(StrId id) const {
  const auto index = static_cast<size_t>(id);
  if (index >= static_cast<size_t>(StrId::_COUNT)) {
    return "???";
  }

  // Use generated helper function - no hardcoded switch needed!
  const LangStrings lang = getLanguageStrings(_language);

  // If bit 15 of the offset is set, apply the offset to the English lookup table
  const uint16_t off = lang.offsets[index];
  if (off & 0x8000) return STRINGS_EN_DATA + (off & 0x7FFF);
  return lang.data + off;
}

void I18n::setLanguage(Language lang) {
  if (lang >= Language::_COUNT) {
    return;
  }
  _language = lang;
}

const char* I18n::getLanguageName(Language lang) const {
  const auto index = static_cast<size_t>(lang);
  if (index >= static_cast<size_t>(Language::_COUNT)) {
    return "???";
  }
  return LANGUAGE_NAMES[index];
}

const char* I18n::languageNameForCode(const char* code) {
  if (!code || !*code) return nullptr;
  // Table codes are uppercase enum names; book tags arrive lowercase and may carry a region
  // subtag ("ja-JP"), which is dropped here as it is everywhere else language is normalised.
  char upper[8];
  size_t n = 0;
  for (; code[n] && code[n] != '-' && code[n] != '_' && n < sizeof(upper) - 1; n++) {
    const char c = code[n];
    upper[n] = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
  }
  upper[n] = '\0';
  for (uint8_t i = 0; i < getLanguageCount(); i++) {
    if (strcmp(upper, LANGUAGE_CODES[i]) == 0) return LANGUAGE_NAMES[i];
  }
  return nullptr;
}

Language I18n::languageFromCode(const char* code) {
  for (uint8_t i = 0; i < getLanguageCount(); i++) {
    if (strcmp(code, LANGUAGE_CODES[i]) == 0) return static_cast<Language>(i);
  }
  return Language::EN;
}

// Generate character set for a specific language
const char* I18n::getCharacterSet(Language lang) {
  const auto langIndex = static_cast<size_t>(lang);
  if (langIndex >= static_cast<size_t>(Language::_COUNT)) {
    lang = Language::EN;  // Fallback to first language
  }

  return CHARACTER_SETS[static_cast<size_t>(lang)];
}
