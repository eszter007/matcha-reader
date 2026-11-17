#include "BookInsights.h"

#include <Logging.h>

#include <utility>
#include <vector>

#include "BookStats.h"
#include "ReadingStatsStore.h"
#include "util/BookWalk.h"

namespace bookinsights {

void followMove(const std::string& oldPath, const std::string& newPath) {
  if (oldPath == newPath) return;
  // The walk runs over the new path, which exists now; each book's old path is rebased from it.
  std::vector<std::pair<std::string, std::string>> moves;
  bookwalk::forEachBook(newPath, 0, [&](const std::string& newBookPath) {
    if (newBookPath.size() < newPath.size()) return true;
    std::string oldBookPath = oldPath + newBookPath.substr(newPath.size());
    if (!BookStats::movePath(oldBookPath.c_str(), newBookPath.c_str())) {
      LOG_ERR("INSIGHT", "history of %s did not move", oldBookPath.c_str());
    }
    moves.emplace_back(std::move(oldBookPath), newBookPath);
    return true;
  });
  if (!ReadingStatsStore::renameBooks(moves)) LOG_ERR("INSIGHT", "reading time of %s did not move", oldPath.c_str());
}

}  // namespace bookinsights
