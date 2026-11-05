#include <gtest/gtest.h>

#include "src/RecentBook.h"

namespace {
bool allExist(const std::string&) { return true; }
bool onlyScannedExist(const std::string& path) { return path != "/deleted.epub"; }
}  // namespace

TEST(RecentBooksMerge, KeepsRecentsFirstWithoutDuplicatesOrLosingScannedCovers) {
  std::vector<RecentBook> catalog{{"/a.epub", "Scanned A", "", "cover-a"}, {"/b.epub", "Scanned B", "", "cover-b"}};
  std::vector<RecentBook> recents{{"/b.epub", "Recent B", "Author B", ""}, {"/new.epub", "New", "", "cover-new"}};

  mergeRecentBooks(catalog, std::move(recents), allExist);

  ASSERT_EQ(catalog.size(), 3u);
  EXPECT_EQ(catalog[0].path, "/b.epub");
  EXPECT_EQ(catalog[0].title, "Recent B");
  EXPECT_EQ(catalog[0].author, "Author B");
  EXPECT_EQ(catalog[0].coverBmpPath, "cover-b");
  EXPECT_EQ(catalog[1].path, "/new.epub");
  EXPECT_EQ(catalog[2].path, "/a.epub");
}

TEST(RecentBooksMerge, RecentWhoseFileIsGoneIsNotAddedBack) {
  std::vector<RecentBook> catalog{{"/a.epub", "A", "", ""}};
  std::vector<RecentBook> recents{{"/deleted.epub", "Gone", "", ""}, {"/a.epub", "A", "", ""}};

  mergeRecentBooks(catalog, std::move(recents), onlyScannedExist);

  ASSERT_EQ(catalog.size(), 1u);
  EXPECT_EQ(catalog[0].path, "/a.epub");
}
