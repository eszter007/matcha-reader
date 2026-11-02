#pragma once

#include <functional>
#include <string>

namespace bookwalk {

// Calls `fn(bookPath)` for every file at or under `path`, and for every manga folder, which is a book and
// is not descended into. Dot folders (the caches) are skipped. Stops when `fn` returns false; returns false
// then. Nothing is accumulated, so a card of a thousand books costs no list of paths.
bool forEachBook(const std::string& path, int depth, const std::function<bool(const std::string&)>& fn);

}  // namespace bookwalk
