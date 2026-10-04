#pragma once

#include <string>
#include <vector>

// One StarDict dictionary found under /dictionaries or /.dictionaries: a
// subfolder holding <stem>.idx plus <stem>.dict or <stem>.dict.dz.
struct DictionaryEntry {
  std::string name;  // folder path below /dictionaries (shown to the user, stored in settings)
  std::string stem;  // index basename without .idx
};

namespace DictionaryRegistry {

// Scan /dictionaries/*/ and /.dictionaries/*/ for dictionaries. Folders with
// multiple index stems are ambiguous and skipped. Result is sorted
// case-insensitively by name.
void discover(std::vector<DictionaryEntry>& out);

// Resolve a folder name to its extensionless base path
// ("/dictionaries/<folder>/<stem>" or "/.dictionaries/<folder>/<stem>").
// Returns false if the folder holds no usable dictionary in either root.
bool resolveBasePath(const char* folderName, std::string& basePathOut);

// Every dictionary for the book language, looked up together: `preferred` (the settings pick)
// first when it is one of them, then the rest in name order, at most `max`. A language with none
// gets `preferred` alone, as folderForLanguageOrFallback does. Empty when there is nothing to use.
void foldersForLanguage(const std::string& language, const char* preferred, size_t max,
                        std::vector<std::string>& foldersOut);

}  // namespace DictionaryRegistry
