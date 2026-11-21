#include "HyphenationPacks.h"

#include <Epub/hyphenation/HyphenationPack.h>
#include <Epub/hyphenation/Hyphenator.h>
#include <Epub/hyphenation/LanguageRegistry.h>

#include <cstring>

#include "CrossPointState.h"

namespace hyphpacks {

const LanguageEntry* packFor(const std::string& bookLanguage) {
  const LanguageEntry* entry = hyphenationLanguage(Hyphenator::primaryTag(bookLanguage));
  return entry != nullptr && !entry->builtIn ? entry : nullptr;
}

bool isInstalled(const LanguageEntry& language) {
  char path[HyphenationPack::PATH_SIZE];
  return HyphenationPack::pathFor(language.primaryTag, path, sizeof(path)) && HyphenationPack::exists(path);
}

bool wasOffered(const LanguageEntry& language) {
  const std::string& offered = APP_STATE.hyphenationPacksOffered;
  const size_t length = strlen(language.primaryTag);
  for (size_t at = offered.find(language.primaryTag); at != std::string::npos;
       at = offered.find(language.primaryTag, at + 1)) {
    const bool startsWord = at == 0 || offered[at - 1] == ' ';
    const bool endsWord = at + length == offered.size() || offered[at + length] == ' ';
    if (startsWord && endsWord) return true;
  }
  return false;
}

void markOffered(const LanguageEntry& language) {
  if (wasOffered(language)) return;
  if (!APP_STATE.hyphenationPacksOffered.empty()) APP_STATE.hyphenationPacksOffered += ' ';
  APP_STATE.hyphenationPacksOffered += language.primaryTag;
  APP_STATE.saveToFile();
}

std::string displayName(const LanguageEntry& language) { return language.nativeName; }

}  // namespace hyphpacks
