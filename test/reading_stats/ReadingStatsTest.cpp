// Covers what neither the emulator nor the device can show without months of real history:
// streak boundaries, session counting, per-language independence, and the save/load round trip.
#include <HalStorage.h>  // the stub: provides testRoot()
#include <I18n.h>
#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "BookStats.h"
#include "ReadingStatsStore.h"

namespace {

// The stores write through the HalStorage stub. Each test gets its own root: ctest runs them
// as parallel processes sharing a working directory, so a fixed root races.
class StatsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    testRoot() = std::string("sdroot_") + info->name();
    std::filesystem::remove_all(testRoot());
    ASSERT_TRUE(std::filesystem::create_directories(testRoot() + "/system"));
    READING_STATS_STORE = ReadingStatsStore{};
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(testRoot(), ec);  // best effort; a leftover dir fails no test
  }
};

// 2026-08-08 is the anchor for every fixture below.
constexpr uint16_t Y = 2026;
constexpr uint8_t M = 8;

}  // namespace

TEST_F(StatsTest, StreakCountsConsecutiveDaysEndingToday) {
  auto& s = READING_STATS_STORE;
  for (uint8_t d = 5; d <= 8; d++) s.addMinutes(Y, M, d, 10);
  EXPECT_EQ(s.getStreak(Y, M, 8), 4);
  EXPECT_EQ(s.getLongestStreak(), 4);
  EXPECT_EQ(s.getDaysRead(), 4);
  EXPECT_EQ(s.getTotalMinutes(), 40u);
}

TEST_F(StatsTest, GapBreaksCurrentStreakButNotLongest) {
  auto& s = READING_STATS_STORE;
  for (uint8_t d = 1; d <= 6; d++) s.addMinutes(Y, M, d, 10);
  s.addMinutes(Y, M, 8, 10);  // nothing on the 7th
  EXPECT_EQ(s.getStreak(Y, M, 8), 1);
  EXPECT_EQ(s.getLongestStreak(), 6);
}

TEST_F(StatsTest, NotReadTodayMeansNoCurrentStreak) {
  auto& s = READING_STATS_STORE;
  for (uint8_t d = 1; d <= 7; d++) s.addMinutes(Y, M, d, 10);
  EXPECT_EQ(s.getStreak(Y, M, 8), 0);
  EXPECT_EQ(s.getLongestStreak(), 7);
}

TEST_F(StatsTest, StreakCrossesMonthYearAndLeapDay) {
  auto& s = READING_STATS_STORE;
  s.addMinutes(2024, 2, 28, 10);
  s.addMinutes(2024, 2, 29, 10);
  s.addMinutes(2024, 3, 1, 10);
  EXPECT_EQ(s.getStreak(2024, 3, 1), 3);

  READING_STATS_STORE = ReadingStatsStore{};
  s.addMinutes(2025, 12, 31, 10);
  s.addMinutes(2026, 1, 1, 10);
  EXPECT_EQ(s.getStreak(2026, 1, 1), 2);
}

TEST_F(StatsTest, SameDayAccumulatesRatherThanDuplicating) {
  auto& s = READING_STATS_STORE;
  s.addMinutes(Y, M, 8, 10);
  s.addMinutes(Y, M, 8, 5);
  EXPECT_EQ(s.getDaysRead(), 1);
  EXPECT_EQ(s.getMinutesForDay(Y, M, 8), 15);
}

// Only a backwards clock jump produces this, but the streak passes assume sorted order.
TEST_F(StatsTest, OutOfOrderDaysStillSortCorrectly) {
  auto& s = READING_STATS_STORE;
  s.addMinutes(Y, M, 8, 10);
  s.addMinutes(Y, M, 6, 10);
  s.addMinutes(Y, M, 7, 10);
  EXPECT_EQ(s.getStreak(Y, M, 8), 3);
  EXPECT_EQ(s.getLongestStreak(), 3);
}

TEST_F(StatsTest, EmptyStoreClaimsNothing) {
  auto& s = READING_STATS_STORE;
  EXPECT_EQ(s.getStreak(Y, M, 8), 0);
  EXPECT_EQ(s.getLongestStreak(), 0);
  EXPECT_EQ(s.getDaysRead(), 0);
}

// A week of saturated days exceeds a uint16; the sum must clamp, not wrap to a small number.
TEST_F(StatsTest, WeekMinutesSaturateInsteadOfWrapping) {
  auto& s = READING_STATS_STORE;
  for (uint8_t d = 3; d <= 8; d++) s.addMinutes(Y, M, d, UINT16_MAX);
  EXPECT_EQ(s.getMinutesThisWeek(Y, M, 8), UINT16_MAX);
}

