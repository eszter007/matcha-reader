#pragma once

#include <string>

// A book's insights are filed under its path: its history (BookStats), its reading time and its
// finished mark (ReadingStatsStore). A book moved or renamed on the card has to take them along.
namespace bookinsights {

// After `oldPath` was moved or renamed to `newPath` -- a book, or a folder of books -- every book's
// insights follow it. Failures are logged: the move has happened and stands.
void followMove(const std::string& oldPath, const std::string& newPath);

}  // namespace bookinsights
