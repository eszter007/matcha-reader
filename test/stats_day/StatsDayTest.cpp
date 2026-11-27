#include <gtest/gtest.h>

#include "util/StatsDay.h"

TEST(StatsDayParseTime, AcceptsValid24HourTimes) {
  EXPECT_EQ(statsday::parseTime("00:00"), 0);
  EXPECT_EQ(statsday::parseTime("04:04"), 4 * 60 + 4);
  EXPECT_EQ(statsday::parseTime("4:30"), 4 * 60 + 30);
  EXPECT_EQ(statsday::parseTime("23:59"), 23 * 60 + 59);
  EXPECT_EQ(statsday::parseTime(" 12:00 "), 12 * 60);
}

TEST(StatsDayParseTime, RejectsInvalidTimes) {
  for (const char* text : {"24:00", "12:60", "7", "7:5", "007:00", "12:00 pm", "ab:cd", ":30", "12:3a", "", "12.30"}) {
    EXPECT_EQ(statsday::parseTime(text), -1) << text;
  }
  EXPECT_EQ(statsday::parseTime(nullptr), -1);
}
