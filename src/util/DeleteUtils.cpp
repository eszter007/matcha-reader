#include "DeleteUtils.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <cstring>
#include <utility>
#include <vector>

#include "BookCacheUtils.h"
#include "TaskWatchdog.h"

namespace {
// Matches FileBrowserActivity's listing buffer: long UTF-8 names are enabled
// (USE_UTF8_LONG_NAMES), so a name can far exceed the 256-byte stack budget --
// hence the heap allocation rather than a local array.
constexpr size_t NAME_BUFFER_SIZE = 500;
}  // namespace

bool deletePathRecursive(const std::string& path) {
  auto file = Storage.open(path.c_str());
  if (!file) {
    LOG_ERR("DEL", "Failed to open: %s", path.c_str());
    return false;
  }

  if (!file.isDirectory()) {
    file.close();
    clearBookCache(path);
    return Storage.remove(path.c_str());
  }
  file.close();

  const auto nameBuffer = makeUniqueNoThrow<char[]>(NAME_BUFFER_SIZE);
  if (!nameBuffer) {
    LOG_ERR("DEL", "OOM: %d bytes", static_cast<int>(NAME_BUFFER_SIZE));
    return false;
  }

  // Stack of (dirPath, postOrder): postOrder=true means rmdir this path after
  // its children are processed, since rmdir only succeeds on an empty dir.
  std::vector<std::pair<std::string, bool>> stack;
  stack.reserve(16);
  stack.push_back({path, false});

  while (!stack.empty()) {
    // A deep tree (macOS Spotlight indexes hold thousands of entries) can run
    // long enough to trip the watchdog on whichever task is deleting.
    resetTaskWatchdogIfSubscribed();

    auto [currentPath, postOrder] = std::move(stack.back());
    stack.pop_back();

    if (postOrder) {
      if (!Storage.rmdir(currentPath.c_str())) {
        LOG_ERR("DEL", "Failed to rmdir: %s", currentPath.c_str());
        return false;
      }
      continue;
    }

    auto dir = Storage.open(currentPath.c_str());
    if (!dir) {
      LOG_ERR("DEL", "Failed to open dir: %s", currentPath.c_str());
      return false;
    }
    if (!dir.isDirectory()) {
      LOG_ERR("DEL", "Not a directory: %s", currentPath.c_str());
      return false;
    }

    stack.push_back({currentPath, true});

    dir.rewindDirectory();
    for (auto entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
      entry.getName(nameBuffer.get(), NAME_BUFFER_SIZE);
      if (strcmp(nameBuffer.get(), ".") == 0 || strcmp(nameBuffer.get(), "..") == 0) {
        continue;
      }
      std::string entryPath = currentPath;
      if (entryPath.back() != '/') {
        entryPath += "/";
      }
      entryPath += nameBuffer.get();

      const bool isDir = entry.isDirectory();
      entry.close();

      if (isDir) {
        stack.push_back({std::move(entryPath), false});
      } else {
        clearBookCache(entryPath);
        if (!Storage.remove(entryPath.c_str())) {
          LOG_ERR("DEL", "Failed to remove file: %s", entryPath.c_str());
          return false;
        }
      }
    }
  }

  return true;
}
