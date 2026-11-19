#include <gtest/gtest.h>

#include <string>

#include "src/components/CharCount.h"

namespace {
std::string text(const CharCount& count) {
  char buf[32];
  count.format(buf, sizeof(buf));
  return buf;
}
}  // namespace

TEST(CharCountTest, HiddenUntilAPositionIsKnown) {
  EXPECT_FALSE(CharCount{}.visible());
  EXPECT_TRUE((CharCount{0, -1, false}).visible());
}

TEST(CharCountTest, FormatsReadOverTotal) {
  EXPECT_EQ(text({1240, 8900, false}), "1240/8900");
  EXPECT_EQ(text({45000, 312000, true}), "~45k/312k");
  EXPECT_EQ(text({9999, 10000, false}), "9999/10k");
}

TEST(CharCountTest, PageOffsetIsScaledToCountedCharacters) {
  // A file of 10000 visible codepoints, 8000 of them counted: a page at 5000 has 4000 behind it.
  EXPECT_EQ(CharCount::countedBefore(5000, 8000, 10000), 4000u);
  EXPECT_EQ(CharCount::countedBefore(0, 8000, 10000), 0u);
  EXPECT_EQ(CharCount::countedBefore(12000, 8000, 10000), 8000u);
  EXPECT_EQ(CharCount::countedBefore(5000, 8000, 0), 0u);
}
