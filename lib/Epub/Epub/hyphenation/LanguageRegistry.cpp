#include "LanguageRegistry.h"

// Pattern licenses and copyright notices: licenses/hyphenation.txt.

#include <Logging.h>

#include <array>
#include <cstring>

#include "HyphenationCommon.h"
#include "HyphenationPack.h"
#include "generated/hyph-en.trie.h"

namespace {

// Filled by HyphenationPack::load(); every pack language reads its patterns from here.
SerializedHyphenationPatterns packPatterns{0, nullptr, 0};

// English hyphenation patterns (3/3 minimum prefix/suffix length)
LanguageHyphenator englishHyphenator(en_patterns, isLatinLetter, toLowerLatin, 3, 3);
LanguageHyphenator frenchHyphenator(packPatterns, isLatinLetter, toLowerLatin);
LanguageHyphenator germanHyphenator(packPatterns, isLatinLetter, toLowerLatin);
LanguageHyphenator russianHyphenator(packPatterns, isCyrillicLetter, toLowerCyrillic);
LanguageHyphenator spanishHyphenator(packPatterns, isLatinLetter, toLowerLatin);
LanguageHyphenator italianHyphenator(packPatterns, isLatinLetter, toLowerLatin);
LanguageHyphenator swedishHyphenator(packPatterns, isLatinLetter, toLowerLatin);
LanguageHyphenator ukrainianHyphenator(packPatterns, isCyrillicLetter, toLowerCyrillic);
LanguageHyphenator polishHyphenator(packPatterns, isLatinLetter, toLowerLatin);
LanguageHyphenator finnishHyphenator(packPatterns, isLatinLetter, toLowerLatin);
LanguageHyphenator portugueseHyphenator(packPatterns, isLatinLetter, toLowerLatin);
LanguageHyphenator afrikaansHyphenator(packPatterns, isLatinLetter, toLowerLatin);
LanguageHyphenator catalanHyphenator(packPatterns, isLatinLetter, toLowerLatin);
LanguageHyphenator czechHyphenator(packPatterns, isLatinLetter, toLowerLatin);
LanguageHyphenator danishHyphenator(packPatterns, isLatinLetter, toLowerLatin);
LanguageHyphenator estonianHyphenator(packPatterns, isLatinLetter, toLowerLatin);
LanguageHyphenator galicianHyphenator(packPatterns, isLatinLetter, toLowerLatin);
LanguageHyphenator croatianHyphenator(packPatterns, isLatinLetter, toLowerLatin);
LanguageHyphenator hungarianHyphenator(packPatterns, isLatinLetter, toLowerLatin);
LanguageHyphenator icelandicHyphenator(packPatterns, isLatinLetter, toLowerLatin);
LanguageHyphenator latinHyphenator(packPatterns, isLatinLetter, toLowerLatin);
LanguageHyphenator lithuanianHyphenator(packPatterns, isLatinLetter, toLowerLatin);
LanguageHyphenator dutchHyphenator(packPatterns, isLatinLetter, toLowerLatin);
LanguageHyphenator norwegianHyphenator(packPatterns, isLatinLetter, toLowerLatin);
LanguageHyphenator slovakHyphenator(packPatterns, isLatinLetter, toLowerLatin);
LanguageHyphenator slovenianHyphenator(packPatterns, isLatinLetter, toLowerLatin);
LanguageHyphenator albanianHyphenator(packPatterns, isLatinLetter, toLowerLatin);
LanguageHyphenator turkmenHyphenator(packPatterns, isLatinLetter, toLowerLatin);
LanguageHyphenator turkishHyphenator(packPatterns, isLatinLetter, toLowerLatin);
LanguageHyphenator kurdishHyphenator(packPatterns, isLatinLetter, toLowerLatin);
LanguageHyphenator belarusianHyphenator(packPatterns, isCyrillicLetter, toLowerCyrillic);
LanguageHyphenator bulgarianHyphenator(packPatterns, isCyrillicLetter, toLowerCyrillic);
LanguageHyphenator mongolianHyphenator(packPatterns, isCyrillicLetter, toLowerCyrillic);
LanguageHyphenator serbianHyphenator(packPatterns, isCyrillicLetter, toLowerCyrillic);
LanguageHyphenator greekHyphenator(packPatterns, isGreekLetter, toLowerGreek);
LanguageHyphenator georgianHyphenator(packPatterns, isGeorgianLetter, toLowerGeorgian);

constexpr size_t kLanguageCount = 36;
// English first, then the pack languages in the order menus list them (by their own names).
const std::array<LanguageEntry, kLanguageCount> kEntries = {
    {{"english", "en", "English", &englishHyphenator, true},
     {"afrikaans", "af", "Afrikaans", &afrikaansHyphenator, false},
     {"catalan", "ca", "Català", &catalanHyphenator, false},
     {"czech", "cs", "Čeština", &czechHyphenator, false},
     {"danish", "da", "Dansk", &danishHyphenator, false},
     {"german", "de", "Deutsch", &germanHyphenator, false},
     {"estonian", "et", "Eesti", &estonianHyphenator, false},
     {"greek", "el", "Ellinika", &greekHyphenator, false},
     {"spanish", "es", "Español", &spanishHyphenator, false},
     {"french", "fr", "Français", &frenchHyphenator, false},
     {"galician", "gl", "Galego", &galicianHyphenator, false},
     {"croatian", "hr", "Hrvatski", &croatianHyphenator, false},
     {"icelandic", "is", "Íslenska", &icelandicHyphenator, false},
     {"italian", "it", "Italiano", &italianHyphenator, false},
     {"georgian", "ka", "Kartuli", &georgianHyphenator, false},
     {"kurdish", "ku", "Kurdî", &kurdishHyphenator, false},
     {"latin", "la", "Latina", &latinHyphenator, false},
     {"lithuanian", "lt", "Lietuvių", &lithuanianHyphenator, false},
     {"hungarian", "hu", "Magyar", &hungarianHyphenator, false},
     {"dutch", "nl", "Nederlands", &dutchHyphenator, false},
     {"norwegian", "no", "Norsk", &norwegianHyphenator, false},
     {"polish", "pl", "Polski", &polishHyphenator, false},
     {"portuguese", "pt", "Português", &portugueseHyphenator, false},
     {"albanian", "sq", "Shqip", &albanianHyphenator, false},
     {"slovak", "sk", "Slovenčina", &slovakHyphenator, false},
     {"slovenian", "sl", "Slovenščina", &slovenianHyphenator, false},
     {"finnish", "fi", "Suomi", &finnishHyphenator, false},
     {"swedish", "sv", "Svenska", &swedishHyphenator, false},
     {"turkish", "tr", "Türkçe", &turkishHyphenator, false},
     {"turkmen", "tk", "Türkmençe", &turkmenHyphenator, false},
     {"belarusian", "be", "Беларуская", &belarusianHyphenator, false},
     {"bulgarian", "bg", "Български", &bulgarianHyphenator, false},
     {"mongolian", "mn", "Монгол", &mongolianHyphenator, false},
     {"russian", "ru", "Русский", &russianHyphenator, false},
     {"serbian", "sr", "Српски", &serbianHyphenator, false},
     {"ukrainian", "uk", "Українська", &ukrainianHyphenator, false}}};

const LanguageEntry* loadedPack = nullptr;
// A pack found missing is not looked for again (a page turn probes several chapters) until
// resetHyphenationPacks(). A pack that failed to load is: the failure may be "not right now".
bool missing[kLanguageCount] = {};

}  // namespace

