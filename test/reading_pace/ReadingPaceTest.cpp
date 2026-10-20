#include <gtest/gtest.h>

#include "ReadingPace.h"

namespace {

TEST(ReadingPace, NoEstimateBeforeFivePages) {
  ReadingPace pace;
  for (int i = 0; i < 4; i++) pace.addPage(60, 10);
  EXPECT_EQ(pace.secondsForProgress(100.0f), -1);
  pace.addPage(60, 10);
  EXPECT_EQ(pace.secondsForProgress(100.0f), 600);
}

TEST(ReadingPace, EstimatesByTextNotPages) {
  // 60 s per 10 bp of text: a chapter of 25 bp left takes 150 s, however many pages it fills.
  ReadingPace pace;
  for (int i = 0; i < 10; i++) pace.addPage(60, 10);
  EXPECT_EQ(pace.secondsForProgress(25.0f), 150);
  EXPECT_EQ(pace.secondsForProgress(0.5f), 3);
}

TEST(ReadingPace, CapsALongPauseAtThreeTimesTheAverage) {
  ReadingPace pace;
  for (int i = 0; i < 5; i++) pace.addPage(60, 10);
  pace.addPage(1500, 10);  // looked something up for 25 minutes
  // Counted as 180 s: 480 s over 60 bp.
  EXPECT_EQ(pace.secondsForProgress(60.0f), 480);
}

}  // namespace
