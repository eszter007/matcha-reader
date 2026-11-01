#include "CounterReading.h"

#include <gtest/gtest.h>

#include <string>

namespace {

std::string reading(uint64_t n, const char* counter) {
  std::string out;
  return CounterReading::read(n, counter, out) ? out : "<none>";
}

size_t counterChars(const char* text) {
  const std::string s(text, CounterReading::matchCounter(text));
  size_t chars = 0;
  for (const char c : s) chars += (static_cast<unsigned char>(c) & 0xC0) != 0x80;
  return chars;
}

TEST(CounterReading, Months) {
  EXPECT_EQ(reading(6, "月"), "ろくがつ");
  EXPECT_EQ(reading(4, "月"), "しがつ");
  EXPECT_EQ(reading(7, "月"), "しちがつ");
  EXPECT_EQ(reading(9, "月"), "くがつ");
  EXPECT_EQ(reading(12, "月"), "じゅうにがつ");
  EXPECT_EQ(reading(6, "ヶ月"), "ろっかげつ");
  EXPECT_EQ(reading(2, "か月"), "にかげつ");
}

TEST(CounterReading, Days) {
  EXPECT_EQ(reading(1, "日"), "ついたち・いちにち");
  EXPECT_EQ(reading(6, "日"), "むいか");
  EXPECT_EQ(reading(9, "日"), "ここのか");
  EXPECT_EQ(reading(20, "日"), "はつか");
  EXPECT_EQ(reading(14, "日"), "じゅうよっか");
  EXPECT_EQ(reading(21, "日"), "にじゅういちにち");
  EXPECT_EQ(reading(7, "日間"), "なのかかん");
  EXPECT_EQ(reading(1, "日間"), "いちにちかん");
  EXPECT_EQ(reading(11, "日間"), "じゅういちにちかん");
  EXPECT_EQ(reading(3, "日目"), "みっかめ");
  EXPECT_EQ(reading(1, "日目"), "いちにちめ");
}

TEST(CounterReading, Time) {
  EXPECT_EQ(reading(4, "時"), "よじ");
  EXPECT_EQ(reading(7, "時"), "しちじ");
  EXPECT_EQ(reading(9, "時"), "くじ");
  EXPECT_EQ(reading(4, "時半"), "よじはん");
  EXPECT_EQ(reading(2, "時間"), "にじかん");
  EXPECT_EQ(reading(1, "分"), "いっぷん");
  EXPECT_EQ(reading(3, "分"), "さんぷん");
  EXPECT_EQ(reading(4, "分"), "よんぷん");
  EXPECT_EQ(reading(5, "分"), "ごふん");
  EXPECT_EQ(reading(40, "分"), "よんじゅっぷん");
  EXPECT_EQ(reading(4, "年"), "よねん");
  EXPECT_EQ(reading(1964, "年"), "せんきゅうひゃくろくじゅうよねん");
  EXPECT_EQ(reading(1, "週間"), "いっしゅうかん");
  EXPECT_EQ(reading(8, "世紀"), "はっせいき");
}

TEST(CounterReading, PeopleAndAges) {
  EXPECT_EQ(reading(1, "人"), "ひとり");
  EXPECT_EQ(reading(2, "人"), "ふたり");
  EXPECT_EQ(reading(4, "人"), "よにん");
  EXPECT_EQ(reading(3, "人目"), "さんにんめ");
  EXPECT_EQ(reading(20, "歳"), "はたち");
  EXPECT_EQ(reading(1, "歳"), "いっさい");
  EXPECT_EQ(reading(2, "人前"), "ににんまえ");
}

TEST(CounterReading, SoundChangesOnRoundNumbers) {
  EXPECT_EQ(reading(1, "本"), "いっぽん");
  EXPECT_EQ(reading(3, "本"), "さんぼん");
  EXPECT_EQ(reading(4, "本"), "よんほん");
  EXPECT_EQ(reading(30, "本"), "さんじゅっぽん");
  EXPECT_EQ(reading(100, "本"), "ひゃっぽん");
  EXPECT_EQ(reading(300, "本"), "さんびゃっぽん");
  EXPECT_EQ(reading(1000, "本"), "せんぼん");
  EXPECT_EQ(reading(3000, "本"), "さんぜんぼん");
  EXPECT_EQ(reading(10000, "本"), "いちまんぼん");
  EXPECT_EQ(reading(100, "回"), "ひゃっかい");
  EXPECT_EQ(reading(3, "階"), "さんがい");
  EXPECT_EQ(reading(1, "匹"), "いっぴき");
  EXPECT_EQ(reading(3, "匹"), "さんびき");
  EXPECT_EQ(reading(3, "羽"), "さんば");
  EXPECT_EQ(reading(1, "羽"), "いちわ");
  EXPECT_EQ(reading(6, "羽"), "ろっぱ");
  EXPECT_EQ(reading(3, "杯"), "さんばい");
  EXPECT_EQ(reading(3, "軒"), "さんげん");
  EXPECT_EQ(reading(3, "足"), "さんぞく");
  EXPECT_EQ(reading(3, "泊"), "さんぱく");
  EXPECT_EQ(reading(1, "票"), "いっぴょう");
}

TEST(CounterReading, NativeCounters) {
  EXPECT_EQ(reading(1, "箱"), "ひとはこ");
  EXPECT_EQ(reading(2, "口"), "ふたくち");
  EXPECT_EQ(reading(1, "晩"), "ひとばん");
  EXPECT_EQ(reading(3, "つ"), "みっつ");
  EXPECT_EQ(reading(10, "つ"), "とお");
}

TEST(CounterReading, LargeNumbersAndMultipliers) {
  EXPECT_EQ(reading(300, "円"), "さんびゃくえん");
  EXPECT_EQ(reading(8000, "円"), "はっせんえん");
  EXPECT_EQ(reading(25000, "円"), "にまんごせんえん");
  EXPECT_EQ(reading(4, "円"), "よえん");
  EXPECT_EQ(reading(5, "万円"), "ごまんえん");
  EXPECT_EQ(reading(3, "億円"), "さんおくえん");
  EXPECT_EQ(reading(1, "兆円"), "いっちょうえん");
  EXPECT_EQ(reading(3, "千円"), "さんぜんえん");
  EXPECT_EQ(reading(3, "万"), "さんまん");
  EXPECT_EQ(reading(120000000, "人"), "いちおくにせんまんにん");
}

TEST(CounterReading, Units) {
  EXPECT_EQ(reading(5, "km"), "ごキロメートル");
  EXPECT_EQ(reading(10, "%"), "じゅっパーセント");
  EXPECT_EQ(reading(1, "cm"), "いっセンチメートル");
  EXPECT_EQ(reading(30, "℃"), "さんじゅうど");
  EXPECT_EQ(reading(2, "トン"), "にトン");
  EXPECT_STREQ(CounterReading::unitWord("kg"), "キログラム");
  EXPECT_EQ(CounterReading::unitWord("円"), nullptr);
}

TEST(CounterReading, MatchesTheLongestCounter) {
  EXPECT_EQ(counterChars("日目の朝"), 2u);
  EXPECT_EQ(counterChars("日間続いた"), 2u);
  EXPECT_EQ(counterChars("万円の時計"), 2u);
  EXPECT_EQ(counterChars("kmを走った"), 2u);
  EXPECT_EQ(counterChars("mmの差"), 2u);
  EXPECT_EQ(counterChars("時間半"), 2u);
  EXPECT_EQ(counterChars("万"), 1u);
  EXPECT_EQ(counterChars("の"), 0u);
}

TEST(CounterReading, UnknownCounterOrZero) {
  EXPECT_EQ(reading(3, "体"), "<none>");
  EXPECT_EQ(reading(0, "月"), "<none>");
}

TEST(CounterReading, ParsesAsciiAndFullWidthDigits) {
  uint64_t n = 0;
  EXPECT_TRUE(CounterReading::parseNumber("21", n));
  EXPECT_EQ(n, 21u);
  EXPECT_TRUE(CounterReading::parseNumber("６", n));
  EXPECT_EQ(n, 6u);
  EXPECT_TRUE(CounterReading::parseNumber("１９６４", n));
  EXPECT_EQ(n, 1964u);
  EXPECT_FALSE(CounterReading::parseNumber("", n));
  EXPECT_FALSE(CounterReading::parseNumber("2a", n));
}

}  // namespace
