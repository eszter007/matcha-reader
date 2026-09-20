#include "RenameWithState.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <MangaPanel.h>

#include <functional>

#include "RecentBooksStore.h"
#include "util/BookmarkUtil.h"

namespace renamestate {
namespace {

// Deepest folder nesting this will walk. A card organised into series/volume folders is two or
// three levels; the bound exists so a malformed tree cannot recurse without end on a device whose
// stack is measured in kilobytes.
constexpr int MAX_DEPTH = 8;

// Moves one state path, reporting whether it actually moved so the caller can put it back.
// A missing source is success with moved=false: not every book has a cache or a bookmark.
bool moveState(const std::string& from, const std::string& to, bool& moved, const char* tag) {
  moved = false;
  if (from.empty() || to.empty() || !Storage.exists(from.c_str())) return true;
  if (Storage.exists(to.c_str())) {
    LOG_ERR(tag, "Rename state target already exists: %s", to.c_str());
    return false;
  }
  moved = Storage.rename(from.c_str(), to.c_str());
  if (!moved) LOG_ERR(tag, "Failed to move rename state: %s -> %s", from.c_str(), to.c_str());
  return moved;
}

// Reverse of moveState, used on the rollback pass. Best-effort by construction: it runs when
// something has already gone wrong, and a path that never moved simply is not there.
void moveStateBack(const std::string& from, const std::string& to, const char* tag) {
  if (from.empty() || to.empty() || !Storage.exists(from.c_str())) return;
  if (!Storage.rename(from.c_str(), to.c_str())) {
    LOG_ERR(tag, "Failed to roll back rename state: %s -> %s", from.c_str(), to.c_str());
  }
}

// Calls `fn(bookPath)` for every book at or under `path`. A manga folder IS a book, so it is
// reported and not descended into. Nothing is accumulated: a folder of a thousand books would
// cost tens of KB as a list of paths, on a heap that has ~30KB free when the web server runs.
// The walk is deterministic, so the rollback pass re-derives the same set instead of storing it.
bool forEachBook(const std::string& path, int depth, const std::function<bool(const std::string&)>& fn) {
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

// The path a book under `oldRoot` will have once that root becomes `newRoot`.
std::string rebase(const std::string& bookPath, const std::string& oldRoot, const std::string& newRoot) {
  if (bookPath.size() < oldRoot.size()) return bookPath;
  return newRoot + bookPath.substr(oldRoot.size());
}

}  // namespace

std::string bookCachePath(const std::string& path) {
  const char* prefix = nullptr;
  if (FsHelpers::hasEpubExtension(path)) {
    prefix = "epub_";
  } else if (FsHelpers::hasXtcExtension(path)) {
    prefix = "xtc_";
  } else if (FsHelpers::hasTxtExtension(path) || FsHelpers::hasMarkdownExtension(path)) {
    prefix = "txt_";
  } else if (manga::MangaBook::isMangaFolder(path)) {
    // A manga book is a folder of page images; its index and progress live under this hash the
    // same way an EPUB's do, so a folder rename has to carry them too.
    prefix = "manga_";
  } else {
    return "";
  }
  return std::string("/.crosspoint/") + prefix + std::to_string(std::hash<std::string>{}(path));
}

bool renamePathWithState(const std::string& oldPath, const std::string& newPath, const char* logTag) {
  // Pass 1: move every book's cache and bookmarks to the names the new path implies. Stops at the
  // first failure, and pass 1b puts back everything pass 1 moved -- all-or-nothing, so the card is
  // never left with a book's state filed under a name no book has.
  bool allMoved = true;
  forEachBook(oldPath, 0, [&](const std::string& bookPath) {
    const std::string newBookPath = rebase(bookPath, oldPath, newPath);
    bool cacheMoved = false;
    bool marksMoved = false;
    const std::string oldCache = bookCachePath(bookPath);
    const std::string newCache = bookCachePath(newBookPath);
    if (!moveState(oldCache, newCache, cacheMoved, logTag)) {
      allMoved = false;
      return false;
    }
    const bool isEpub = FsHelpers::hasEpubExtension(bookPath);
    if (!moveState(isEpub ? BookmarkUtil::getBookmarkPath(bookPath) : "",
                   isEpub ? BookmarkUtil::getBookmarkPath(newBookPath) : "", marksMoved, logTag)) {
      moveStateBack(newCache, oldCache, logTag);  // this book's cache, before giving up
      allMoved = false;
      return false;
    }
    return true;
  });

  const auto rollBackState = [&]() {
    forEachBook(oldPath, 0, [&](const std::string& bookPath) {
      const std::string newBookPath = rebase(bookPath, oldPath, newPath);
      moveStateBack(bookCachePath(newBookPath), bookCachePath(bookPath), logTag);
      if (FsHelpers::hasEpubExtension(bookPath)) {
        moveStateBack(BookmarkUtil::getBookmarkPath(newBookPath), BookmarkUtil::getBookmarkPath(bookPath), logTag);
      }
      return true;
    });
  };

  if (!allMoved) {
    rollBackState();
    return false;
  }

  // Pass 2: the rename itself, last -- while it is still undone, the walk above can re-derive
  // every book path from oldPath. Once it succeeds there is nothing left that can fail.
  if (!Storage.rename(oldPath.c_str(), newPath.c_str())) {
    LOG_ERR(logTag, "Failed to rename: %s -> %s", oldPath.c_str(), newPath.c_str());
    rollBackState();
    return false;
  }

  // Pass 3: recents. The walk now has to run over the NEW path, and reports the new book paths,
  // so each entry is mapped back to find what it used to be.
  forEachBook(newPath, 0, [&](const std::string& newBookPath) {
    const std::string oldBookPath = rebase(newBookPath, newPath, oldPath);
    RECENT_BOOKS.updatePath(oldBookPath, newBookPath, bookCachePath(oldBookPath), bookCachePath(newBookPath));
    return true;
  });
  return true;
}

}  // namespace renamestate
