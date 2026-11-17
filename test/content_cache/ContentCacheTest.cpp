#include <gtest/gtest.h>

#include <string>

#include "ContentCache.h"
#include "HalStorage.h"

namespace {
const std::string BOOK = "/Books/book.epub";
const std::string CACHE = "/.crosspoint/epub_1";

std::string bookBytes(char fill, size_t size = 3000) { return std::string(size, fill); }

// A cache as a reader leaves it: reading state beside what was built from the file.
void addReadCache() {
  fake::addDir(CACHE);
  for (const char* kept : {"progress.bin", "readerprefs.bin", "language.bin", "pace.bin", "synced.bin"}) {
    fake::addFile(CACHE + "/" + kept, "state");
  }
  fake::addFile(CACHE + "/book.bin", "meta");
  fake::addFile(CACHE + "/links.bin", "back stack");
  fake::addFile(CACHE + "/thumb_480.bmp", "thumb");
  fake::addDir(CACHE + "/sections");
  fake::addFile(CACHE + "/sections/0.bin", "layout");
  fake::addDir(CACHE + "/html");
  fake::addFile(CACHE + "/html/0.html", "chapter");
}

void expectStateKept() {
  for (const char* kept : {"progress.bin", "readerprefs.bin", "language.bin", "pace.bin", "synced.bin"}) {
    EXPECT_TRUE(fake::has(CACHE + "/" + kept)) << kept;
  }
}

void expectContentCleared() {
  for (const char* gone :
       {"/book.bin", "/links.bin", "/thumb_480.bmp", "/sections", "/sections/0.bin", "/html", "/html/0.html"}) {
    EXPECT_FALSE(fake::has(CACHE + gone)) << gone;
  }
}

class ContentCacheTest : public ::testing::Test {
 protected:
  void SetUp() override { fake::reset(); }
};
}  // namespace

TEST_F(ContentCacheTest, ClearKeepsReadingStateOnly) {
  addReadCache();
  EXPECT_TRUE(content_cache::clear(CACHE));
  expectStateKept();
  expectContentCleared();
  EXPECT_TRUE(fake::has(CACHE));
}

TEST_F(ContentCacheTest, ClearWithoutCacheIsNothingToDo) { EXPECT_TRUE(content_cache::clear(CACHE)); }

TEST_F(ContentCacheTest, UnchangedBookKeepsItsCache) {
  fake::addFile(BOOK, bookBytes('a'));
  addReadCache();
  content_cache::stamp(BOOK, CACHE);
  content_cache::checkSource(BOOK, CACHE);
  EXPECT_TRUE(fake::has(CACHE + "/book.bin"));
  EXPECT_TRUE(fake::has(CACHE + "/sections/0.bin"));
}

TEST_F(ContentCacheTest, ReplacedBookDropsContentAndKeepsPosition) {
  fake::addFile(BOOK, bookBytes('a'));
  addReadCache();
  content_cache::stamp(BOOK, CACHE);
  fake::addFile(BOOK, bookBytes('b', 3500));  // copied over USB: no upload code ran
  content_cache::checkSource(BOOK, CACHE);
  expectStateKept();
  expectContentCleared();
  // The new file is recorded: the rebuilt cache is not cleared again at the next open.
  fake::addFile(CACHE + "/book.bin", "new meta");
  content_cache::checkSource(BOOK, CACHE);
  EXPECT_TRUE(fake::has(CACHE + "/book.bin"));
}

TEST_F(ContentCacheTest, SameSizeChangeInTheTailIsCaught) {
  std::string bytes = bookBytes('a');
  fake::addFile(BOOK, bytes);
  addReadCache();
  content_cache::stamp(BOOK, CACHE);
  bytes[bytes.size() - 10] = 'z';  // one entry's CRC in the ZIP central directory
  fake::addFile(BOOK, bytes);
  content_cache::checkSource(BOOK, CACHE);
  EXPECT_FALSE(fake::has(CACHE + "/book.bin"));
}

TEST_F(ContentCacheTest, CacheFromBeforeStampsIsTrustedOnce) {
  fake::addFile(BOOK, bookBytes('a'));
  addReadCache();  // no source.bin: built by an older firmware
  content_cache::checkSource(BOOK, CACHE);
  EXPECT_TRUE(fake::has(CACHE + "/book.bin"));
  EXPECT_TRUE(fake::has(CACHE + "/source.bin"));
  fake::addFile(BOOK, bookBytes('b'));
  content_cache::checkSource(BOOK, CACHE);
  EXPECT_FALSE(fake::has(CACHE + "/book.bin"));
}

TEST_F(ContentCacheTest, UnreadableBookChangesNothing) {
  fake::addFile(BOOK, bookBytes('a'));
  addReadCache();
  content_cache::stamp(BOOK, CACHE);
  fake::addFile(BOOK, bookBytes('b'));
  fake::failReads = true;
  content_cache::checkSource(BOOK, CACHE);
  EXPECT_TRUE(fake::has(CACHE + "/book.bin"));
}

TEST_F(ContentCacheTest, NoCacheFolderIsLeftAlone) {
  fake::addFile(BOOK, bookBytes('a'));
  content_cache::checkSource(BOOK, CACHE);
  EXPECT_FALSE(fake::has(CACHE));
  EXPECT_FALSE(fake::has(CACHE + "/source.bin"));
}

TEST_F(ContentCacheTest, SmallBookIsHashedWhole) {
  fake::addFile(BOOK, "tiny");
  addReadCache();
  content_cache::stamp(BOOK, CACHE);
  fake::addFile(BOOK, "tinz");
  content_cache::checkSource(BOOK, CACHE);
  EXPECT_FALSE(fake::has(CACHE + "/book.bin"));
}

TEST_F(ContentCacheTest, TouchedFileIsRebuilt) {
  fake::addFile(BOOK, bookBytes('a'));
  addReadCache();
  content_cache::stamp(BOOK, CACHE);
  fake::nodes[BOOK]->modified = 2;  // same bytes, newer time: a same-length edit looks like this
  content_cache::checkSource(BOOK, CACHE);
  EXPECT_FALSE(fake::has(CACHE + "/book.bin"));
  expectStateKept();
}

TEST_F(ContentCacheTest, FailedClearIsRetriedAtTheNextOpen) {
  fake::addFile(BOOK, bookBytes('a'));
  addReadCache();
  content_cache::stamp(BOOK, CACHE);
  fake::addFile(BOOK, bookBytes('b', 3500));
  fake::failRemove = CACHE + "/sections";
  content_cache::checkSource(BOOK, CACHE);
  EXPECT_TRUE(fake::has(CACHE + "/sections/0.bin"));  // the SD refused once
  EXPECT_TRUE(fake::has(CACHE + "/source.bin"));      // still naming the old file
  fake::failRemove.clear();
  content_cache::checkSource(BOOK, CACHE);
  EXPECT_FALSE(fake::has(CACHE + "/sections"));
}

TEST_F(ContentCacheTest, ClearForAnUploadLeavesTheStampToTheNextOpen) {
  fake::addFile(BOOK, bookBytes('a'));
  addReadCache();
  content_cache::stamp(BOOK, CACHE);
  EXPECT_TRUE(content_cache::clear(CACHE));
  EXPECT_FALSE(fake::has(CACHE + "/source.bin"));
}