// The history used to be a 365-entry ring, which silently forgot the year before last.
TEST_F(StatsTest, HistoryBeyond365DaysSurvivesAndRoundTrips) {
  auto& s = READING_STATS_STORE;
  constexpr int total = 500;
  for (int i = total - 1; i >= 0; i--) {
    int y = Y, m = M, d = 8 - i;
    while (d < 1) {
      if (--m < 1) {
        m = 12;
        y--;
      }
      static const int dm[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
      d += (m == 2 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0))) ? 29 : dm[m];
    }
    s.addMinutes(static_cast<uint16_t>(y), static_cast<uint8_t>(m), static_cast<uint8_t>(d), 10);
  }
  EXPECT_EQ(s.getDaysRead(), total);
  EXPECT_EQ(s.getStreak(Y, M, 8), total);

  ASSERT_TRUE(s.saveToFile());
  READING_STATS_STORE = ReadingStatsStore{};
  ASSERT_TRUE(s.loadFromFile());
  EXPECT_EQ(s.getDaysRead(), total);
  EXPECT_EQ(s.getStreak(Y, M, 8), total);
  EXPECT_EQ(s.getMinutesForDay(2025, 8, 8), 10);  // well outside the old window
}

TEST_F(StatsTest, LanguagesAreListedMostReadFirst) {
  auto& s = READING_STATS_STORE;
  for (uint8_t d = 5; d <= 8; d++) s.addLanguageMinutes("ja", 20, Y, M, d);
  s.addLanguageMinutes("en", 10, Y, M, 6);
  s.addLanguageMinutes("en", 10, Y, M, 8);
  s.addLanguageMinutes("", 7, Y, M, 8);  // untagged books get their own bucket

  std::vector<ReadingStatsStore::LanguageSummary> langs;
  s.getLanguages(langs);
  ASSERT_EQ(langs.size(), 3u);
  EXPECT_STREQ(langs[0].code, "ja");
  EXPECT_EQ(langs[0].minutes, 80u);
  EXPECT_STREQ(langs[1].code, "en");
  EXPECT_EQ(langs[2].code[0], '\0');
}

TEST_F(StatsTest, PerLanguageStreaksAreIndependent) {
  auto& s = READING_STATS_STORE;
  for (uint8_t d = 5; d <= 8; d++) s.addLanguageMinutes("ja", 20, Y, M, d);
  s.addLanguageMinutes("en", 10, Y, M, 6);
  s.addLanguageMinutes("en", 10, Y, M, 8);

  EXPECT_EQ(s.getStreak("ja", Y, M, 8), 4);
  EXPECT_EQ(s.getStreak("en", Y, M, 8), 1);
  EXPECT_EQ(s.getLongestStreak("ja"), 4);
  EXPECT_EQ(s.getDaysRead("ja"), 4);
  EXPECT_EQ(s.getDaysRead("en"), 2);

  bool st[32];
  s.getMonthStatus("en", Y, M, st);
  EXPECT_TRUE(st[6] && st[8]);
  EXPECT_FALSE(st[5] || st[7]);  // ja-only days stay clear
}

// Region subtags and country-code aliases must not open a second bucket for one language.
TEST_F(StatsTest, LanguageTagsNormaliseToOneBucket) {
  auto& s = READING_STATS_STORE;
  s.addLanguageMinutes("ja", 10, Y, M, 8);
  s.addLanguageMinutes("ja-JP", 5, Y, M, 8);
  s.addLanguageMinutes("JP", 5, Y, M, 8);

  std::vector<ReadingStatsStore::LanguageSummary> langs;
  s.getLanguages(langs);
  EXPECT_EQ(langs.size(), 1u);
  EXPECT_EQ(s.getTotalMinutes("ja"), 20u);
}

TEST_F(StatsTest, LanguageHistoryBeyond512EntriesSurvives) {
  auto& s = READING_STATS_STORE;
  for (int i = 0; i < 700; i++) {
    s.addLanguageMinutes("ja", 5, Y, 1, 1);  // one day, accumulating
    s.addLanguageMinutes("en", 5, static_cast<uint16_t>(2020 + i / 300), 1, static_cast<uint8_t>((i % 28) + 1));
  }
  EXPECT_EQ(s.getTotalMinutes("ja"), 3500u);
  ASSERT_TRUE(s.saveToFile());
  ASSERT_TRUE(s.loadFromFile());
  EXPECT_EQ(s.getTotalMinutes("ja"), 3500u);
}

// The tabs feed this whatever the store holds; a region subtag must not lose the name.
TEST_F(StatsTest, LanguageNameIgnoresRegionSubtagAndCase) {
  EXPECT_STRNE(I18n::languageNameForCode("ja"), nullptr);
  EXPECT_STREQ(I18n::languageNameForCode("ja-JP"), I18n::languageNameForCode("ja"));
  EXPECT_STREQ(I18n::languageNameForCode("JA"), I18n::languageNameForCode("ja"));
  EXPECT_STREQ(I18n::languageNameForCode("pt_BR"), I18n::languageNameForCode("pt"));
  // A language with no shipped UI must report a miss, not fall back to English.
  EXPECT_EQ(I18n::languageNameForCode("zh"), nullptr);
  EXPECT_EQ(I18n::languageNameForCode(""), nullptr);
}

// ---- BookStats ----

