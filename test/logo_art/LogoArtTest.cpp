#include <gtest/gtest.h>

#include <vector>

#include "activities/boot_sleep/LogoArt.h"

using logo_art::buildFootprint;
using logo_art::fitInBox;
using logo_art::maskBit;
using logo_art::targetRow;

namespace {

// A w x h 2-bit image, every pixel `level`.
std::vector<uint8_t> image(const int w, const int h, const uint8_t level) {
  const uint8_t byte = static_cast<uint8_t>(level << 6 | level << 4 | level << 2 | level);
  return std::vector<uint8_t>((static_cast<size_t>(w) * h + 3) / 4, byte);
}

void setLevel(std::vector<uint8_t>& img, const int w, const int x, const int y, const uint8_t level) {
  const size_t i = static_cast<size_t>(y) * w + x;
  const int shift = 6 - static_cast<int>(i & 3) * 2;
  img[i >> 2] = static_cast<uint8_t>((img[i >> 2] & ~(0x3 << shift)) | (level << shift));
}

struct Masks {
  std::vector<uint8_t> footprint;
  std::vector<uint8_t> scratch;
  explicit Masks(const int w, const int h)
      : footprint((static_cast<size_t>(w) * h + 7) / 8), scratch((static_cast<size_t>(w) * h + 7) / 8) {}
  bool at(const int w, const int x, const int y) const {
    return maskBit(footprint.data(), static_cast<size_t>(y) * w + x);
  }
};

}  // namespace

TEST(LogoArtFit, SmallerThanBoxIsNotEnlarged) {
  const auto fit = fitInBox(100, 50, 222, 316);
  EXPECT_EQ(fit.width, 100);
  EXPECT_EQ(fit.height, 50);
}

TEST(LogoArtFit, WideSourceIsLimitedByWidth) {
  const auto fit = fitInBox(800, 400, 222, 316);
  EXPECT_EQ(fit.width, 222);
  EXPECT_EQ(fit.height, 111);
}

TEST(LogoArtFit, ScreenSizedPortraitSourceIsLimitedByHeight) {
  const auto fit = fitInBox(480, 800, 222, 316);
  EXPECT_EQ(fit.height, 316);
  EXPECT_EQ(fit.width, 189);
}

TEST(LogoArtFit, ExtremeAspectKeepsOnePixel) {
  const auto fit = fitInBox(800, 1, 222, 316);
  EXPECT_EQ(fit.width, 222);
  EXPECT_EQ(fit.height, 1);
}

TEST(LogoArtTargetRow, EveryTargetRowIsFedExactlyOnce) {
  for (const int srcH : {1, 2, 7, 316, 317, 480, 800}) {
    for (const int h : {1, 3, 111, 316}) {
      if (h > srcH) continue;
      std::vector<int> fed(h, 0);
      for (int srcY = 0; srcY < srcH; srcY++) {
        const int ty = targetRow(srcY, srcH, h);
        if (ty < 0) continue;
        ASSERT_LT(ty, h);
        fed[ty]++;
      }
      for (int ty = 0; ty < h; ty++) EXPECT_EQ(fed[ty], 1) << "srcH=" << srcH << " h=" << h << " ty=" << ty;
    }
  }
}

TEST(LogoArtTargetRow, SameSizeMapsEachRowToItself) {
  for (int y = 0; y < 50; y++) EXPECT_EQ(targetRow(y, 50, 50), y);
}

TEST(LogoArtFootprint, WhiteBackgroundIsOutsideAndEnclosedWhiteIsInside) {
  // A black ring on white: the ring and the white it encloses are footprint, the rest is not.
  constexpr int W = 20;
  constexpr int H = 20;
  auto img = image(W, H, 3);
  for (int y = 5; y <= 14; y++) {
    for (int x = 5; x <= 14; x++) {
      if (x == 5 || x == 14 || y == 5 || y == 14) setLevel(img, W, x, y, 0);
    }
  }
  Masks m(W, H);
  EXPECT_TRUE(buildFootprint(img.data(), W, H, 0, m.footprint.data(), m.scratch.data()));
  EXPECT_FALSE(m.at(W, 0, 0));
  EXPECT_FALSE(m.at(W, 4, 10));
  EXPECT_TRUE(m.at(W, 5, 10));   // the ring
  EXPECT_TRUE(m.at(W, 10, 10));  // white enclosed by it
}

TEST(LogoArtFootprint, GrowsByTheRequestedEdge) {
  constexpr int W = 21;
  constexpr int H = 21;
  auto img = image(W, H, 3);
  setLevel(img, W, 10, 10, 0);
  Masks m(W, H);
  ASSERT_TRUE(buildFootprint(img.data(), W, H, 4, m.footprint.data(), m.scratch.data()));
  EXPECT_TRUE(m.at(W, 14, 10));   // 4 px straight out
  EXPECT_FALSE(m.at(W, 15, 10));  // not 5
  EXPECT_FALSE(m.at(W, 14, 14));  // corners are rounded, not square
}

TEST(LogoArtFootprint, GrayCountsAsDrawing) {
  constexpr int W = 9;
  constexpr int H = 9;
  auto img = image(W, H, 3);
  setLevel(img, W, 4, 4, 2);  // light gray must not join the background
  Masks m(W, H);
  ASSERT_TRUE(buildFootprint(img.data(), W, H, 0, m.footprint.data(), m.scratch.data()));
  EXPECT_TRUE(m.at(W, 4, 4));
  EXPECT_FALSE(m.at(W, 3, 4));
}

TEST(LogoArtFootprint, ReportsWhenTheFillHitsItsCap) {
  // A serpentine white corridor between black walls, entered from the border, needs one sweep per
  // turn; with more turns than the cap the fill stops early and says so.
  constexpr int W = 5;
  constexpr int H = 600;
  auto img = image(W, H, 0);
  for (int y = 0; y < H; y++) {
    if (y % 2 == 0) {
      for (int x = 1; x < W - 1; x++) setLevel(img, W, x, y, 3);
    } else {
      setLevel(img, W, (y / 2) % 2 == 0 ? W - 2 : 1, y, 3);
    }
  }
  Masks m(W, H);
  EXPECT_FALSE(buildFootprint(img.data(), W, H, 0, m.footprint.data(), m.scratch.data()));
}
