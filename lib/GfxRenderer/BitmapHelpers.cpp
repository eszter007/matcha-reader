#include "BitmapHelpers.h"

#include <cstdint>
#include <cstring>  // Added for memset

#include "Bitmap.h"

// Brightness/Contrast adjustments:
constexpr bool USE_BRIGHTNESS = false;       // true: apply brightness/gamma adjustments
constexpr int BRIGHTNESS_BOOST = 10;         // Brightness offset (0-50)
constexpr bool GAMMA_CORRECTION = false;     // Gamma curve (brightens midtones)
constexpr float CONTRAST_FACTOR = 1.15f;     // Contrast multiplier (1.0 = no change, >1 = more contrast)
constexpr bool USE_NOISE_DITHERING = false;  // Hash-based noise dithering

// Integer approximation of gamma correction (brightens midtones)
// Uses a simple curve: out = 255 * sqrt(in/255) ≈ sqrt(in * 255)
static inline int applyGamma(int gray) {
  if (!GAMMA_CORRECTION) return gray;
  // Fast integer square root approximation for gamma ~0.5 (brightening)
  // This brightens dark/mid tones while preserving highlights
  const int product = gray * 255;
  // Newton-Raphson integer sqrt (2 iterations for good accuracy)
  int x = gray;
  if (x > 0) {
    x = (x + product / x) >> 1;
    x = (x + product / x) >> 1;
  }
  return x > 255 ? 255 : x;
}

// Apply contrast adjustment around midpoint (128)
// factor > 1.0 increases contrast, < 1.0 decreases
static inline int applyContrast(int gray) {
  // Integer-based contrast: (gray - 128) * factor + 128
  // Using fixed-point: factor 1.15 ≈ 115/100
  constexpr int factorNum = static_cast<int>(CONTRAST_FACTOR * 100);
  int adjusted = ((gray - 128) * factorNum) / 100 + 128;
  if (adjusted < 0) adjusted = 0;
  if (adjusted > 255) adjusted = 255;
  return adjusted;
}
// Gamma 0.75 for 1-bit cover thumbnails. Atkinson diffuses only 6/8 of the error, which drives
// dark midtones to solid black; a saturated red cover (luma ~97) otherwise swallows black
// lettering on it. Gamma 0.6 also fixed that but washed out light covers.
static constexpr uint8_t THUMB_MIDTONE_LIFT[256] = {
    0,   4,   7,   9,   11,  13,  15,  17,  19,  21,  22,  24,  26,  27,  29,  30,  32,  33,  35,  36,  38,  39,
    41,  42,  43,  45,  46,  47,  49,  50,  51,  52,  54,  55,  56,  58,  59,  60,  61,  62,  64,  65,  66,  67,
    68,  69,  71,  72,  73,  74,  75,  76,  77,  78,  80,  81,  82,  83,  84,  85,  86,  87,  88,  89,  90,  91,
    93,  94,  95,  96,  97,  98,  99,  100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114,
    115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 124, 125, 126, 127, 128, 129, 130, 131, 132, 133, 134, 135,
    136, 137, 138, 138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 148, 149, 150, 151, 152, 153, 154, 155,
    156, 157, 157, 158, 159, 160, 161, 162, 163, 164, 164, 165, 166, 167, 168, 169, 170, 170, 171, 172, 173, 174,
    175, 176, 176, 177, 178, 179, 180, 181, 181, 182, 183, 184, 185, 186, 186, 187, 188, 189, 190, 191, 191, 192,
    193, 194, 195, 196, 196, 197, 198, 199, 200, 200, 201, 202, 203, 204, 205, 205, 206, 207, 208, 209, 209, 210,
    211, 212, 213, 213, 214, 215, 216, 216, 217, 218, 219, 220, 220, 221, 222, 223, 224, 224, 225, 226, 227, 227,
    228, 229, 230, 231, 231, 232, 233, 234, 234, 235, 236, 237, 238, 238, 239, 240, 241, 241, 242, 243, 244, 244,
    245, 246, 247, 247, 248, 249, 250, 250, 251, 252, 253, 253, 254, 255,
};

int liftThumbMidtones(int gray) {
  if (gray <= 0) return 0;
  if (gray >= 255) return 255;
  return THUMB_MIDTONE_LIFT[gray];
}

