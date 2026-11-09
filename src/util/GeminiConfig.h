#pragma once

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

}  // namespace gemini