TEST_F(StatsTest, BookWithNoHistoryLoadsCleanly) {
  BookStats b;
  EXPECT_TRUE(b.load("/Japanese/test.epub"));
  EXPECT_EQ(b.getSessions(), 0u);
  EXPECT_EQ(b.getDaysRead(), 0);
}

TEST_F(StatsTest, SessionsCountOpeningsNotHeartbeats) {
  const char* path = "/Japanese/test.epub";
  ASSERT_TRUE(BookStats::recordOpen(path));

  BookStats b;
  ASSERT_TRUE(b.load(path));
  b.recordMinutes(Y, M, 8, 5);
  b.recordMinutes(Y, M, 8, 5);
  b.recordMinutes(Y, M, 8, 5);
  ASSERT_TRUE(b.save());

  ASSERT_TRUE(b.load(path));
  EXPECT_EQ(b.getSessions(), 1u) << "flush heartbeats must not count as sessions";
  EXPECT_EQ(b.getTotalMinutes(), 15u);
  EXPECT_EQ(b.getDaysRead(), 1);

  ASSERT_TRUE(BookStats::recordOpen(path));
  ASSERT_TRUE(b.load(path));
  EXPECT_EQ(b.getSessions(), 2u) << "reopening the same day counts again";
  EXPECT_EQ(b.getDaysRead(), 1);
}

// An open too short to bank a minute is still a session; it stays in the average's divisor.
TEST_F(StatsTest, ZeroMinuteOpenStillCounts) {
  const char* path = "/Japanese/avg.epub";
  ASSERT_TRUE(BookStats::recordOpen(path));
  BookStats b;
  ASSERT_TRUE(b.load(path));
  b.recordMinutes(Y, M, 8, 15);
  ASSERT_TRUE(b.save());

  ASSERT_TRUE(BookStats::recordOpen(path));
  ASSERT_TRUE(BookStats::recordOpen(path));
  ASSERT_TRUE(b.load(path));
  EXPECT_EQ(b.getSessions(), 3u);
  EXPECT_EQ(b.getAverageSessionMinutes(), 5u);
}

TEST_F(StatsTest, BookRejectsUnsetClockAndSeparatesBooks) {
  BookStats b;
  ASSERT_TRUE(b.load("/Japanese/a.epub"));
  b.recordMinutes(Y, M, 8, 10);
  b.recordMinutes(1970, 1, 1, 30);
  EXPECT_EQ(b.getDaysRead(), 1) << "days recorded before the clock was set are misdated";

  BookStats other;
  ASSERT_TRUE(other.load("/Japanese/b.epub"));
  EXPECT_EQ(other.getSessions(), 0u);
  EXPECT_EQ(other.getTotalMinutes(), 0u);
}

// An older layout must reset rather than error, or the screen stays blank with no way back.
TEST_F(StatsTest, OldFormatFileIsTreatedAsNoHistory) {
  const char* path = "/Japanese/ver.epub";
  ASSERT_TRUE(BookStats::recordOpen(path));

  const std::string f = testRoot() + BookStats::filePathFor(path);
  FILE* fp = fopen(f.c_str(), "r+b");
  ASSERT_NE(fp, nullptr);
  ASSERT_EQ(fseek(fp, 4, SEEK_SET), 0);
  const unsigned char v1 = 1;
  ASSERT_EQ(fwrite(&v1, 1, 1, fp), 1u);
  fclose(fp);

  BookStats b;
  EXPECT_TRUE(b.load(path));
  EXPECT_EQ(b.getSessions(), 0u);
}

// save() trims by keeping the tail, so the order must hold even after a backwards clock jump.
TEST_F(StatsTest, BookDaysStaySortedWhenRecordedOutOfOrder) {
  const char* path = "/Japanese/order.epub";
  BookStats b;
  ASSERT_TRUE(b.load(path));
  b.recordMinutes(Y, M, 8, 10);
  b.recordMinutes(Y, M, 3, 10);  // clock jumped back
  b.recordMinutes(Y, M, 5, 10);
  ASSERT_TRUE(b.save());
  ASSERT_TRUE(b.load(path));

  const auto& days = b.getDays();
  ASSERT_EQ(days.size(), 3u);
  EXPECT_EQ(days[0].day, 3);
  EXPECT_EQ(days[1].day, 5);
  EXPECT_EQ(days[2].day, 8);
  EXPECT_EQ(b.getTotalMinutes(), 30u);
}

TEST_F(StatsTest, MonthViewMarksOnlyDaysRead) {
  BookStats b;
  ASSERT_TRUE(b.load("/Japanese/cal.epub"));
  b.recordMinutes(Y, M, 8, 10);
  b.recordMinutes(Y, M, 9, 20);

  bool st[32];
  b.getMonthStatus(Y, M, st);
  EXPECT_TRUE(st[8] && st[9]);
  EXPECT_FALSE(st[7] || st[10]);
  EXPECT_EQ(b.getDaysReadInMonth(Y, M), 2);
  EXPECT_EQ(b.getDaysReadInMonth(Y, 7), 0);
}
