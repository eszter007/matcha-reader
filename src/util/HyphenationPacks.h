#pragma once

#include <string>

struct LanguageEntry;

// Screen-side view of the downloadable hyphenation packs (lib/Epub/Epub/hyphenation/HyphenationPack.h).
namespace hyphpacks {

// The pack a book in `bookLanguage` needs: its rules are not built in. Null otherwise.
const LanguageEntry* packFor(const std::string& bookLanguage);

bool isInstalled(const LanguageEntry& language);

// The book-open offer is made once per language.
bool wasOffered(const LanguageEntry& language);
void markOffered(const LanguageEntry& language);

// The language in its own name ("Deutsch").
std::string displayName(const LanguageEntry& language);

}  // namespace hyphpacks
