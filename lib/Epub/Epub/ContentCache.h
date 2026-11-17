#pragma once

#include <string>

// A book's cache folder holds two kinds of files: the reading state (position, per-book settings)
// and what was built from the book file (metadata, chapter layouts, images, covers). When the file is
// replaced by a newer version, only the second kind is dropped.
namespace content_cache {
// Empties the cache folder except the reading state.
bool clear(const std::string& cachePath);
// Run before the cache is used: clears it when the book file no longer matches the one it was built
// from. Does nothing when the cache folder does not exist.
void checkSource(const std::string& bookPath, const std::string& cachePath);
// Records which book file a new cache folder is built from.
void stamp(const std::string& bookPath, const std::string& cachePath);
}  // namespace content_cache
