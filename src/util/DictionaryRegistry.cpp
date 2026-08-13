#include "DictionaryRegistry.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cctype>
#include <cstring>

#include "StringUtils.h"

namespace DictionaryRegistry {
namespace {

// Dictionaries are looked up in both roots, in order. The hidden variant
// lets users keep the folder out of the file browser (hidden by default,
// see FileBrowserActivity's showHiddenFiles check).
constexpr const char* DICT_ROOTS[] = {"/dictionaries", "/.dictionaries"};

// /dictionaries/jp belongs to DictIndex (the Japanese lookup path), not to StarDict: it holds
// vocab, names and grammar .idx+.dat side by side, at the fixed paths DictIndex.h declares.
// Three stems in one folder is exactly what findStem() calls ambiguous, so probing it logged a
// "multiple index stems" skip on every scan -- a correct outcome reported as a fault, for a
// folder that was never a StarDict dictionary. Japanese does not use StarDict at all, so the
// folder itself is never a dictionary this registry should return.
//
// Only the folder ITSELF is DictIndex's. StarDict dictionaries nested inside it
// (/dictionaries/jp/<name>/) are still discovered, and folderForLanguage() resolves "ja" to
// exactly that "jp/" prefix.
bool isDictIndexFolder(const char* folderName) { return strcmp(folderName, "jp") == 0; }

std::string languageFolder(const std::string& language) {
  if (language.size() < 2) return {};
  std::string out = language.substr(0, 2);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
  return out == "ja" ? "jp" : out;
}

// Find the single .idx stem inside one dictionary folder. Returns false when
// the folder holds no .idx or more than one distinct stem (ambiguous).
bool findStem(const char* folderPath, std::string& stemOut) {
  auto dir = Storage.open(folderPath);
  if (!dir || !dir.isDirectory()) return false;

  dir.rewindDirectory();
  char name[128];
  char foundStem[128];
  foundStem[0] = '\0';
  for (auto entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    entry.getName(name, sizeof(name));
    // Skip macOS metadata files (AppleDouble resource forks)
    if (entry.isDirectory() || strncmp(name, "._", 2) == 0) continue;

    const size_t len = strlen(name);
    if (len <= 4 || strcmp(name + len - 4, ".idx") != 0) continue;

    name[len - 4] = '\0';
    if (foundStem[0] != '\0' && strcmp(foundStem, name) != 0) {
      LOG_DBG("DREG", "Skipping %s: multiple index stems found", folderPath);
      return false;
    }
    strncpy(foundStem, name, sizeof(foundStem) - 1);
    foundStem[sizeof(foundStem) - 1] = '\0';
  }

  if (foundStem[0] == '\0') return false;

  // Require dictionary data next to the index, so folders holding only an
  // .idx never surface as selectable dictionaries that fail at lookup time.
  const std::string base = std::string(folderPath) + "/" + foundStem;
  if (!Storage.exists((base + ".dict").c_str()) && !Storage.exists((base + ".dict.dz").c_str())) {
    LOG_DBG("DREG", "Skipping %s: no .dict or .dict.dz", folderPath);
    return false;
  }

  stemOut = foundStem;
  return true;
}

}  // namespace

void discover(std::vector<DictionaryEntry>& out) {
  out.clear();
  out.reserve(8);

  for (const char* dictRoot : DICT_ROOTS) {
    auto rootDir = Storage.open(dictRoot);
    if (!rootDir || !rootDir.isDirectory()) {
      LOG_DBG("DREG", "No %s directory on SD card", dictRoot);
      continue;
    }

    rootDir.rewindDirectory();
    char name[128];
    for (auto entry = rootDir.openNextFile(); entry; entry = rootDir.openNextFile()) {
      entry.getName(name, sizeof(name));
      if (!entry.isDirectory() || name[0] == '.') continue;

      std::string folderPath = std::string(dictRoot) + "/" + name;
      std::string stem;
      // Skipped as a dictionary in its own right, still descended into below for nested ones.
      if (!isDictIndexFolder(name) && findStem(folderPath.c_str(), stem)) {
        out.push_back({name, std::move(stem)});
        continue;
      }
      auto languageDir = Storage.open(folderPath.c_str());
      if (!languageDir || !languageDir.isDirectory()) continue;
      languageDir.rewindDirectory();
      char child[128];
      for (auto nested = languageDir.openNextFile(); nested; nested = languageDir.openNextFile()) {
        nested.getName(child, sizeof(child));
        if (!nested.isDirectory() || child[0] == '.') continue;
        if (findStem((folderPath + "/" + child).c_str(), stem)) out.push_back({std::string(name) + "/" + child, stem});
      }
    }
  }

  // Case-insensitive sort by folder name (matches FileBrowserActivity ordering).
  std::sort(out.begin(), out.end(), [](const DictionaryEntry& a, const DictionaryEntry& b) {
    return StringUtils::asciiCaseCmp(a.name.c_str(), b.name.c_str()) < 0;
  });
}

bool resolveBasePath(const char* folderName, std::string& basePathOut) {
  if (!folderName || folderName[0] == '\0') return false;
  if (folderName[0] == '.' || strpbrk(folderName, "\\") != nullptr || strstr(folderName, "..") != nullptr) return false;
  const char* slash = strchr(folderName, '/');
  if (slash && (slash == folderName || slash[1] == '\0' || strchr(slash + 1, '/'))) return false;
  // Never resolvable as StarDict (see isDictIndexFolder); returning early keeps a stale settings
  // value naming it from re-logging the ambiguous-stem skip on every lookup. Nested "jp/<name>"
  // still resolves -- only the bare folder is DictIndex's.
  if (isDictIndexFolder(folderName)) return false;

  for (const char* dictRoot : DICT_ROOTS) {
    std::string folderPath = std::string(dictRoot) + "/" + folderName;
    std::string stem;
    if (!findStem(folderPath.c_str(), stem)) continue;
    basePathOut = folderPath + "/" + stem;
    return true;
  }
  return false;
}

bool folderForLanguage(const std::string& language, std::string& folderNameOut) {
  const std::string lang = languageFolder(language);
  if (lang.empty()) return false;
  std::vector<DictionaryEntry> entries;
  discover(entries);
  const std::string prefix = lang + "/";
  const auto entry = std::find_if(entries.begin(), entries.end(), [&prefix](const auto& candidate) {
    return candidate.name.compare(0, prefix.size(), prefix) == 0;
  });
  if (entry == entries.end()) return false;
  folderNameOut = entry->name;
  return true;
}

bool folderForLanguageOrFallback(const std::string& language, const char* fallbackFolder, std::string& folderNameOut) {
  if (!language.empty() && folderForLanguage(language, folderNameOut)) return true;
  if (!fallbackFolder || fallbackFolder[0] == '\0') {
    folderNameOut.clear();
    return false;
  }
  folderNameOut = fallbackFolder;
  return true;
}

}  // namespace DictionaryRegistry
