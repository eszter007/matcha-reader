#include "NextBookFinder.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <MangaPanel.h>
#include <Memory.h>

#include <algorithm>
#include <string_view>

#include "CrossPointSettings.h"

namespace {
constexpr size_t NAME_BUFFER_SIZE = 500;

bool isSupportedBookFile(const std::string_view name) {
  // File formats ReaderActivity can open (bmp is a viewer, not a book, so it is excluded)
  return FsHelpers::hasEpubExtension(name) || FsHelpers::hasXtcExtension(name) || FsHelpers::hasTxtExtension(name) ||
         FsHelpers::hasMarkdownExtension(name);
}
}  // namespace

std::vector<std::string> NextBookFinder::findNextBooks(const std::string& currentBookPath, const size_t maxCount) {
  std::vector<std::string> result;
  if (maxCount == 0 || currentBookPath.empty()) {
    return result;
  }

  const std::string folder = FsHelpers::extractFolderPath(currentBookPath);
  const auto lastSlash = currentBookPath.find_last_of('/');
  const std::string currentName =
      lastSlash == std::string::npos ? currentBookPath : currentBookPath.substr(lastSlash + 1);

  auto dir = Storage.open(folder.c_str());
  if (!dir || !dir.isDirectory()) {
    LOG_ERR("NBF", "Cannot open folder: %s", folder.c_str());
    return result;
  }
  dir.rewindDirectory();

  const auto nameBuffer = makeUniqueNoThrow<char[]>(NAME_BUFFER_SIZE);
  if (!nameBuffer) {
    LOG_ERR("NBF", "OOM: %d bytes", static_cast<int>(NAME_BUFFER_SIZE));
    dir.close();
    return result;
  }

  // Heap use is bounded: at most maxCount+1 short filename strings live at once (the
  // file browser holds a whole folder in the same std::string form). A failed
  // allocation here would abort like any STL growth in this codebase; the reserve
  // below makes vector growth a single up-front allocation.
  result.reserve(maxCount + 1);
  const auto less = [](const std::string& a, const std::string& b) { return FsHelpers::naturalLess(a, b); };

  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    file.getName(nameBuffer.get(), NAME_BUFFER_SIZE);
    if (!SETTINGS.showHiddenFiles && nameBuffer[0] == '.') {
      continue;
    }
    const std::string_view nameView{nameBuffer.get()};
    if (file.isDirectory()) {
      const std::string fullPath = folder == "/" ? "/" + std::string{nameView} : folder + "/" + std::string{nameView};
      if (!manga::MangaBook::isMangaFolder(fullPath)) continue;
    } else if (!isSupportedBookFile(nameView)) {
      continue;
    }
    std::string name{nameView};
    // Keep only files ordering strictly after the current one; equal names (the book
    // itself, or a case-variant of it) compare "not less" both ways and drop out here.
    if (!FsHelpers::naturalLess(currentName, name)) {
      continue;
    }
    // Bounded insertion sort: keep the maxCount lowest-ordering candidates
    if (result.size() >= maxCount && !less(name, result.back())) {
      continue;
    }
    const auto pos = std::lower_bound(result.begin(), result.end(), name, less);
    result.insert(pos, std::move(name));
    if (result.size() > maxCount) {
      result.pop_back();
    }
  }
  dir.close();

  return result;
}
