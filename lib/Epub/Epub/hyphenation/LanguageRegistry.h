#pragma once

#include <cstddef>
#include <string>

#include "LanguageHyphenator.h"

struct LanguageEntry {
  const char* cliName;
  const char* primaryTag;
  const char* nativeName;  // shown in menus, e.g. "Deutsch"
  const LanguageHyphenator* hyphenator;
  bool builtIn;  // false: the patterns come from a downloaded pack
};

// Every language with hyphenation rules, built in or from a pack.
const LanguageEntry* hyphenationLanguages(size_t* count);

// The entry for a primary language tag (e.g. "en", "fr"), or null.
const LanguageEntry* hyphenationLanguage(const std::string& primaryTag);

// Returns the Liang-backed hyphenator for a primary language tag, or null when it has no patterns
// here. A pack language loads its pack only when loadPack is set; the pack loaded last stays loaded.
const LanguageHyphenator* getLanguageHyphenatorForPrimaryTag(const std::string& primaryTag, bool loadPack);

// Releases the loaded pack and forgets which packs were missing. After installing or removing a pack,
// call Hyphenator::resetPacks(), which also drops the hyphenator in use.
void resetHyphenationPacks();
