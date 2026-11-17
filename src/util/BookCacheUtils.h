#pragma once

#include <string>

// Clears the reading cache for a book file if its extension is recognised
// (EPUB, XTC, or TXT). Does nothing for other file types.
void clearBookCache(const std::string& path);

// For a book file just written (uploaded, downloaded, copied): clears what was built from the old
// file but keeps the reading position and per-book settings of EPUB, TXT and Markdown books. XTC
// books are cleared in full, as before.
void clearBookContentCache(const std::string& path);

// Returns true if the directory name matches a book cache entry.
bool isBookCacheDirectoryName(const char* name);

// Delete a local book and its local clipping state after the file is removed.
bool removeBookFile(const std::string& path);
