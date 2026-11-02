#include "BookWalk.h"

#include <HalStorage.h>
#include <MangaPanel.h>

namespace bookwalk {

namespace {
// Deepest folder nesting this will walk. A card organised into series/volume folders is two or
// three levels; the bound exists so a malformed tree cannot recurse without end on a device whose
// stack is measured in kilobytes.
constexpr int MAX_DEPTH = 8;
}  // namespace

bool forEachBook(const std::string& path, const int depth, const std::function<bool(const std::string&)>& fn) {
  if (depth > MAX_DEPTH) return true;
  HalFile entry = Storage.open(path.c_str());
  if (!entry) return true;
  if (!entry.isDirectory()) {
    entry.close();
    return fn(path);
  }
  if (manga::MangaBook::isMangaFolder(path)) {
    entry.close();
    return fn(path);
  }
  bool ok = true;
  for (auto child = entry.openNextFile(); child && ok; child = entry.openNextFile()) {
    char nameBuf[256];
    child.getName(nameBuf, sizeof(nameBuf));
    const bool childIsDir = child.isDirectory();
    child.close();
    if (nameBuf[0] == '\0' || nameBuf[0] == '.') continue;  // skip dotfiles and the cache dir
    std::string childPath = path;
    if (childPath.back() != '/') childPath += '/';
    childPath += nameBuf;
    ok = childIsDir ? forEachBook(childPath, depth + 1, fn) : fn(childPath);
  }
  entry.close();
  return ok;
}

}  // namespace bookwalk
