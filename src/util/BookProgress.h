#pragma once

#include <string>

// Saved reading percentage, or -1 when unavailable; 100 for a book finished elsewhere (finishedIndex).
// Never builds reading caches.
int loadBookProgress(const std::string& path);

// Cache directory a book's progress and layout files live in, whatever its type, or empty for a
// path that is not a book. Same paths the readers use, derived without loading the book.
std::string bookCachePath(const std::string& path);

// Mark a book finished / not started. "Read" writes a progress record every reader and both
// grids agree is 100%, or, when the book's cache cannot place its end (never opened, or laid out by
// an older firmware), puts it in the finished index. "Unread" removes the record and the index entry, which is what a
// book that was never opened looks like.
bool markBookRead(const std::string& path);
bool markBookUnread(const std::string& path);

// Manga progress from the two small cache reads it takes (current page, page count), with no
// MangaBook load. Shared so the Home grid and the Library grid read a manga book the same way.
// Returns 0 when the book has not been opened yet, -1 when the path is not a manga folder.
int mangaProgressPercent(const std::string& path);
