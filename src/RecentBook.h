#pragma once

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

struct RecentBook {
  std::string path;
  std::string title;
  std::string author;
  std::string coverBmpPath;

  bool operator==(const RecentBook& other) const { return path == other.path; }
};

// Recents contains at most ten entries. Move them to the front of the scanned catalog in place
// instead of allocating another full catalog just to preserve recent-first ordering.
inline void mergeRecentBooks(std::vector<RecentBook>& catalog, std::vector<RecentBook> recents) {
  for (auto recent = recents.rbegin(); recent != recents.rend(); ++recent) {
    const auto scanned =
        std::find_if(catalog.begin(), catalog.end(), [&](const RecentBook& book) { return book.path == recent->path; });
    if (scanned == catalog.end()) {
      catalog.insert(catalog.begin(), std::move(*recent));
      continue;
    }
    if (!recent->title.empty()) scanned->title = std::move(recent->title);
    if (!recent->author.empty()) scanned->author = std::move(recent->author);
    if (!recent->coverBmpPath.empty()) scanned->coverBmpPath = std::move(recent->coverBmpPath);
    std::rotate(catalog.begin(), scanned, scanned + 1);
  }
}