const LanguageEntry* hyphenationLanguages(size_t* count) {
  *count = kEntries.size();
  return kEntries.data();
}

const LanguageEntry* hyphenationLanguage(const std::string& primaryTag) {
  for (const auto& entry : kEntries) {
    if (primaryTag == entry.primaryTag) return &entry;
  }
  return nullptr;
}

const LanguageHyphenator* getLanguageHyphenatorForPrimaryTag(const std::string& primaryTag, const bool loadPack) {
  const LanguageEntry* entry = hyphenationLanguage(primaryTag);
  if (entry == nullptr) return nullptr;
  if (entry->builtIn) return entry->hyphenator;
  if (!loadPack) {
    // Hyphenation is off: the pack's flash slots or PSRAM go back.
    if (entry == loadedPack) resetHyphenationPacks();
    return nullptr;
  }
  if (entry == loadedPack) return entry->hyphenator;
  const size_t index = static_cast<size_t>(entry - kEntries.data());
  if (missing[index]) return nullptr;

  char path[HyphenationPack::PATH_SIZE];
  if (!HyphenationPack::pathFor(entry->primaryTag, path, sizeof(path))) return nullptr;
  loadedPack = nullptr;
  HyphenationPack::release();
  if (!HyphenationPack::exists(path)) {
    missing[index] = true;
    return nullptr;
  }
  if (!HyphenationPack::load(path, entry->primaryTag, packPatterns)) return nullptr;
  loadedPack = entry;
  LOG_INF("HYPH", "Loaded %s hyphenation pack (%u bytes)", entry->primaryTag, static_cast<unsigned>(packPatterns.size));
  return entry->hyphenator;
}

void resetHyphenationPacks() {
  loadedPack = nullptr;
  HyphenationPack::release();
  packPatterns = SerializedHyphenationPatterns{0, nullptr, 0};
  memset(missing, 0, sizeof(missing));
}