// Combined brightness/contrast/gamma adjustment
int adjustPixel(int gray) {
  if (!USE_BRIGHTNESS) return gray;

  // Order: contrast first, then brightness, then gamma
  gray = applyContrast(gray);
  gray += BRIGHTNESS_BOOST;
  if (gray > 255) gray = 255;
  if (gray < 0) gray = 0;
  gray = applyGamma(gray);

  return gray;
}
// Simple quantization without dithering - divide into 4 levels
// The thresholds are fine-tuned to the X4 display
uint8_t quantizeSimple(int gray) {
  if (gray < 45) {
    return 0;
  } else if (gray < 70) {
    return 1;
  } else if (gray < 140) {
    return 2;
  } else {
    return 3;
  }
}

// Hash-based noise dithering - survives downsampling without moiré artifacts
// Uses integer hash to generate pseudo-random threshold per pixel
static inline uint8_t quantizeNoise(int gray, int x, int y) {
  uint32_t hash = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u;
  hash = (hash ^ (hash >> 13)) * 1274126177u;
  const int threshold = static_cast<int>(hash >> 24);

  const int scaled = gray * 3;
  if (scaled < 255) {
    return (scaled + threshold >= 255) ? 1 : 0;
  } else if (scaled < 510) {
    return ((scaled - 255) + threshold >= 255) ? 2 : 1;
  } else {
    return ((scaled - 510) + threshold >= 255) ? 3 : 2;
  }
}

// Main quantization function - selects between methods based on config
uint8_t quantize(int gray, int x, int y) {
  if (USE_NOISE_DITHERING) {
    return quantizeNoise(gray, x, y);
  } else {
    return quantizeSimple(gray);
  }
}

// 1-bit noise dithering for fast home screen rendering
// Uses hash-based noise for consistent dithering that works well at small sizes
uint8_t quantize1bit(int gray, int x, int y) {
  gray = adjustPixel(gray);

  // Generate noise threshold using integer hash (no regular pattern to alias)
  uint32_t hash = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u;
  hash = (hash ^ (hash >> 13)) * 1274126177u;
  const int threshold = static_cast<int>(hash >> 24);  // 0-255

  // Simple threshold with noise: gray >= (128 + noise offset) -> white
  // The noise adds variation around the 128 midpoint
  const int adjustedThreshold = 128 + ((threshold - 128) / 2);  // Range: 64-192
  return (gray >= adjustedThreshold) ? 1 : 0;
}

void createBmpHeader(BmpHeader* bmpHeader, int width, int height, BmpRowOrder rowOrder) {
  if (!bmpHeader) return;

  // Zero out the memory to ensure no garbage data if called on uninitialized stack memory
  std::memset(bmpHeader, 0, sizeof(BmpHeader));

  uint32_t rowSize = (width + 31) / 32 * 4;
  uint32_t imageSize = rowSize * height;
  uint32_t fileSize = sizeof(BmpHeader) + imageSize;

  bmpHeader->fileHeader.bfType = 0x4D42;
  bmpHeader->fileHeader.bfSize = fileSize;
  bmpHeader->fileHeader.bfReserved1 = 0;
  bmpHeader->fileHeader.bfReserved2 = 0;
  bmpHeader->fileHeader.bfOffBits = sizeof(BmpHeader);

  bmpHeader->infoHeader.biSize = sizeof(bmpHeader->infoHeader);
  bmpHeader->infoHeader.biWidth = width;
  bmpHeader->infoHeader.biHeight = (rowOrder == BmpRowOrder::TopDown) ? -height : height;
  bmpHeader->infoHeader.biPlanes = 1;
  bmpHeader->infoHeader.biBitCount = 1;
  bmpHeader->infoHeader.biCompression = 0;
  bmpHeader->infoHeader.biSizeImage = imageSize;
  bmpHeader->infoHeader.biXPelsPerMeter = 2835;  // 72 DPI
  bmpHeader->infoHeader.biYPelsPerMeter = 2835;  // 72 DPI
  bmpHeader->infoHeader.biClrUsed = 2;
  bmpHeader->infoHeader.biClrImportant = 2;

  // Color 0 (black)
  bmpHeader->colors[0].rgbBlue = 0;
  bmpHeader->colors[0].rgbGreen = 0;
  bmpHeader->colors[0].rgbRed = 0;
  bmpHeader->colors[0].rgbReserved = 0;

  // Color 1 (white)
  bmpHeader->colors[1].rgbBlue = 255;
  bmpHeader->colors[1].rgbGreen = 255;
  bmpHeader->colors[1].rgbRed = 255;
  bmpHeader->colors[1].rgbReserved = 0;
}