#pragma once

#include <string>

// Delete a file, or a directory and everything inside it. Hidden and dot-
// prefixed entries are removed like any other -- macOS leaves `.Spotlight-V100`
// and `.Trashes` on the card, and refusing to delete them is what left them
// stranded. Callers still decide what is off-limits before calling.
//
// The reading cache of every book file removed is cleared during the same walk,
// so the tree is traversed once rather than twice. Returns false at the first
// failure; whatever was already deleted stays deleted.
bool deletePathRecursive(const std::string& path);
