#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <string>
#include <vector>

#include "BookStats.h"

// This reader's per-book stats as the sync server holds them, saved by Refresh Stats for the books on the
// card to take back when they are next loaded (BookStats::load). Keyed by filename, the one name a book
// keeps across devices; books not on the card are never written anywhere else.
//
// The download is written as a ".new" file, either the whole list or only the books changed since the last
// one. The next boot combines it with the list it has (combine()), folds the result's languages and finished
// books into the reading stats (ReadingStatsStore::compact()) and keeps it as the file BookStats reads.
namespace bookdownload {

constexpr size_t MAX_LANGUAGE = 15;

struct Book {
  uint32_t nameHash = 0;  // nameHash() of the filename
  bool finished = false;
  uint32_t sessions = 0;
  char language[MAX_LANGUAGE + 1] = {};
  uint16_t dayCount = 0;
};

inline uint32_t nameHash(const std::string& pathOrName) { return bookNameHash(pathOrName.data(), pathOrName.size()); }

std::string newPath();  // written by the download, read once at boot
std::string path();     // read by BookStats::load

// Streams books into `file`: beginBook(), the book's days in date order, endBook(); then commit().
// `increment`: only the books changed since the last download. A failed write leaves the previous files.
class Writer {
 public:
  bool begin(const std::string& file, bool increment);
  bool beginBook(uint32_t nameHash, bool finished, uint32_t sessions, const char* language);
  bool addDay(const BookDay& day);
  bool endBook();
  bool commit();
  void abort();

 private:
  HalFile file;
  std::string target;
  size_t countAt = 0;
  uint16_t count = 0;
  bool ok = false;
};

// Called with a context pointer, a book and its days.
using BookFn = void (*)(void*, const Book&, const std::vector<BookDay>&);

// Calls fn for every book in `file` with its days, read into `days` (reused). False when the file is
// missing or damaged; the books before the damage were still passed to fn.
bool forEach(const std::string& file, std::vector<BookDay>& days, void* ctx, BookFn fn);

// The book saved under `nameHash` in path(), with its days. False when there is none.
bool find(uint32_t nameHash, Book& book, std::vector<BookDay>& days);

// The whole list once newPath() is taken in: newPath() itself when it is a whole list, else a temporary
// file of path()'s books with the changed ones replaced. False when it cannot be built now.
bool combine(std::string& combined);
// Keeps `combined` as path() and drops newPath().
bool keep(const std::string& combined);
// Drops a combine() result that is not kept.
void discard(const std::string& combined);

}  // namespace bookdownload
