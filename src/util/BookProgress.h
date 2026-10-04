#pragma once

#include <string>

// Saved reading percentage, or -1 when unavailable. Never builds reading caches.
int loadBookProgress(const std::string& path);

// Cache directory a book's progress and layout files live in, whatever its type, or empty for a
// path that is not a book. Same paths the readers use, derived without loading the book.
std::string bookCachePath(const std::string& path);

// Mark a book finished / not started. "Read" writes a progress record every reader and both
// grids agree is 100%; "unread" removes the record, which is what a book that was never opened
// looks like. Both return false when the book has no cache to write into -- a book that has
// never been opened has no cache directory, and creating one just to say "unread" would be
// inventing state.
bool markBookRead(const std::string& path);
bool markBookUnread(const std::string& path);

// Manga progress from the two small cache reads it takes (current page, page count), with no
// MangaBook load. Shared so the Home grid and the Library grid read a manga book the same way.
// Returns 0 when the book has not been opened yet, -1 when the path is not a manga folder.
int mangaProgressPercent(const std::string& path);
