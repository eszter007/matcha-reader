#pragma once

#include <I18nKeys.h>

#include <cstddef>
#include <cstdint>
#include <memory>
/**
 * Internationalization (i18n) system for CrossPoint Reader
 */

class I18n {
 public:
  static I18n& getInstance();

  // Disable copy
  I18n(const I18n&) = delete;
  I18n& operator=(const I18n&) = delete;

  // Get localized string by ID
  const char* get(StrId id) const;

  /// A count message: its forms are separated by '|' in the order of the language's plural
  /// categories (PluralRules.h), each formatted with the count ("%d day read|%d days read").
  /// Writes the form for n into out. A message with fewer forms uses its last; one that falls
  /// back to English follows the English rule.
  void formatCount(char* out, size_t size, StrId id, int n) const;
  /// The same for a position in a counted list ("%d/%d book|%d/%d books"): the form follows the
  /// total, and each form takes the position and then the total.
  void formatCountOf(char* out, size_t size, StrId id, int position, int total) const;

  Language getLanguage() const { return _language; }
  /// Select the UI language. A language whose strings are compiled in switches with no I/O; any
  /// other one needs its .cplang pack on the SD card, which is read into a single heap buffer
  /// here. A missing or unusable pack leaves the language unchanged and returns false, so the UI
  /// never ends up pointing at strings that were not loaded.
  bool setLanguage(Language lang);

  enum class PackState : uint8_t {
    BuiltIn,   // compiled in, no pack involved
    Missing,   // no pack on the card
    Outdated,  // a pack is there but was built for other firmware, so it will not load
    Ready,     // a pack that loads
  };
  /// What the language picker shows for `lang`. Reads only the pack header, not the strings.
  static PackState packState(Language lang);

  /// Directory holding the .cplang packs, alongside the SD font roots.
  static constexpr const char* PACK_DIR = "/.crosspoint/lang";
  const char* getLanguageName(Language lang) const;
  static Language languageFromCode(const char* code);
  // Endonym for a language tag, case-insensitive and ignoring any region subtag ("ja-JP" ->
  // "ja"); nullptr when the tag has no shipped UI.
  // Not languageFromCode() + getLanguageName(): that pair answers a miss with Language::EN, so
  // an unknown tag would be labelled "English" instead of reported as unknown.
  static const char* languageNameForCode(const char* code);

  // Get all unique characters used in a specific language
  // Returns a sorted string of unique characters
  static const char* getCharacterSet(Language lang);

 private:
  // The form of id for the count n, NUL-terminated in format (size > 0).
  void countForm(StrId id, int n, char* format, size_t size) const;
  I18n() : _language(Language::EN) { i18n_strings::requireCurrentTable(); }

  // Reads PACK_DIR/<CODE>.cplang into _packBuffer and points _packStrings at it.
  // Leaves both untouched and returns false on any rejection.
  bool loadPack(Language lang);

  // The string for id, and whether it is the English text (English, or a fallback to it).
  const char* lookup(StrId id, bool& english) const;

  Language _language;
  // Backing store for a language that is not compiled in. Held for as long as that language is
  // selected: get() runs in the render loop and must resolve by pointer, never by reading the
  // card. Null whenever the active language is built in.
  std::unique_ptr<uint8_t[]> _packBuffer;
  LangStrings _packStrings{nullptr, nullptr};
};

// Convenience macros
#define tr(id) I18n::getInstance().get(StrId::id)
#define I18N I18n::getInstance()
