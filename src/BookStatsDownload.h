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
// The download is written as a ".new" file; the next boot folds its languages and finished books into the
// reading stats (ReadingStatsStore::compact()) and renames it to the file BookStats reads.
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

// Streams books into newPath(): beginBook(), the book's days in date order, endBook(); then commit().
// A failed write leaves the previous files as they are.
class Writer {
 public:
  bool begin();
  bool beginBook(uint32_t nameHash, bool finished, uint32_t sessions, const char* language);
  bool addDay(const BookDay& day);
  bool endBook();
  bool commit();
  void abort();

 private:
  HalFile file;
  size_t countAt = 0;
  uint16_t count = 0;
  bool ok = false;
};

// Calls fn for every book in `file` with its days, read into `days` (reused). False when the file is
// missing or damaged; the books before the damage were still passed to fn.
bool forEach(const std::string& file, std::vector<BookDay>& days, void* ctx,
             void (*fn)(void* ctx, const Book& book, const std::vector<BookDay>& days));

// The book saved under `nameHash` in path(), with its days. False when there is none.
bool find(uint32_t nameHash, Book& book, std::vector<BookDay>& days);

}  // namespace bookdownload
