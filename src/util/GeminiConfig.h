#pragma once

#include <I18n.h>

#include <cstdint>
#include <string>

// The Gemini API key and model used by page translation, kept as two small text files in the
// system folder (gemini.key, gemini.model) so a card set up by hand keeps working. The device's
// Translation settings and the web Settings page both read and write them through here.
namespace gemini {

constexpr const char* DEFAULT_MODEL = "gemini-3.8-flash";
// Tried when the model in use is busy or not offered to the key; also offered in the settings.
// An alias, so it follows Google's current lite model.
constexpr const char* LITE_MODEL = "gemini-flash-lite-latest";

// The key, trimmed; false when there is none.
bool readKey(std::string& keyOut);
// Saves the key; an empty one removes the file.
bool writeKey(const std::string& key);
// The key's last four characters behind dots, or "" when there is none.
std::string maskedKey();

// The model the reader chose, or "" for the default.
std::string readModel();
// Saves the model; "" (or the default's own name) removes the file.
bool writeModel(const std::string& model);

// The model setting as a choice: the default, the lite model, or any other name.
enum class ModelChoice : uint8_t { Default, Lite, Custom };
ModelChoice modelChoice();

// Languages page translation can translate into: the label shown in the settings, and the
// English name the request asks the model for. Persisted by index: append only.
struct TargetLanguage {
  StrId label;
  const char* promptName;
};
constexpr TargetLanguage TARGET_LANGUAGES[] = {
    {StrId::STR_XLAT_LANG_EN, "English"},
    {StrId::STR_XLAT_LANG_DE, "German"},
    {StrId::STR_XLAT_LANG_FR, "French"},
    {StrId::STR_XLAT_LANG_ES, "Spanish"},
    {StrId::STR_XLAT_LANG_IT, "Italian"},
    {StrId::STR_XLAT_LANG_PT, "Portuguese"},
    {StrId::STR_XLAT_LANG_NL, "Dutch"},
    {StrId::STR_XLAT_LANG_HU, "Hungarian"},
    {StrId::STR_XLAT_LANG_PL, "Polish"},
    {StrId::STR_XLAT_LANG_RU, "Russian"},
    {StrId::STR_XLAT_LANG_UK, "Ukrainian"},
    {StrId::STR_XLAT_LANG_JA, "Japanese"},
    {StrId::STR_XLAT_LANG_ZH, "Simplified Chinese"},
    {StrId::STR_XLAT_LANG_KO, "Korean"},
};
constexpr uint8_t TARGET_LANGUAGE_COUNT = sizeof(TARGET_LANGUAGES) / sizeof(TARGET_LANGUAGES[0]);
// The translationTarget value that means "the language typed in gemini.lang".
constexpr uint8_t TARGET_CUSTOM = TARGET_LANGUAGE_COUNT;
constexpr size_t CUSTOM_LANGUAGE_MAX = 40;

// The typed target language, trimmed, or "" when there is none.
std::string readCustomLanguage();
// Saves it; "" removes the file.
bool writeCustomLanguage(const std::string& language);
// The language name to put in the request for a translationTarget value. Custom with nothing
// typed falls back to English.
std::string targetLanguageName(uint8_t index);
// The label for the settings row: the language's name, or the typed text for Custom.
std::string targetLanguageLabel(uint8_t index);

}  // namespace gemini
