#include <gtest/gtest.h>

#include "RecentPages.h"

namespace {

RecentPages::Place place(uint16_t spine, uint16_t page, uint16_t total = 20) {
  RecentPages::Place p;
  p.spineIndex = spine;
  p.page = page;
  p.totalPages = total;
  return p;
}

// Reads page by page from (spine, first) to (spine, last), one turn a minute.
unsigned long readPages(RecentPages& recent, uint16_t spine, uint16_t first, uint16_t last, unsigned long nowMs) {
  for (uint16_t page = first; page <= last; page++) {
    recent.note(place(spine, page), nowMs);
    nowMs += 60000;
  }
  return nowMs;
}

TEST(RecentPagesTest, PageTurnsMoveTheNewestPlace) {
  RecentPages recent;
  readPages(recent, 3, 0, 10, 0);
  ASSERT_EQ(recent.count(), 1);
  EXPECT_EQ(recent.at(0).page, 10);
}

TEST(RecentPagesTest, TurningIntoTheNextChapterIsNotAJump) {
  RecentPages recent;
  unsigned long now = readPages(recent, 3, 17, 19, 0);
  recent.note(place(4, 0), now);
  ASSERT_EQ(recent.count(), 1);
  EXPECT_EQ(recent.at(0).spineIndex, 4);

  // And back again.
  recent.note(place(3, 19), now + 60000);
  EXPECT_EQ(recent.count(), 1);
}

TEST(RecentPagesTest, AJumpKeepsThePlaceLeft) {
  RecentPages recent;
  unsigned long now = readPages(recent, 3, 0, 10, 0);
  recent.note(place(8, 0), now);
  ASSERT_EQ(recent.count(), 2);
  EXPECT_EQ(recent.at(0).spineIndex, 8);
  EXPECT_EQ(recent.at(1).spineIndex, 3);
  EXPECT_EQ(recent.at(1).page, 10);
}

TEST(RecentPagesTest, PlacesOnlyPassedThroughAreReplaced) {
  RecentPages recent;
  unsigned long now = readPages(recent, 3, 0, 10, 0);
  // Scrubbing: three quick jumps without turning a page.
  recent.note(place(8, 0), now);
  recent.note(place(12, 5), now + 2000);
  recent.note(place(15, 3), now + 4000);
  ASSERT_EQ(recent.count(), 2);
  EXPECT_EQ(recent.at(0).spineIndex, 15);
  EXPECT_EQ(recent.at(1).spineIndex, 3);
}

TEST(RecentPagesTest, APlaceStayedOnIsKept) {
  RecentPages recent;
  unsigned long now = readPages(recent, 3, 0, 10, 0);
  recent.note(place(8, 0), now);
  recent.note(place(12, 5), now + RecentPages::PASS_THROUGH_MS);
  EXPECT_EQ(recent.count(), 3);
}

TEST(RecentPagesTest, APlaceWithAPageTurnIsKept) {
  RecentPages recent;
  unsigned long now = readPages(recent, 3, 0, 10, 0);
  recent.note(place(8, 0), now);
  recent.note(place(8, 1), now + 1000);
  recent.note(place(12, 5), now + 2000);
  EXPECT_EQ(recent.count(), 3);
}

TEST(RecentPagesTest, ReturningToAPlaceMovesItToTheFront) {
  RecentPages recent;
  unsigned long now = readPages(recent, 3, 0, 10, 0);
  now = readPages(recent, 8, 0, 4, now);
  recent.note(place(3, 11), now);
  ASSERT_EQ(recent.count(), 2);
  EXPECT_EQ(recent.at(0).spineIndex, 3);
  EXPECT_EQ(recent.at(0).page, 11);
  EXPECT_EQ(recent.at(1).spineIndex, 8);
}

TEST(RecentPagesTest, KeepsTheNewestSix) {
  RecentPages recent;
  unsigned long now = 0;
  for (uint16_t spine = 0; spine < 10; spine++) now = readPages(recent, spine * 2, 0, 3, now);
  ASSERT_EQ(recent.count(), RecentPages::MAX_PLACES);
  EXPECT_EQ(recent.at(0).spineIndex, 18);
  EXPECT_EQ(recent.at(RecentPages::MAX_PLACES - 1).spineIndex, 8);
}

TEST(RecentPagesTest, ASaveWithoutAnOffsetKeepsThePagesOffset) {
  RecentPages recent;
  RecentPages::Place withOffset = place(3, 4);
  withOffset.hasOffset = true;
  withOffset.offset = 1234;
  recent.note(withOffset, 0);
  recent.note(place(3, 4), 1000);
  ASSERT_EQ(recent.count(), 1);
  EXPECT_TRUE(recent.at(0).hasOffset);
  EXPECT_EQ(recent.at(0).offset, 1234u);
}

TEST(RecentPagesTest, SerializesAndLoadsBack) {
  RecentPages recent;
  RecentPages::Place first = place(3, 10, 40);
  first.bookPage = 396;
  first.percent = 42;
  first.hasOffset = true;
  first.offset = 0x01020304;
  first.year = 2026;
  first.month = 11;
  first.day = 6;
  first.hour = 12;
  first.minute = 32;
  recent.note(first, 0);
  recent.note(place(9, 2), 60000);

  uint8_t data[RecentPages::MAX_SERIALIZED_SIZE];
  const size_t size = recent.serialize(data, sizeof(data));
  ASSERT_EQ(size, 2 + 2 * RecentPages::PLACE_BYTES);

  RecentPages loaded;
  ASSERT_TRUE(loaded.deserialize(data, size, 20));
  ASSERT_EQ(loaded.count(), 2);
  const RecentPages::Place& p = loaded.at(1);
  EXPECT_EQ(p.spineIndex, 3);
  EXPECT_EQ(p.page, 10);
  EXPECT_EQ(p.totalPages, 40);
  EXPECT_EQ(p.bookPage, 396);
  EXPECT_EQ(p.percent, 42);
  EXPECT_TRUE(p.hasOffset);
  EXPECT_EQ(p.offset, 0x01020304u);
  EXPECT_EQ(p.year, 2026);
  EXPECT_EQ(p.month, 11);
  EXPECT_EQ(p.day, 6);
  EXPECT_EQ(p.hour, 12);
  EXPECT_EQ(p.minute, 32);

  // A loaded newest place counts as read: a quick jump does not replace it.
  loaded.note(place(15, 0), 0);
  EXPECT_EQ(loaded.count(), 3);
}

TEST(RecentPagesTest, RejectsBadFiles) {
  RecentPages recent;
  recent.note(place(3, 10), 0);
  uint8_t data[RecentPages::MAX_SERIALIZED_SIZE];
  const size_t size = recent.serialize(data, sizeof(data));

  RecentPages loaded;
  EXPECT_FALSE(loaded.deserialize(data, size - 1, 20));  // truncated
  EXPECT_FALSE(loaded.deserialize(data, size, 3));       // spine past the book's end
  data[0] = 99;
  EXPECT_FALSE(loaded.deserialize(data, size, 20));  // unknown version
  EXPECT_EQ(loaded.count(), 0);
  EXPECT_EQ(RecentPages().serialize(data, sizeof(data)), 0u);  // nothing to save
}

TEST(RecentPagesTest, DayNumbers) {
  EXPECT_EQ(RecentPages::dayNumber(1970, 1, 1), 0);
  EXPECT_EQ(RecentPages::dayNumber(2026, 1, 1) - RecentPages::dayNumber(2025, 12, 31), 1);
  EXPECT_EQ(RecentPages::dayNumber(2024, 3, 1) - RecentPages::dayNumber(2024, 2, 28), 2);  // leap year
}

}  // namespace
