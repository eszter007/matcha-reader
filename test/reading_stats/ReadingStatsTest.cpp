// Covers what neither the emulator nor the device can show without months of real history:
// streak boundaries, session counting, per-language independence, and the save/load round trip.
#include <HalStorage.h>  // the stub: provides testRoot()
#include <I18n.h>
#include <SdSystemDir.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "BookStats.h"
#include "BookStatsDownload.h"
#include "ReadingStatsStore.h"
#include "util/DateFormat.h"

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

// The week runs Monday to today. 2026-10-04 is a Sunday, so all seven days count; the weekday
// was once computed a day late, which made Sunday a Monday and the week a single day.
TEST_F(StatsTest, WeekMinutesCoverMondayThroughToday) {
  auto& s = READING_STATS_STORE;
  s.addMinutes(2026, 9, 27, 100);  // the Sunday before: last week
  for (uint8_t d = 28; d <= 30; d++) s.addMinutes(2026, 9, d, 10);
  for (uint8_t d = 1; d <= 4; d++) s.addMinutes(2026, 10, d, 10);
  EXPECT_EQ(s.getMinutesThisWeek(2026, 10, 4), 70);
  EXPECT_EQ(s.getMinutesThisWeek(2026, 9, 28), 10);  // a Monday: itself only
  s.addLanguageMinutes("zh", 5, 2026, 9, 28);
  s.addLanguageMinutes("zh", 5, 2026, 10, 4);
  EXPECT_EQ(s.getMinutesThisWeek("zh", 2026, 10, 4), 10);
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

// A file longer than the in-memory cap must keep its NEWEST entries and leave the file
// position correct, so the blocks after the day records still parse.
TEST_F(StatsTest, OverlongHistoryKeepsTheRecentTailAndStaysAligned) {
  auto& s = READING_STATS_STORE;
  constexpr size_t over = 4200;  // > MAX_DAYS (3650)
  for (size_t i = 0; i < over; i++) {
    // Spread across years so every date is distinct; the exact dates do not matter.
    const auto y = static_cast<uint16_t>(2020 + i / 360);
    const auto m = static_cast<uint8_t>((i % 12) + 1);
    const auto d = static_cast<uint8_t>((i % 28) + 1);
    s.addMinutes(y, m, d, 1);
  }
  s.markBookFinished("/after/the/day/block.epub");
  s.addLanguageMinutes("ja", 7, 2026, 8, 8);
  ASSERT_TRUE(s.saveToFile());

  READING_STATS_STORE = ReadingStatsStore{};
  ASSERT_TRUE(s.loadFromFile());
  EXPECT_LE(s.getDaysRead(), 3650) << "must not hold more than the memory cap";
  EXPECT_GT(s.getDaysRead(), 0);
  // The blocks written after the day records must still be readable, which only holds if the
  // discarded records were consumed rather than skipped by clamping the count.
  EXPECT_EQ(s.getBooksFinished(), 1);
  EXPECT_EQ(s.getTotalMinutes("ja"), 7u);
}

TEST_F(StatsTest, BookHistoryLongerThanTheCapKeepsTheNewest) {
  const char* path = "/Japanese/long.epub";
  BookStats b;
  ASSERT_TRUE(b.load(path));
  constexpr size_t over = 2100;  // > BookStats::MAX_DAYS (2000)
  for (size_t i = 0; i < over; i++) {
    const auto y = static_cast<uint16_t>(2020 + i / 360);
    const auto m = static_cast<uint8_t>((i % 12) + 1);
    const auto d = static_cast<uint8_t>((i % 28) + 1);
    b.recordMinutes(y, m, d, 1);
  }
  ASSERT_TRUE(b.save());
  ASSERT_TRUE(b.load(path));

  const auto& days = b.getDays();
  EXPECT_LE(days.size(), BookStats::MAX_DAYS);
  ASSERT_FALSE(days.empty());
  // Sorted, so the kept tail must end at the latest date recorded.
  EXPECT_EQ(days.back().year, 2025);
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
  EXPECT_EQ(I18n::languageNameForCode("xx"), nullptr);
  // Chinese has two script UIs but one stats bucket: a shared endonym rather than a miss.
  EXPECT_STREQ(I18n::languageNameForCode("zh-Hant"), I18n::languageNameForCode("zh"));
  EXPECT_STRNE(I18n::languageNameForCode("zh"), nullptr);
  EXPECT_STRNE(I18n::languageNameForCode("yue"), nullptr);
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

TEST_F(StatsTest, DictionaryCountersPersistAlongsideHistory) {
  const char* path = "/Japanese/count.epub";
  ASSERT_TRUE(BookStats::recordOpen(path));
  ASSERT_TRUE(BookStats::addCounts(path, 12, 0));
  ASSERT_TRUE(BookStats::addCounts(path, 0, 1));
  ASSERT_TRUE(BookStats::addCounts(path, 3, 2));
  EXPECT_FALSE(BookStats::addCounts(path, 0, 0)) << "nothing to add, nothing written";

  BookStats b;
  ASSERT_TRUE(b.load(path));
  EXPECT_EQ(b.getLookups(), 15u);
  EXPECT_EQ(b.getSentencesSaved(), 3u);
  EXPECT_EQ(b.getSessions(), 1u) << "adding counts must not disturb the sessions";

  b.recordMinutes(Y, M, 8, 5);
  ASSERT_TRUE(b.save());
  ASSERT_TRUE(b.load(path));
  EXPECT_EQ(b.getLookups(), 15u) << "a minutes save keeps the counters";
  EXPECT_EQ(b.getTotalMinutes(), 5u);
}

// v2 files predate the counters: they keep their history and start the counters at zero.
TEST_F(StatsTest, Version2FileLoadsWithZeroCounters) {
  const char* path = "/Japanese/v2.epub";
  const std::string file = testRoot() + BookStats::filePathFor(path);
  const std::string stored(path);
  std::string bytes = "BKST";
  bytes += static_cast<char>(2);
  const uint32_t sessions = 4, dayCount = 1;
  const auto pathLen = static_cast<uint16_t>(stored.size());
  bytes.append(reinterpret_cast<const char*>(&sessions), 4);
  bytes.append(reinterpret_cast<const char*>(&dayCount), 4);
  bytes.append(reinterpret_cast<const char*>(&pathLen), 2);
  bytes += stored;
  const uint16_t year = Y, minutes = 20;
  bytes.append(reinterpret_cast<const char*>(&year), 2);
  bytes += static_cast<char>(M);
  bytes += static_cast<char>(8);
  bytes.append(reinterpret_cast<const char*>(&minutes), 2);

  BookStats::recordOpen("/Japanese/mkdir.epub");  // creates the stats folder
  FILE* fp = fopen(file.c_str(), "wb");
  ASSERT_NE(fp, nullptr);
  ASSERT_EQ(fwrite(bytes.data(), 1, bytes.size(), fp), bytes.size());
  fclose(fp);

  BookStats b;
  ASSERT_TRUE(b.load(path));
  EXPECT_EQ(b.getSessions(), 4u);
  EXPECT_EQ(b.getTotalMinutes(), 20u);
  EXPECT_EQ(b.getLookups(), 0u);
  EXPECT_EQ(b.getSentencesSaved(), 0u);
}

// The stamp tells how long the read took, from its first reading day to the finish, and a reopening
// after the finish day adds nothing to it.
TEST_F(StatsTest, ReadUntilTheFinish) {
  BookStats b;
  ASSERT_TRUE(b.load("/Japanese/span.epub"));
  uint32_t minutes = 1;
  int days = 1;
  b.readUntil(2026, 10, 24, minutes, days);
  EXPECT_EQ(minutes, 0u);
  EXPECT_EQ(days, 0);
  b.recordMinutes(2026, 10, 20, 300);
  b.recordMinutes(2026, 10, 22, 400);
  b.recordMinutes(2026, 10, 24, 635);
  b.recordMinutes(2026, 10, 27, 3);  // reopened after finishing
  b.readUntil(2026, 10, 24, minutes, days);
  EXPECT_EQ(minutes, 1335u);
  EXPECT_EQ(days, 5) << "the 20th to the 24th";
  b.readUntil(2027, 3, 2, minutes, days);
  EXPECT_EQ(days, 134) << "across a year end and a non-leap February";
}

// What the reader records goes to the journal; a load replays it, and compact() folds it into the
// stats file. Either way the result must equal applying the same changes in memory.
TEST_F(StatsTest, JournalReplaysAndCompactsLikeInMemoryUpdates) {
  auto& s = READING_STATS_STORE;
  s.addMinutes(Y, M, 1, 10);
  s.addBookMinutes("/a.epub", "ja", 10, Y, M, 1);
  s.addLanguageMinutes("ja", 10, Y, M, 1);
  ASSERT_TRUE(s.saveToFile());

  std::vector<ReadingStatsStore::Change> changes(4);
  changes[0] = {Y, M, 3, 5, "/b.epub", "en", nullptr};
  changes[1] = {Y, M, 2, 7, "/c.epub", "ja-JP", nullptr};  // an earlier day than the last
  changes[2].finishedPath = "/b.epub";
  changes[3] = {Y, M, 3, 4, "/a.epub", "", "/a.epub"};

  // Reference: the same changes applied in memory.
  ReadingStatsStore ref;
  ASSERT_TRUE(ref.loadFromFile());
  for (const auto& c : changes) {
    if (c.minutes) {
      ref.addMinutes(c.year, c.month, c.day, c.minutes);
      ref.addBookMinutes(c.bookPath, c.language, c.minutes, c.year, c.month, c.day);
      ref.addLanguageMinutes(c.language, c.minutes, c.year, c.month, c.day);
    }
    if (c.finishedPath) ref.markBookFinished(c.finishedPath);
  }

  READING_STATS_STORE = ReadingStatsStore{};
  for (const auto& c : changes) ASSERT_TRUE(s.record(c));
  const auto same = [&ref](const ReadingStatsStore& st) {
    EXPECT_EQ(st.getTotalMinutes(), ref.getTotalMinutes());
    EXPECT_EQ(st.getDaysRead(), ref.getDaysRead());
    EXPECT_EQ(st.getBooksFinished(), ref.getBooksFinished());
    EXPECT_EQ(st.getTotalMinutes("ja"), ref.getTotalMinutes("ja"));
    EXPECT_EQ(st.getTotalMinutes("en"), ref.getTotalMinutes("en"));
    EXPECT_EQ(st.getBooks().size(), ref.getBooks().size());
  };
  ASSERT_TRUE(s.loadFromFile());  // file + journal replay
  same(s);
  s.release();

  ASSERT_TRUE(s.compact());
  EXPECT_FALSE(std::filesystem::exists(testRoot() + sdsystem::path("reading_stats.log")));
  ASSERT_TRUE(s.loadFromFile());  // file alone now
  same(s);
}

// A record cut short by a power loss is ignored; the ones before it still count.
TEST_F(StatsTest, TornJournalTailIsIgnored) {
  auto& s = READING_STATS_STORE;
  ASSERT_TRUE(s.record({Y, M, 5, 12, "/a.epub", "ja", nullptr}));
  {
    std::ofstream log(testRoot() + sdsystem::path("reading_stats.log"), std::ios::binary | std::ios::app);
    const char torn[] = {static_cast<char>(0xA5), 0x07, 0x00};
    log.write(torn, sizeof(torn));
  }
  ASSERT_TRUE(s.loadFromFile());
  EXPECT_EQ(s.getTotalMinutes(), 12u);
}

// An empty stats file (a save cut off before its first byte) is no history, not a reason to stop
// recording.
TEST_F(StatsTest, EmptyStatsFileStillTakesTheJournal) {
  auto& s = READING_STATS_STORE;
  {
    std::ofstream(testRoot() + sdsystem::path("reading_stats.bin"), std::ios::binary);
  }
  ASSERT_TRUE(s.record({Y, M, 5, 9, "/a.epub", "en", nullptr}));
  ASSERT_TRUE(s.compact());
  READING_STATS_STORE = ReadingStatsStore{};
  ASSERT_TRUE(s.loadFromFile());
  EXPECT_EQ(s.getTotalMinutes(), 9u);
}

// ja, then en, then ja again on one day is one ja record, not two.
TEST_F(StatsTest, SameDayLanguageReturnMergesIntoItsRecord) {
  auto& s = READING_STATS_STORE;
  s.addLanguageMinutes("ja", 10, Y, M, 5);
  s.addLanguageMinutes("en", 5, Y, M, 5);
  s.addLanguageMinutes("ja", 3, Y, M, 5);
  EXPECT_EQ(s.getLanguageDays().size(), 2u);
  EXPECT_EQ(s.getMinutesForDay("ja", Y, M, 5), 13);
}

// summarize() streams the file and journal; it must agree with the getters over the loaded tables.
TEST_F(StatsTest, SummaryMatchesTheLoadedTables) {
  auto& s = READING_STATS_STORE;
  for (uint8_t d = 1; d <= 6; d++) s.addMinutes(Y, M, d, 10);
  s.addBookMinutes("/a.epub", "ja", 30, Y, M, 3);
  s.addBookMinutes("/b.epub", "en", 30, Y, M, 6);
  s.addLanguageMinutes("ja", 30, Y, M, 3);
  s.addLanguageMinutes("en", 30, Y, M, 6);
  s.markBookFinished("/a.epub");
  ASSERT_TRUE(s.saveToFile());
  READING_STATS_STORE = ReadingStatsStore{};
  ASSERT_TRUE(s.record({Y, M, 7, 15, "/b.epub", "en", nullptr}));
  ASSERT_TRUE(s.record({Y, M, 8, 20, "/c.epub", "ja-JP", "/c.epub"}));
  ASSERT_TRUE(s.record({Y, M, 8, 0, nullptr, nullptr, "/b.epub"}));

  const ReadingStatsStore::Summary sum = s.summarize(Y, M, 8, "ja");
  ASSERT_TRUE(s.loadFromFile());
  EXPECT_EQ(sum.streak, s.getStreak(Y, M, 8));
  EXPECT_EQ(sum.booksFinished, s.getBooksFinished());
  EXPECT_EQ(sum.languageBooksFinished, s.getBooksFinished("ja"));
  EXPECT_EQ(sum.streak, 8);
  EXPECT_EQ(sum.booksFinished, 3);
  EXPECT_EQ(sum.languageBooksFinished, 2);
}

// A fold whose journal removal never happened (power loss) must not count the journal again, and
// a new journal that starts with the same record must still count.
TEST_F(StatsTest, FoldedJournalIsNotCountedTwice) {
  auto& s = READING_STATS_STORE;
  const std::string log = testRoot() + sdsystem::path("reading_stats.log");
  ASSERT_TRUE(s.record({Y, M, 5, 10, "/a.epub", "ja", nullptr}));
  std::filesystem::copy_file(log, log + ".keep");
  ASSERT_TRUE(s.compact());
  std::filesystem::rename(log + ".keep", log);  // as if the removal was cut off
  ASSERT_TRUE(s.compact());
  ASSERT_TRUE(s.loadFromFile());
  EXPECT_EQ(s.getTotalMinutes(), 10u);
  s.release();

  std::filesystem::remove(log);
  ASSERT_TRUE(s.record({Y, M, 5, 10, "/a.epub", "ja", nullptr}));  // identical record, new journal
  ASSERT_TRUE(s.loadFromFile());
  EXPECT_EQ(s.getTotalMinutes(), 20u);
}

// A save interrupted between removing the old file and renaming the new one leaves only the
// complete temp file; it is adopted, not overwritten.
TEST_F(StatsTest, InterruptedSaveTempIsAdopted) {
  auto& s = READING_STATS_STORE;
  s.addMinutes(Y, M, 1, 30);
  ASSERT_TRUE(s.saveToFile());
  const std::string bin = testRoot() + sdsystem::path("reading_stats.bin");
  std::filesystem::rename(bin, bin + ".tmp");
  READING_STATS_STORE = ReadingStatsStore{};
  ASSERT_TRUE(s.record({Y, M, 2, 5, "/a.epub", "en", nullptr}));
  ASSERT_TRUE(s.compact());
  ASSERT_TRUE(s.loadFromFile());
  EXPECT_EQ(s.getTotalMinutes(), 35u);
}

// A file shorter than its header is no history; recording carries on.
TEST_F(StatsTest, TruncatedHeaderIsNoHistory) {
  auto& s = READING_STATS_STORE;
  {
    std::ofstream bin(testRoot() + sdsystem::path("reading_stats.bin"), std::ios::binary);
    bin.write("\x04\x01", 2);
  }
  ASSERT_TRUE(s.record({Y, M, 5, 9, "/a.epub", "en", nullptr}));
  ASSERT_TRUE(s.compact());
  ASSERT_TRUE(s.loadFromFile());
  EXPECT_EQ(s.getTotalMinutes(), 9u);
}

// A stats file whose bytes are another file's text (cross-linked clusters) is not stats: it is
// neither read as history nor rewritten, and the summary claims nothing from it.
TEST_F(StatsTest, TextFilledStatsFileIsLeftAlone) {
  auto& s = READING_STATS_STORE;
  const std::string text = R"({"openEpubPath":"/a.epub","pe":1})";
  const std::string path = testRoot() + sdsystem::path("reading_stats.bin");
  {
    std::ofstream bin(path, std::ios::binary);
    bin << text;
  }
  ASSERT_TRUE(s.record({Y, M, 5, 9, "/a.epub", "en", "/a.epub"}));
  EXPECT_FALSE(s.compact());
  const auto summary = s.summarize(Y, M, 5, "en");
  EXPECT_LT(summary.booksFinished, 2u) << "header bytes must not be read as a count";
  std::ifstream in(path, std::ios::binary);
  EXPECT_EQ(std::string(std::istreambuf_iterator<char>(in), {}), text) << "the file must not be rewritten";
}

// A journal header cut short by a power loss is started over, so the records after it count.
TEST_F(StatsTest, TornJournalHeaderIsStartedOver) {
  auto& s = READING_STATS_STORE;
  {
    std::ofstream log(testRoot() + sdsystem::path("reading_stats.log"), std::ios::binary);
    log.write("\x5A\x01\x02", 3);
  }
  ASSERT_TRUE(s.record({Y, M, 5, 7, "/a.epub", "en", nullptr}));
  ASSERT_TRUE(s.loadFromFile());
  EXPECT_EQ(s.getTotalMinutes(), 7u);
}

// Server minutes raise a day to their value and never lower it: merging the same download twice,
// or one older than what the device recorded since, changes nothing.
TEST_F(StatsTest, MergeMinutesTakesTheLargerValue) {
  auto& s = READING_STATS_STORE;
  s.addMinutes(Y, M, 5, 20);
  s.mergeMinutes(Y, M, 5, 30);
  s.mergeMinutes(Y, M, 5, 10);
  s.mergeMinutes(Y, M, 3, 15);  // a day the device never saw
  s.mergeMinutes(Y, M, 3, 15);
  EXPECT_EQ(s.getMinutesForDay(Y, M, 5), 30);
  EXPECT_EQ(s.getMinutesForDay(Y, M, 3), 15);
  EXPECT_EQ(s.getTotalMinutes(), 45u);
}

// A saved download is folded in by the next compact(), alongside the journal, and removed.
TEST_F(StatsTest, CompactMergesSavedDownload) {
  auto& s = READING_STATS_STORE;
  s.addMinutes(Y, M, 1, 30);
  ASSERT_TRUE(s.saveToFile());
  READING_STATS_STORE = ReadingStatsStore{};
  ASSERT_TRUE(s.record({Y, M, 2, 5, "/a.epub", "en", nullptr}));
  ASSERT_TRUE(ReadingStatsStore::saveDownload({{Y, M, 1, 20}, {Y, M, 2, 12}, {Y, M, 4, 7}}));
  ASSERT_TRUE(s.compact());
  const std::string download = testRoot() + sdsystem::path("reading_stats_download.bin");
  EXPECT_FALSE(std::filesystem::exists(download));
  ASSERT_TRUE(s.loadFromFile());
  EXPECT_EQ(s.getMinutesForDay(Y, M, 1), 30);  // the device's own larger value stays
  EXPECT_EQ(s.getMinutesForDay(Y, M, 2), 12);  // the journal's 5, raised
  EXPECT_EQ(s.getMinutesForDay(Y, M, 4), 7);
  EXPECT_EQ(s.getTotalMinutes(), 49u);
  s.release();
  ASSERT_TRUE(s.compact());  // nothing left to fold: the journal is not replayed twice
  ASSERT_TRUE(s.loadFromFile());
  EXPECT_EQ(s.getTotalMinutes(), 49u);
}

namespace {
// Writes a per-book download as Refresh Stats would, as the ".new" file the next boot folds in.
void writeBookDownload(
    const std::vector<std::tuple<std::string, bool, uint32_t, const char*, std::vector<BookDay>>>& books,
    const bool increment = false) {
  bookdownload::Writer w;
  ASSERT_TRUE(w.begin(bookdownload::newPath(), increment));
  for (const auto& [name, finished, sessions, language, days] : books) {
    ASSERT_TRUE(w.beginBook(bookdownload::nameHash(name), finished, sessions, language));
    for (const auto& d : days) ASSERT_TRUE(w.addDay(d));
    ASSERT_TRUE(w.endBook());
  }
  ASSERT_TRUE(w.commit());
}
}  // namespace

// Boot folds the download into what the stats keep across books: a language's day is the sum of its
// books, and books finished elsewhere count, without being counted twice once finished here.
TEST_F(StatsTest, CompactFoldsBookDownload) {
  auto& s = READING_STATS_STORE;
  s.addLanguageMinutes("ja", 10, Y, M, 1);
  s.markBookFinished("/books/here.epub");
  ASSERT_TRUE(s.saveToFile());
  READING_STATS_STORE = ReadingStatsStore{};
  writeBookDownload({
      {"a.epub", true, 3, "ja", {{Y, M, 1, 20}, {Y, M, 2, 5}}},
      {"b.epub", false, 1, "ja-JP", {{Y, M, 1, 15}}},
      {"here.epub", true, 2, "en", {}},            // already finished on this card
      {"scan.xtc", false, 1, "", {{Y, M, 2, 8}}},  // no language: the Unknown tab
  });
  ASSERT_TRUE(s.compact());
  EXPECT_FALSE(std::filesystem::exists(testRoot() + bookdownload::newPath()));
  EXPECT_TRUE(std::filesystem::exists(testRoot() + bookdownload::path()));  // kept for BookStats
  ASSERT_TRUE(s.loadFromFile());
  EXPECT_EQ(s.getMinutesForDay("ja", Y, M, 1), 35);  // 20 + 15, above the 10 recorded here
  EXPECT_EQ(s.getMinutesForDay("ja", Y, M, 2), 5);
  EXPECT_EQ(s.getMinutesForDay("", Y, M, 2), 8);
  EXPECT_EQ(s.getBooksFinished(), 2);  // here.epub, and a.epub from elsewhere
  // Finishing a.epub here replaces its stand-in rather than adding to it.
  s.markBookFinished("/books/a.epub");
  EXPECT_EQ(s.getBooksFinished(), 2);
  EXPECT_TRUE(s.isFinished("/books/a.epub"));
}

// A book on the card takes in its server days when loaded, keeping the larger value per day, and its
// first open since marks it finished if the server says so.
TEST_F(StatsTest, BookStatsTakeInDownload) {
  BookStats::recordOpen("/books/a.epub");  // one local session, no days yet
  {
    BookStats b;
    ASSERT_TRUE(b.load("/books/a.epub"));
    b.recordMinutes(Y, M, 1, 30);
    ASSERT_TRUE(b.save());
  }
  writeBookDownload({{"a.epub", true, 4, "ja", {{Y, M, 1, 20}, {Y, M, 2, 12}}}});
  ASSERT_TRUE(READING_STATS_STORE.compact());

  BookStats plain;
  ASSERT_TRUE(plain.load("/books/a.epub"));  // a save's load leaves the server copy alone
  EXPECT_EQ(plain.getTotalMinutes(), 30u);
  BookStats b;
  ASSERT_TRUE(b.load("/books/a.epub", /*withServerCopy=*/true));
  EXPECT_EQ(b.getSessions(), 4u);
  EXPECT_EQ(b.getTotalMinutes(), 42u);  // 30 kept on the 1st, 12 added on the 2nd
  BookStats other;
  ASSERT_TRUE(other.load("/books/unknown.epub", /*withServerCopy=*/true));
  EXPECT_EQ(other.getTotalMinutes(), 0u);

  ASSERT_TRUE(BookStats::recordOpen("/books/a.epub"));
  ASSERT_TRUE(READING_STATS_STORE.compact());
  ASSERT_TRUE(READING_STATS_STORE.loadFromFile());
  EXPECT_TRUE(READING_STATS_STORE.isFinished("/books/a.epub"));
  EXPECT_EQ(READING_STATS_STORE.getBooksFinished(), 1);  // its stand-in went
}

// A download of only the changed books replaces those books in the list the card has; the language
// tabs are rebuilt from the whole list, so an unchanged book's minutes still count.
TEST_F(StatsTest, IncrementalBookDownloadCombines) {
  auto& s = READING_STATS_STORE;
  writeBookDownload({
      {"a.epub", false, 1, "ja", {{Y, M, 1, 20}}},
      {"b.epub", false, 1, "ja", {{Y, M, 1, 10}}},
  });
  ASSERT_TRUE(s.compact());
  writeBookDownload({{"a.epub", true, 2, "ja", {{Y, M, 1, 25}, {Y, M, 2, 5}}}}, /*increment=*/true);
  ASSERT_TRUE(s.compact());
  EXPECT_FALSE(std::filesystem::exists(testRoot() + bookdownload::newPath()));
  ASSERT_TRUE(s.loadFromFile());
  EXPECT_EQ(s.getMinutesForDay("ja", Y, M, 1), 35);  // a's new 25 plus b's 10, kept from the old list
  EXPECT_EQ(s.getMinutesForDay("ja", Y, M, 2), 5);
  EXPECT_EQ(s.getBooksFinished(), 1);
  s.release();

  bookdownload::Book book;
  std::vector<BookDay> days;
  ASSERT_TRUE(bookdownload::find(bookdownload::nameHash("a.epub"), book, days));
  EXPECT_EQ(book.sessions, 2u);
  EXPECT_EQ(days.size(), 2u);
  EXPECT_TRUE(bookdownload::find(bookdownload::nameHash("b.epub"), book, days));
}

// The end-of-book screen records the finish, then asks whether it was a reopening of a finished book.
TEST_F(StatsTest, SummaryTellsAReopenedFinishedBook) {
  auto& s = READING_STATS_STORE;
  EXPECT_FALSE(s.summarize(Y, M, 5, "en", "/a.epub").alreadyFinished);  // asked before the first finish
  ASSERT_TRUE(s.record({Y, M, 5, 9, "/a.epub", "en", "/a.epub"}));
  EXPECT_TRUE(s.summarize(Y, M, 6, "en", "/a.epub").alreadyFinished);  // in the journal

  ASSERT_TRUE(s.compact());
  EXPECT_TRUE(s.summarize(Y, M, 7, "en", "/a.epub").alreadyFinished);  // in the file

  const auto b = s.summarize(Y, M, 7, "en", "/b.epub");
  EXPECT_FALSE(b.alreadyFinished);
  EXPECT_EQ(b.booksFinished, 1u);
}

// The stamp's lookups and sentences stop at the finish: looking words up in a finished book later adds
// to the book's stats, not to its stamp.
TEST_F(StatsTest, FinishCountsStayAtTheFinish) {
  const char* path = "/Japanese/counted.epub";
  ASSERT_TRUE(BookStats::addMinutes(path, Y, M, 6, 30));
  ASSERT_TRUE(BookStats::addCounts(path, 40, 3));
  BookStats b;
  ASSERT_TRUE(BookStats::recordFinish(path, Y, M, 7, true, b));
  ASSERT_TRUE(BookStats::addCounts(path, 5, 1));  // reopened later
  ASSERT_TRUE(BookStats::recordFinish(path, Y, M, 9, false, b));
  EXPECT_EQ(b.getLookups(), 45u);
  EXPECT_EQ(b.getLookupsAtFinish(), 40u);
  EXPECT_EQ(b.getSentencesAtFinish(), 3u);
}

// The 読了 stamp keeps the first finish day: later visits, flushes and counters must not move it.
TEST_F(StatsTest, FinishDayIsKeptAcrossReopens) {
  const char* path = "/Japanese/done.epub";
  ASSERT_TRUE(BookStats::addMinutes(path, Y, M, 6, 30));
  BookStats b;
  uint16_t y = 0;
  uint8_t m = 0, d = 0;
  ASSERT_TRUE(BookStats::recordFinish(path, Y, M, 7, true, b));
  ASSERT_TRUE(b.getFinishDay(y, m, d));
  EXPECT_EQ(d, 7);

  // Reopened the next day, after reading more and a dictionary lookup.
  ASSERT_TRUE(BookStats::addMinutes(path, Y, M, 8, 5));
  ASSERT_TRUE(BookStats::addCounts(path, 1, 0));
  ASSERT_TRUE(BookStats::recordOpen(path));
  ASSERT_TRUE(BookStats::recordFinish(path, Y, M, 8, false, b));
  ASSERT_TRUE(b.getFinishDay(y, m, d));
  EXPECT_EQ(d, 7);
  EXPECT_EQ(b.getTotalMinutes(), 35u);
  EXPECT_EQ(b.getLookups(), 1u);
}

// A book finished before dates were kept shows its last reading day before today, and nothing is stored.
TEST_F(StatsTest, BookFinishedEarlierShowsItsLastReadingDayUnsaved) {
  const char* path = "/Japanese/old.epub";
  ASSERT_TRUE(BookStats::addMinutes(path, Y, M, 3, 20));
  ASSERT_TRUE(BookStats::addMinutes(path, Y, M, 8, 5));  // read a little on reopening, before the end
  BookStats b;
  uint16_t y = 0;
  uint8_t m = 0, d = 0;
  ASSERT_TRUE(BookStats::recordFinish(path, Y, M, 8, false, b));
  ASSERT_TRUE(b.getFinishDay(y, m, d));
  EXPECT_EQ(d, 3);

  ASSERT_TRUE(b.load(path));
  EXPECT_FALSE(b.getFinishDay(y, m, d)) << "a guessed day must not be saved";
}

// ---- Date display ----

TEST(DateFormat, PatternsPadAndCarryText) {
  char out[32];
  formatDate(out, sizeof(out), "DD.MM.YYYY", 2026, 3, 7);
  EXPECT_STREQ(out, "07.03.2026");
  formatDate(out, sizeof(out), "YYYY年M月D日", 2026, 3, 7);
  EXPECT_STREQ(out, "2026年3月7日");
  formatDate(out, sizeof(out), "N YYYY", 2026, 3, 1, "March");
  EXPECT_STREQ(out, "March 2026");
  formatDate(out, sizeof(out), "YYYY. N", 2026, 3, 1, "Március");
  EXPECT_STREQ(out, "2026. március") << "a month name mid-phrase is lowercased";
  formatDate(out, 6, "DD.MM.YYYY", 2026, 3, 7);
  EXPECT_STREQ(out, "07.03") << "truncates to fit";
}

// Every language's patterns use only known tokens and fit the stamp's 24-byte buffers.
TEST(DateFormat, EveryLanguageHasUsablePatterns) {
  auto& i18n = I18n::getInstance();
  for (uint8_t l = 0; l < static_cast<uint8_t>(Language::_COUNT); l++) {
    ASSERT_TRUE(i18n.setLanguage(static_cast<Language>(l)));
    char date[64], heading[64];
    formatDate(date, sizeof(date), tr(STR_DATE_FORMAT), 2026, 12, 31);
    formatDate(heading, sizeof(heading), tr(STR_MONTH_YEAR_FORMAT), 2026, 12, 1, tr(STR_MONTH_DEC));
    const char* code = i18n.getLanguageName(static_cast<Language>(l));
    EXPECT_NE(strstr(date, "2026"), nullptr) << code << ": " << date;
    EXPECT_NE(strstr(date, "31"), nullptr) << code << ": " << date;
    EXPECT_NE(strstr(date, "12"), nullptr) << code << ": " << date;
    EXPECT_EQ(strpbrk(date, "YMDN"), nullptr) << code << " left a token: " << date;
    EXPECT_LT(strlen(date), 24u) << code << ": " << date;
    // The stamp's dates sit inside its ring: CJK languages use the dotted hanko form there.
    char stamp[24];
    formatDate(stamp, sizeof(stamp), tr(STR_STAMP_DATE_FORMAT), 2026, 12, 31);
    EXPECT_EQ(strpbrk(stamp, "YMDN"), nullptr) << code << " left a token: " << stamp;
    EXPECT_LE(strlen(stamp), 14u) << code << ": " << stamp << " is too wide for the stamp";
    EXPECT_NE(strstr(heading, tr(STR_MONTH_DEC) + 1), nullptr)
        << code << ": " << heading;  // case of the first letter may change
    EXPECT_NE(strstr(heading, "2026"), nullptr) << code << ": " << heading;
  }
  i18n.setLanguage(Language::EN);
}

namespace {
// A digest as the server's overview would give it, answered on 2026-08-07 (day 9715 since 2000).
StatsDigest digest(const char* code, const std::vector<int>& readBack, const uint32_t readingDays,
                   const uint16_t longest, const uint16_t finished, const uint32_t minutes, const uint32_t week) {
  StatsDigest g{};
  strncpy(g.code, code, sizeof(g.code) - 1);
  g.anchorDay = 9715;
  g.days = 730;
  for (const int n : readBack) g.history[n >> 3] |= 1 << (n & 7);
  g.readingDays = readingDays;
  g.longestStreak = longest;
  g.booksFinished = finished;
  g.minutes = minutes;
  g.weekMinutes = week;
  return g;
}
}  // namespace

// With the account loaded, a day read on any device counts, and this reader's minutes after the
// server answered are added to its totals; without it, nothing changes.
TEST_F(StatsTest, AccountOverviewCountsEveryDevice) {
  auto& s = READING_STATS_STORE;
  // This reader: Friday 7 and Saturday 8 August, both Japanese.
  s.addMinutes(Y, M, 7, 10);
  s.addMinutes(Y, M, 8, 5);
  s.addLanguageMinutes("ja", 10, Y, M, 7);
  s.addLanguageMinutes("ja", 5, Y, M, 8);
  ASSERT_TRUE(s.saveToFile());
  READING_STATS_STORE = ReadingStatsStore{};
  // The account, the day before: read on the 7th, 6th and 4th (bits 0, 1, 3); German only on the server.
  ASSERT_TRUE(
      ReadingStatsStore::saveAccount({digest("", {0, 1, 3}, 40, 9, 3, 600, 100),
                                      digest("ja", {0, 1}, 10, 2, 1, 300, 50), digest("de", {3}, 5, 1, 0, 900, 0)}));
  {
    const ReadingStatsStore::Loaded stats(true);
    ASSERT_TRUE(stats.loaded());
    EXPECT_EQ(s.getStreak(Y, M, 8), 3);  // 8th here, 7th both, 6th elsewhere
    EXPECT_EQ(s.getStreak(Y, M, 9), 3);  // the 9th not read yet: the streak runs to yesterday
    EXPECT_EQ(s.getDaysRead(), 41);      // the 8th is new to the server
    EXPECT_EQ(s.getTotalMinutes(), 605u);
    EXPECT_EQ(s.getMinutesThisWeek(Y, M, 8), 105);
    EXPECT_EQ(s.getLongestStreak(), 9);
    EXPECT_EQ(s.getBooksFinished(), 3);
    bool week[7];
    s.getWeekStatus(Y, M, 8, 5, week);
    const bool expected[7] = {false, true, false, true, true, true, false};
    EXPECT_TRUE(std::equal(week, week + 7, expected));
    EXPECT_EQ(s.getDaysReadInMonth(Y, M), 4);

    EXPECT_EQ(s.getStreak("ja", Y, M, 8), 3);
    EXPECT_EQ(s.getDaysRead("ja"), 11);
    EXPECT_EQ(s.getTotalMinutes("ja"), 305u);
    EXPECT_EQ(s.getMinutesThisWeek("ja", Y, M, 8), 55);
    std::vector<ReadingStatsStore::LanguageSummary> languages;
    s.getLanguages(languages);
    ASSERT_EQ(languages.size(), 2u);
    EXPECT_STREQ(languages[0].code, "de");  // read elsewhere, and most
    EXPECT_STREQ(languages[1].code, "ja");
  }
  const ReadingStatsStore::Loaded own;
  ASSERT_TRUE(own.loaded());
  EXPECT_EQ(s.getTotalMinutes(), 15u);
  EXPECT_EQ(s.getStreak(Y, M, 8), 2);
}

// A reopened finished book keeps the stamp number it got when it was finished, both while the finishes
// are still in the journal and once they are in the file: two Japanese books, an English one, a third
// Japanese one.
TEST_F(StatsTest, SummaryKeepsAReopenedBooksPlace) {
  auto& s = READING_STATS_STORE;
  ASSERT_TRUE(s.record({Y, M, 5, 9, "/a.epub", "ja", "/a.epub"}));
  ASSERT_TRUE(s.record({Y, M, 5, 9, "/b.epub", "ja", "/b.epub"}));
  ASSERT_TRUE(s.record({Y, M, 6, 9, "/e.epub", "en", "/e.epub"}));
  ASSERT_TRUE(s.record({Y, M, 7, 9, "/c.epub", "ja", "/c.epub"}));
  for (int pass = 0; pass < 2; pass++) {
    const auto newest = s.summarize(Y, M, 7, "ja", "/c.epub");
    EXPECT_EQ(newest.booksFinished, 4);
    EXPECT_EQ(newest.languageBooksFinished, 3);
    ASSERT_TRUE(s.record({Y, M, 8, 0, nullptr, nullptr, "/b.epub"}));  // reopened at its end
    const auto second = s.summarize(Y, M, 8, "ja", "/b.epub");
    EXPECT_EQ(second.booksFinished, 2);
    EXPECT_EQ(second.languageBooksFinished, 2);
    const auto first = s.summarize(Y, M, 8, "ja", "/a.epub");
    EXPECT_EQ(first.booksFinished, 1);
    EXPECT_EQ(first.languageBooksFinished, 1);
    ASSERT_TRUE(s.compact());  // the second pass reads the same from the file
  }
}

// A book finished on another device and brought in by a refresh counts as finished when this card
// reaches its end: no first finish is recorded, and it keeps the place its stand-in has.
TEST_F(StatsTest, SummaryKnowsABookFinishedElsewhere) {
  auto& s = READING_STATS_STORE;
  ASSERT_TRUE(s.record({Y, M, 1, 9, "/here.epub", "ja", "/here.epub"}));
  ASSERT_TRUE(s.compact());
  writeBookDownload({{"elsewhere.epub", true, 2, "ja", {{Y, M, 2, 20}}}});
  ASSERT_TRUE(s.compact());
  ASSERT_TRUE(s.record({Y, M, 3, 9, "/later.epub", "ja", "/later.epub"}));
  const auto sum = s.summarize(Y, M, 4, "ja", "/books/elsewhere.epub");
  EXPECT_TRUE(sum.listRead);
  EXPECT_TRUE(sum.alreadyFinished);
  EXPECT_EQ(sum.booksFinished, 2);
  EXPECT_EQ(sum.languageBooksFinished, 2);
}

// A book finished without any reading time still counts in its language: its finish carries it.
TEST_F(StatsTest, FinishWithoutReadingCountsInItsLanguage) {
  auto& s = READING_STATS_STORE;
  ASSERT_TRUE(s.record({Y, M, 1, 1, "/a.epub", "ja", "/a.epub"}));
  ReadingStatsStore::Change finishOnly{Y, M, 2, 0, "/b.epub", "ja", "/b.epub"};
  ASSERT_TRUE(s.record(finishOnly));
  for (int pass = 0; pass < 2; pass++) {
    const auto sum = s.summarize(Y, M, 2, "ja", "/b.epub");
    EXPECT_EQ(sum.booksFinished, 2);
    EXPECT_EQ(sum.languageBooksFinished, 2);
    ASSERT_TRUE(s.compact());  // the second pass reads the same from the file
  }
  ASSERT_TRUE(s.loadFromFile());
  EXPECT_EQ(s.getBooksFinished("ja"), 2);
  s.release();
}
