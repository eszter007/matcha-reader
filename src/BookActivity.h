#pragma once

#include <HalStorage.h>

#include <string>
#include <vector>

#include "BookStats.h"

// Each book's reading history across every device on the sync account, as the server's per-book
// activity (GET /api/v1/stats/activity/books) tells it: when it was started and finished, and every
// day it was read anywhere -- with recorded time, or only a position sync from a reader that sends no
// stats. Saved by Insights sync for the books read on this card, keyed by filename like bookdownload.
// Display only: never folded into BookStats files or uploaded, which would count another device's
// reading as this one's.
namespace bookactivity {

struct Book {
  uint32_t nameHash = 0;  // bookNameHash() of the filename
  BookDay started{};      // year 0: unknown
  BookDay finished{};     // year 0: not finished
};

std::string path();

// Streams books into the file, then commit(). A failed write leaves the previous file.
class Writer {
 public:
  bool begin();
  // days: oldest first; minutes 0 for a day with only a position sync.
  bool addBook(const Book& book, const BookDay* days, size_t count);
  bool commit();
  void abort();

 private:
  HalFile file;
  bool ok = false;
};

// The saved history of the book at `bookPath`; false when there is none.
bool find(const std::string& bookPath, Book& book, std::vector<BookDay>& days);

}  // namespace bookactivity
