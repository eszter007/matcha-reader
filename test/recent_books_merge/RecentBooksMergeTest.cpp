#include <gtest/gtest.h>

#include "src/RecentBook.h"

TEST(RecentBooksMerge, KeepsRecentsFirstWithoutDuplicatesOrLosingScannedCovers) {
  std::vector<RecentBook> catalog{{"/a.epub", "Scanned A", "", "cover-a"},
                                  {"/b.epub", "Scanned B", "", "cover-b"}};
  std::vector<RecentBook> recents{{"/b.epub", "Recent B", "Author B", ""},
                                  {"/new.epub", "New", "", "cover-new"}};

  mergeRecentBooks(catalog, std::move(recents));

  ASSERT_EQ(catalog.size(), 3u);
  EXPECT_EQ(catalog[0].path, "/b.epub");
  EXPECT_EQ(catalog[0].title, "Recent B");
  EXPECT_EQ(catalog[0].author, "Author B");
  EXPECT_EQ(catalog[0].coverBmpPath, "cover-b");
  EXPECT_EQ(catalog[1].path, "/new.epub");
  EXPECT_EQ(catalog[2].path, "/a.epub");
}
