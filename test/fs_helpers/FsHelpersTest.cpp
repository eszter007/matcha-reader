#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "FsHelpers.h"

namespace {

using namespace std::string_view_literals;

TEST(ReflowableBookExtension, IncludesTextFormatsAndExcludesImageBooks) {
  for (const auto path : {"/Books/book.epub"sv, "book.TXT"sv, "book.mD"sv}) {
    EXPECT_TRUE(FsHelpers::hasReflowableBookExtension(path));
  }
  for (const auto path : {"book.xtc"sv, "book.XTCH"sv, "book.bmp"sv, "book.epub.bak"sv, ""sv}) {
    EXPECT_FALSE(FsHelpers::hasReflowableBookExtension(path));
  }
}

TEST(IsSafePathComponent, AcceptsNamesWithRepeatedDots) {
  EXPECT_TRUE(FsHelpers::isSafePathComponent("volume..2.epub"sv));
  EXPECT_TRUE(FsHelpers::isSafePathComponent("notes...txt"sv));
  EXPECT_TRUE(FsHelpers::isSafePathComponent(".hidden"sv));
  EXPECT_TRUE(FsHelpers::isSafePathComponent("a.b"sv));
  EXPECT_TRUE(FsHelpers::isSafePathComponent("book.epub"sv));
}

TEST(IsSafePathComponent, RejectsEmptyAndExactDotComponents) {
  EXPECT_FALSE(FsHelpers::isSafePathComponent(""sv));
  EXPECT_FALSE(FsHelpers::isSafePathComponent("."sv));
  EXPECT_FALSE(FsHelpers::isSafePathComponent(".."sv));
}

TEST(IsSafePathComponent, RejectsPathSeparatorsAnywhereInTheComponent) {
  EXPECT_FALSE(FsHelpers::isSafePathComponent("a/b"sv));
  EXPECT_FALSE(FsHelpers::isSafePathComponent("a\\b"sv));
  EXPECT_FALSE(FsHelpers::isSafePathComponent("../x"sv));
  EXPECT_FALSE(FsHelpers::isSafePathComponent("x/.."sv));
}

TEST(NormalisePath, CollapsesParentReferenceWithinPath) {
  EXPECT_EQ(FsHelpers::normalisePath("/Books/../.crosspoint/x"), ".crosspoint/x");
}

TEST(NormalisePath, DropsLeadingParentReferencesPastRoot) { EXPECT_EQ(FsHelpers::normalisePath("/../../etc"), "etc"); }

TEST(NormalisePath, DropsCurrentDirectoryComponents) {
  EXPECT_EQ(FsHelpers::normalisePath("/."), "");
  EXPECT_EQ(FsHelpers::normalisePath("/./Books/./a.epub"), "Books/a.epub");
}

// Minimal 54-byte BMP header. Callers tweak fields to model the corruptions seen on real cards.
std::vector<uint8_t> bmpHeader(uint32_t fileSize, uint32_t pixelOffset, int32_t width, int32_t height, uint16_t bpp,
                               uint32_t compression = 0) {
  std::vector<uint8_t> h(54, 0);
  const auto put32 = [&h](size_t at, uint32_t v) {
    h[at] = v & 0xFF;
    h[at + 1] = (v >> 8) & 0xFF;
    h[at + 2] = (v >> 16) & 0xFF;
    h[at + 3] = (v >> 24) & 0xFF;
  };
  h[0] = 'B';
  h[1] = 'M';
  put32(2, fileSize);
  put32(10, pixelOffset);
  put32(14, 40);
  h[26] = 1;  // planes
  put32(18, static_cast<uint32_t>(width));
  put32(22, static_cast<uint32_t>(height));
  h[28] = bpp & 0xFF;
  h[29] = (bpp >> 8) & 0xFF;
  put32(30, compression);
  return h;
}

std::string writeTemp(const std::vector<uint8_t>& header, size_t totalBytes) {
  char path[] = "/tmp/fshelpers_bmp_XXXXXX";
  const int fd = mkstemp(path);
  EXPECT_GE(fd, 0);
  FILE* f = fdopen(fd, "wb");
  fwrite(header.data(), 1, header.size(), f);
  for (size_t i = header.size(); i < totalBytes; ++i) fputc(0, f);
  fclose(f);
  return std::string(path);
}

TEST(HasCompleteBmp, AcceptsAWholeBitmap) {
  // 8x4 at 1bpp: 4-byte rows, 16 bytes of pixel data after the 54-byte header.
  const auto path = writeTemp(bmpHeader(70, 54, 8, 4, 1), 70);
  EXPECT_TRUE(FsHelpers::hasCompleteBmp("TEST", path));
  remove(path.c_str());
}

TEST(HasCompleteBmp, RejectsATruncatedBitmapWhoseHeaderUnderstatesItsSize) {
  // bfSize claims the file ends right after the header, but the geometry needs 16 more bytes.
  const auto path = writeTemp(bmpHeader(54, 54, 8, 4, 1), 54);
  EXPECT_FALSE(FsHelpers::hasCompleteBmp("TEST", path));
  remove(path.c_str());
}

TEST(HasCompleteBmp, RejectsFormatsTheRendererCannotDecode) {
  const auto sixteenBit = writeTemp(bmpHeader(200, 54, 8, 4, 16), 200);
  EXPECT_FALSE(FsHelpers::hasCompleteBmp("TEST", sixteenBit));
  remove(sixteenBit.c_str());

  const auto compressed = writeTemp(bmpHeader(200, 54, 8, 4, 8, 1), 200);
  EXPECT_FALSE(FsHelpers::hasCompleteBmp("TEST", compressed));
  remove(compressed.c_str());
}

TEST(HasCompleteBmp, RejectsDimensionsThatWouldOverflowTheSizeCheck) {
  const auto path = writeTemp(bmpHeader(54, 54, 0x7FFFFFFF, 0x7FFFFFFF, 32), 54);
  EXPECT_FALSE(FsHelpers::hasCompleteBmp("TEST", path));
  remove(path.c_str());

  // INT32_MIN as the height: its magnitude must not be taken in int32_t.
  const auto minHeight = writeTemp(bmpHeader(54, 54, 8, INT32_MIN, 1), 54);
  EXPECT_FALSE(FsHelpers::hasCompleteBmp("TEST", minHeight));
  remove(minHeight.c_str());
}

// Sanitizes into a buffer of `size` bytes (at most 64, the size ScreenshotUtil uses).
std::string sanitize(const char* input, const size_t size = 64) {
  char out[64];
  FsHelpers::sanitizePathComponentForFat32(input, out, size);
  return out;
}

// Book titles from the EPUBs attached to #2103 and #2199.
constexpr char kTitle2103[] = "Богиня глюкозы. Нормализуйте уровень сахара в крови, чтобы изменить свою жизнь";
constexpr char kTitle2199[] = "Вглядываясь в солнце. Жизнь без страха смерти";

TEST(SanitizePathComponentForFat32, KeepsTitleThatFits) {
  EXPECT_EQ(sanitize("Эдем (полный перевод)"), "Эдем-(полный-перевод)");
}

// The readers copy the title into ScreenshotInfo::title (char[64]) with snprintf, which can
// end the copy partway through a Cyrillic letter.
TEST(SanitizePathComponentForFat32, DropsLetterCutOffByCaller) {
  char title[64];
  snprintf(title, sizeof(title), "%s", kTitle2103);
  EXPECT_EQ(sanitize(title), "Богиня-глюкозы.-Нормализуйте-уров");
  snprintf(title, sizeof(title), "%s", kTitle2199);
  EXPECT_EQ(sanitize(title), "Вглядываясь-в-солнце.-Жизнь-без-ст");
}

TEST(SanitizePathComponentForFat32, DoesNotSplitLetterAtBufferLimit) {
  // Each letter of "Жизнь" is 2 bytes. 7 bytes of room holds "Жиз" and half of "н".
  EXPECT_EQ(sanitize("Жизнь", 8), "Жиз");
  EXPECT_EQ(sanitize(kTitle2103), "Богиня-глюкозы.-Нормализуйте-уров");
}

}  // namespace
