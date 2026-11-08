#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "BookStats.h"

// Counts the distinct headwords shown in one lookup session, so browsing back and forth counts
// each word once. flush() adds the total to the book's stats in one write.
class LookupCounter {
 public:
  void count(const std::string& headword) {
    if (headword.empty() || hashes.size() >= MAX_COUNTED) return;
    const auto hash = static_cast<uint32_t>(std::hash<std::string>{}(headword));
    if (std::find(hashes.begin(), hashes.end(), hash) != hashes.end()) return;
    if (hashes.empty()) hashes.reserve(32);
    hashes.push_back(hash);
  }

  void flush(const std::string& bookPath) {
    BookStats::addCounts(bookPath.c_str(), static_cast<uint32_t>(hashes.size()), 0);
    hashes.clear();
  }

 private:
  static constexpr size_t MAX_COUNTED = 256;
  std::vector<uint32_t> hashes;
};
