#pragma once

#include <string>

// Renaming a book moves its reading state with it. Cache directories and bookmark files are named
// from a hash of the book's PATH, so a rename that only touches the card leaves the old state
// orphaned and the book reopens at page one -- silently, which is the worst way to lose a
// bookmark. Renaming a FOLDER changes the path of every book inside it, so each one's state has
// to move too.
//
// Both callers (the on-device file browser and the web file manager) go through here, so the two
// cannot drift: the web path used to rename the file alone and drop the progress the device path
// carefully preserved.
namespace renamestate {

// Moves `oldPath` to `newPath`, carrying the reading state of the book -- or of every book inside
// it, when the path is a folder. A manga folder IS a book and is treated as one rather than
// descended into.
//
// All-or-nothing: if any state move fails, everything already moved is put back and the rename
// itself is not attempted, so the card is never left with a book whose state lives under another
// book's name. Returns false with nothing changed in that case.
//
// `logTag` names the caller in log lines ("FileBrowser", "WEB").
bool renamePathWithState(const std::string& oldPath, const std::string& newPath, const char* logTag);

// The cache directory a book's path maps to, or "" for a path that is not a book. Exposed because
// RecentBooksStore::updatePath() takes the cache paths alongside the book paths.
std::string bookCachePath(const std::string& path);

}  // namespace renamestate
