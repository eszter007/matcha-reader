#include "SleepActivity.h"

#include <BitmapHelpers.h>
#include <DictIndex.h>
#include <Epub.h>
#include <Epub/converters/PngToFramebufferConverter.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>
#include <InflateReader.h>
#include <Memory.h>
#include <PNGdec.h>
#include <Xtc.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "GrayLogo.h"
#include "ReadingStatsStore.h"
#include "SdCardFontSystem.h"
#include "activities/reader/ReaderUtils.h"
#include "components/StatsWidgets.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "images/DeparturesType.h"
#include "images/MoonIcon.h"

namespace {
// Metalio: B/W sleep images use the full 0xF7 waveform instead of HALF (two
// 0xFC partials), matching the clean pass before absolute gray below.
#if FREEINK_DEVICE_METALIO_EINK4
constexpr auto kSleepClean = HalDisplay::FULL_REFRESH;
#else
constexpr auto kSleepClean = HalDisplay::HALF_REFRESH;
#endif

// The absolute gray paint has no base pass. On Metalio the prior screen shows
// through it after the panel sits unpowered, so clean with 0xF7 first.
void cleanBeforeAbsoluteGray(GfxRenderer& renderer) {
#if FREEINK_DEVICE_METALIO_EINK4
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
#else
  (void)renderer;
#endif
}

HalDisplay::GrayscaleMode sleepGrayscaleMode(const GfxRenderer& renderer) {
  return renderer.grayscaleCapabilities(HalDisplay::GrayscaleMode::Direct).supported()
             ? HalDisplay::GrayscaleMode::Direct
             : HalDisplay::GrayscaleMode::Absolute;
}

// Kept separate from normal custom sleep sources so alpha-overlay art does not mix with full-screen wallpapers.
constexpr char TRANSPARENT_SLEEP_ROOT_BMP[] = "/sleep-overlay.bmp";
constexpr char TRANSPARENT_SLEEP_ROOT_PNG[] = "/sleep-overlay.png";
constexpr const char* SLEEP_DIRS[] = {"/.sleep", "/sleep"};
// The last two are the fork's own overlay folders from before upstream added overlays.
constexpr const char* TRANSPARENT_SLEEP_DIRS[] = {"/.sleep-overlay", "/sleep-overlay", "/.sleep/transparent",
                                                  "/sleep/transparent"};
constexpr size_t MAX_SLEEP_FILE_NAME_LEN = 256;
constexpr uint8_t MIN_VISIBLE_ALPHA = 8;

struct BitmapPlacement {
  int x = 0;
  int y = 0;
  float cropX = 0.0f;
  float cropY = 0.0f;
};

struct OverlayBmpInfo {
  int width = 0;
  int height = 0;
  bool topDown = false;
  uint32_t dataOffset = 0;
  uint32_t rowBytes = 0;
};

uint16_t readLE16(HalFile& file) {
  const int c0 = file.read();
  const int c1 = file.read();
  const auto b0 = static_cast<uint8_t>(c0 < 0 ? 0 : c0);
  const auto b1 = static_cast<uint8_t>(c1 < 0 ? 0 : c1);
  return static_cast<uint16_t>(b0) | (static_cast<uint16_t>(b1) << 8);
}

uint32_t readLE32(HalFile& file) {
  const int c0 = file.read();
  const int c1 = file.read();
  const int c2 = file.read();
  const int c3 = file.read();
  const auto b0 = static_cast<uint8_t>(c0 < 0 ? 0 : c0);
  const auto b1 = static_cast<uint8_t>(c1 < 0 ? 0 : c1);
  const auto b2 = static_cast<uint8_t>(c2 < 0 ? 0 : c2);
  const auto b3 = static_cast<uint8_t>(c3 < 0 ? 0 : c3);
  return static_cast<uint32_t>(b0) | (static_cast<uint32_t>(b1) << 8) | (static_cast<uint32_t>(b2) << 16) |
         (static_cast<uint32_t>(b3) << 24);
}

uint32_t readBE32(HalFile& file) {
  const int c0 = file.read();
  const int c1 = file.read();
  const int c2 = file.read();
  const int c3 = file.read();
  if (c0 < 0 || c1 < 0 || c2 < 0 || c3 < 0) return 0;
  return (static_cast<uint32_t>(c0) << 24) | (static_cast<uint32_t>(c1) << 16) | (static_cast<uint32_t>(c2) << 8) |
         static_cast<uint32_t>(c3);
}

bool isValidPngHeader(HalFile& file) {
  static constexpr uint8_t PNG_SIGNATURE[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  static constexpr uint32_t MAX_SOURCE_PIXELS = 2048u * 1536u;
  uint8_t signature[8];
  if (!file.seek(0) || file.read(signature, sizeof(signature)) != static_cast<int>(sizeof(signature)) ||
      !std::equal(std::begin(signature), std::end(signature), std::begin(PNG_SIGNATURE))) {
    return false;
  }

  const uint32_t ihdrLength = readBE32(file);
  char chunkType[4];
  if (file.read(reinterpret_cast<uint8_t*>(chunkType), sizeof(chunkType)) != static_cast<int>(sizeof(chunkType)) ||
      ihdrLength != 13 || !std::equal(std::begin(chunkType), std::end(chunkType), "IHDR")) {
    return false;
  }

  const uint32_t width = readBE32(file);
  const uint32_t height = readBE32(file);
  const int bitDepth = file.read();
  const int colorType = file.read();
  const int compression = file.read();
  const int filter = file.read();
  const int interlace = file.read();

  const bool supportedBitDepth =
      bitDepth == 8 || ((colorType == PNG_PIXEL_GRAYSCALE || colorType == PNG_PIXEL_INDEXED) &&
                        (bitDepth == 1 || bitDepth == 2 || bitDepth == 4));
  const bool supportedColorType = colorType == PNG_PIXEL_GRAYSCALE || colorType == PNG_PIXEL_TRUECOLOR ||
                                  colorType == PNG_PIXEL_INDEXED || colorType == PNG_PIXEL_GRAY_ALPHA ||
                                  colorType == PNG_PIXEL_TRUECOLOR_ALPHA;
  return width > 0 && height > 0 && width <= 2048 && height <= 3072 && width * height <= MAX_SOURCE_PIXELS &&
         supportedBitDepth && supportedColorType && compression == 0 && filter == 0 && interlace == 0;
}

BitmapPlacement calculateBitmapPlacement(const int bitmapWidth, const int bitmapHeight, const GfxRenderer& renderer) {
  BitmapPlacement placement;
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  if (bitmapWidth > pageWidth || bitmapHeight > pageHeight) {
    float ratio = static_cast<float>(bitmapWidth) / static_cast<float>(bitmapHeight);
    const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);

    if (ratio > screenRatio) {
      if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP) {
        placement.cropX = 1.0f - (screenRatio / ratio);
        ratio = (1.0f - placement.cropX) * static_cast<float>(bitmapWidth) / static_cast<float>(bitmapHeight);
      }
      placement.x = 0;
      placement.y = std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
    } else {
      if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP) {
        placement.cropY = 1.0f - (ratio / screenRatio);
        ratio = static_cast<float>(bitmapWidth) / ((1.0f - placement.cropY) * static_cast<float>(bitmapHeight));
      }
      placement.x = std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2);
      placement.y = 0;
    }
  } else {
    placement.x = (pageWidth - bitmapWidth) / 2;
    placement.y = (pageHeight - bitmapHeight) / 2;
  }

  return placement;
}

bool parseOverlayBmpHeader(HalFile& file, OverlayBmpInfo& info, const bool logErrors) {
  if (!file) return false;
  if (!file.seek(0)) return false;

  if (readLE16(file) != 0x4D42) {
    if (logErrors) LOG_ERR("SLP", "Transparent overlay is not a BMP");
    return false;
  }

  file.seekCur(8);
  info.dataOffset = readLE32(file);

  const uint32_t dibSize = readLE32(file);
  if (dibSize < 40) {
    if (logErrors) LOG_ERR("SLP", "Unsupported BMP DIB header: %u", static_cast<unsigned>(dibSize));
    return false;
  }

  info.width = static_cast<int32_t>(readLE32(file));
  const auto rawHeight = static_cast<int32_t>(readLE32(file));
  if (rawHeight == std::numeric_limits<int32_t>::min()) {
    if (logErrors) LOG_ERR("SLP", "Bad transparent overlay dimensions: %dx%d", info.width, rawHeight);
    return false;
  }
  info.topDown = rawHeight < 0;
  info.height = info.topDown ? -rawHeight : rawHeight;

  const uint16_t planes = readLE16(file);
  const uint16_t bpp = readLE16(file);
  const uint32_t compression = readLE32(file);

  // Match Bitmap::parseHeaders(): accept BI_RGB (0) and 32bpp BI_BITFIELDS (3), but keep the same
  // byte-layout assumption as custom sleep BMPs. The renderer below treats pixels as BGRA and does not parse masks.
  if (planes != 1 || bpp != 32 || !(compression == 0 || compression == 3)) {
    if (logErrors) {
      LOG_ERR("SLP", "Transparent overlay must be 32-bit BGRA BMP (planes=%u bpp=%u comp=%u)", planes, bpp,
              static_cast<unsigned>(compression));
    }
    return false;
  }

  constexpr int MAX_IMAGE_WIDTH = 2048;
  constexpr int MAX_IMAGE_HEIGHT = 3072;
  if (info.width <= 0 || info.height <= 0 || info.width > MAX_IMAGE_WIDTH || info.height > MAX_IMAGE_HEIGHT) {
    if (logErrors) LOG_ERR("SLP", "Bad transparent overlay dimensions: %dx%d", info.width, info.height);
    return false;
  }

  info.rowBytes = static_cast<uint32_t>(info.width) * 4u;
  if (!file.seek(info.dataOffset)) {
    if (logErrors) LOG_ERR("SLP", "Failed to seek transparent overlay pixel data");
    return false;
  }

  return true;
}

uint8_t bayerThreshold4x4(const int x, const int y) {
  static constexpr uint8_t BAYER_4X4[16] = {0, 128, 32, 160, 192, 64, 224, 96, 48, 176, 16, 144, 240, 112, 208, 80};
  return BAYER_4X4[((y & 0x03) << 2) | (x & 0x03)];
}

enum class TransparentOverlayPass : uint8_t { BW, GrayscaleLsb, GrayscaleMsb };

uint8_t quantizeOverlayLum(const uint8_t lum) {
  // Match Bitmap's native-palette path: 0, 85, 170, 255 map directly to levels 0..3.
  return lum >> 6;
}

bool renderTransparentOverlayPass(HalFile& file, const OverlayBmpInfo& info, const BitmapPlacement& placement,
                                  const GfxRenderer& renderer, uint8_t* row, const TransparentOverlayPass pass) {
  if (!file.seek(info.dataOffset)) {
    LOG_ERR("SLP", "Failed to seek transparent overlay pixel data");
    return false;
  }

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const int cropPixX = std::floor(info.width * placement.cropX / 2.0f);
  const int cropPixY = std::floor(info.height * placement.cropY / 2.0f);
  const float croppedWidth = (1.0f - placement.cropX) * static_cast<float>(info.width);
  const float croppedHeight = (1.0f - placement.cropY) * static_cast<float>(info.height);

  float scale = 1.0f;
  if (croppedWidth > 0.0f && croppedHeight > 0.0f) {
    const float widthScale = static_cast<float>(pageWidth) / croppedWidth;
    const float heightScale = static_cast<float>(pageHeight) / croppedHeight;
    scale = std::min(widthScale, heightScale);
    if (scale > 1.0f) scale = 1.0f;
  }
  const bool isScaled = scale < 1.0f;

  for (int bmpY = 0; bmpY < info.height; bmpY++) {
    if (file.read(row, info.rowBytes) != static_cast<int>(info.rowBytes)) {
      LOG_ERR("SLP", "Short read in transparent overlay row %d", bmpY);
      return false;
    }

    int screenY = -cropPixY + (info.topDown ? bmpY : info.height - 1 - bmpY);
    if (isScaled) screenY = std::floor(screenY * scale);
    screenY += placement.y;

    if (screenY >= pageHeight) {
      if (info.topDown) break;
      continue;
    }
    if (screenY < 0) {
      if (!info.topDown) break;
      continue;
    }

    for (int bmpX = cropPixX; bmpX < info.width - cropPixX; bmpX++) {
      int screenX = bmpX - cropPixX;
      if (isScaled) screenX = std::floor(screenX * scale);
      screenX += placement.x;

      if (screenX >= renderer.getScreenWidth()) break;
      if (screenX < 0) continue;

      const uint8_t* pixel = row + (static_cast<size_t>(bmpX) * 4u);
      const uint8_t alpha = pixel[3];
      if (alpha < MIN_VISIBLE_ALPHA || alpha <= bayerThreshold4x4(screenX, screenY)) continue;

      const uint8_t lum = (77u * pixel[2] + 150u * pixel[1] + 29u * pixel[0]) >> 8;
      const uint8_t level = quantizeOverlayLum(lum);

      switch (pass) {
        case TransparentOverlayPass::BW:
          // Same first pass as custom bitmap sleep: all non-white levels are painted black.
          // Transparent overlay's only difference is that opaque white explicitly erases underlying text.
          renderer.drawPixel(screenX, screenY, level < 3);
          break;
        case TransparentOverlayPass::GrayscaleLsb:
        case TransparentOverlayPass::GrayscaleMsb: {
          const auto planePixel =
              grayPlanePixel(level, pass == TransparentOverlayPass::GrayscaleMsb, renderer.grayPlanesAreAbsolute());
          if (planePixel.write) renderer.drawPixel(screenX, screenY, planePixel.black);
          break;
        }
      }
    }
  }

  return true;
}

// Starts a gray plane under an overlay. Absolute planes keep the B/W base; other gray planes carry only
// the overlay's gray. Cover + Transparent redraws a gray cover (set only for absolute planes) to keep its gray levels.
// Overlay gray leaves a mask plane in the framebuffer, so a later B/W update of
// the retained sleep frame (the Picco USB charge strip) needs the B/W frame back.
// Absolute planes must keep the gray baseline: restoring the B/W frame there
// makes the next fast refresh drop the grays (Picco SSD1677).
class BwFrameKeeper {
 public:
  BwFrameKeeper(GfxRenderer& renderer, const bool keep)
      : renderer(renderer), keep(keep), stored(!keep || renderer.storeBwBuffer()) {}
  ~BwFrameKeeper() {
    if (keep && stored) renderer.restoreBwBuffer();
  }
  // False when the B/W frame could not be kept; callers must skip the gray passes.
  bool ready() const { return stored; }
  BwFrameKeeper(const BwFrameKeeper&) = delete;
  BwFrameKeeper& operator=(const BwFrameKeeper&) = delete;

 private:
  GfxRenderer& renderer;
  const bool keep;
  const bool stored;
};

bool beginOverlayGrayPlane(GfxRenderer& renderer, const Bitmap* grayCover, const GfxRenderer::RenderMode plane,
                           const bool absolute) {
  if (grayCover) {
    if (grayCover->rewindToData() != BmpReaderError::Ok) return false;
    renderer.clearScreen();
  } else if (!absolute) {
    renderer.clearScreen(0x00);
  }
  renderer.setRenderMode(plane);
  if (!grayCover) return true;
  const auto placement = calculateBitmapPlacement(grayCover->getWidth(), grayCover->getHeight(), renderer);
  return renderer.drawBitmap(*grayCover, placement.x, placement.y, renderer.getScreenWidth(),
                             renderer.getScreenHeight(), placement.cropX, placement.cropY);
}

enum class AlphaOverlayResult : uint8_t { Rendered, NotAlphaOverlay, Error };
enum class AlphaScanResult : uint8_t { Useful, NotUseful, Error };

AlphaScanResult scanForUsefulAlpha(HalFile& file, const OverlayBmpInfo& info, uint8_t* row) {
  if (!file.seek(info.dataOffset)) {
    LOG_ERR("SLP", "Failed to seek transparent overlay pixel data");
    return AlphaScanResult::Error;
  }

  bool hasVisiblePixel = false;
  bool hasNonOpaquePixel = false;
  for (int bmpY = 0; bmpY < info.height; bmpY++) {
    if (file.read(row, info.rowBytes) != static_cast<int>(info.rowBytes)) {
      LOG_ERR("SLP", "Short read while checking transparent overlay row %d", bmpY);
      return AlphaScanResult::Error;
    }

    for (int bmpX = 0; bmpX < info.width; bmpX++) {
      const uint8_t alpha = row[static_cast<size_t>(bmpX) * 4u + 3u];
      hasVisiblePixel |= alpha >= MIN_VISIBLE_ALPHA;
      hasNonOpaquePixel |= alpha < 255;
      if (hasVisiblePixel && hasNonOpaquePixel) return AlphaScanResult::Useful;
    }
  }

  return AlphaScanResult::NotUseful;
}

AlphaOverlayResult tryRenderTransparentOverlayBmp(HalFile& file, GfxRenderer& renderer, const char* pathForLog,
                                                  const Bitmap* grayCover) {
  OverlayBmpInfo info;
  if (!parseOverlayBmpHeader(file, info, false)) return AlphaOverlayResult::NotAlphaOverlay;

  const auto placement = calculateBitmapPlacement(info.width, info.height, renderer);
  auto row = makeUniqueNoThrow<uint8_t[]>(info.rowBytes);
  if (!row) {
    LOG_ERR("SLP", "OOM: transparent overlay row (%u bytes)", static_cast<unsigned>(info.rowBytes));
    return AlphaOverlayResult::Error;
  }

  const auto alphaScanResult = scanForUsefulAlpha(file, info, row.get());
  if (alphaScanResult == AlphaScanResult::Error) return AlphaOverlayResult::Error;
  if (alphaScanResult == AlphaScanResult::NotUseful) return AlphaOverlayResult::NotAlphaOverlay;

  LOG_DBG("SLP", "Rendering transparent overlay: %s (%dx%d)", pathForLog, info.width, info.height);

  if (!renderTransparentOverlayPass(file, info, placement, renderer, row.get(), TransparentOverlayPass::BW))
    return AlphaOverlayResult::Error;
  const bool absolute = renderer.grayscaleCapabilities(sleepGrayscaleMode(renderer)).supported();
  const BwFrameKeeper bwFrame(renderer, !absolute);
  if (!bwFrame.ready()) {
    renderer.displayBuffer(kSleepClean);
    return AlphaOverlayResult::Rendered;
  }
  if (absolute) {
    cleanBeforeAbsoluteGray(renderer);
    if (!renderer.displayGrayscaleBase(sleepGrayscaleMode(renderer))) return AlphaOverlayResult::Error;
  } else {
    renderer.displayGrayscaleBase(HalDisplay::HALF_REFRESH);
  }

  // Absolute planes retain B/W background bits; each visible overlay pixel is rewritten in both passes.
  if (!beginOverlayGrayPlane(renderer, grayCover, GfxRenderer::GRAYSCALE_LSB, absolute) ||
      !renderTransparentOverlayPass(file, info, placement, renderer, row.get(), TransparentOverlayPass::GrayscaleLsb)) {
    renderer.setRenderMode(GfxRenderer::BW);
    // Keep the current display instead of trying another overlay with a
    // framebuffer that now contains an incomplete gray plane.
    return AlphaOverlayResult::Rendered;
  }
  renderer.copyGrayscaleLsbBuffers();

  if (!beginOverlayGrayPlane(renderer, grayCover, GfxRenderer::GRAYSCALE_MSB, absolute) ||
      !renderTransparentOverlayPass(file, info, placement, renderer, row.get(), TransparentOverlayPass::GrayscaleMsb)) {
    renderer.setRenderMode(GfxRenderer::BW);
    return AlphaOverlayResult::Rendered;
  }
  renderer.copyGrayscaleMsbBuffers();

  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);
  return AlphaOverlayResult::Rendered;
}

enum class SleepRecentKind : uint8_t { Standard, Overlay };

bool isRecentSleepIndex(const SleepRecentKind recentKind, const uint16_t idx, const uint8_t window) {
  return recentKind == SleepRecentKind::Overlay ? APP_STATE.isRecentOverlaySleep(idx, window)
                                                : APP_STATE.isRecentSleep(idx, window);
}

void pushRecentSleepIndex(const SleepRecentKind recentKind, const uint16_t idx) {
  if (recentKind == SleepRecentKind::Overlay) {
    APP_STATE.pushRecentOverlaySleep(idx);
  } else {
    APP_STATE.pushRecentSleep(idx);
  }
}

bool findNextValidSleepImage(HalFile& dir, const SleepRecentKind recentKind, char* name) {
  for (auto dirFile = dir.openNextFile(); dirFile; dirFile = dir.openNextFile()) {
    if (dirFile.isDirectory()) continue;

    dirFile.getName(name, MAX_SLEEP_FILE_NAME_LEN);
    if (name[0] == '\0' || name[0] == '.') continue;

    const bool isBmp = FsHelpers::hasBmpExtension(name);
    const bool isPng = recentKind == SleepRecentKind::Overlay && FsHelpers::hasPngExtension(std::string_view{name});
    if (!isBmp && !isPng) {
      LOG_DBG("SLP", "Skipping unsupported sleep image: %s", name);
      continue;
    }

    const bool isValid = isBmp ? [&dirFile]() {
      Bitmap bitmap(dirFile);
      return bitmap.parseHeaders() == BmpReaderError::Ok;
    }()
                               : isValidPngHeader(dirFile);
    if (!isValid) {
      LOG_DBG("SLP", "Skipping invalid sleep image: %s", name);
      continue;
    }
    return true;
  }
  return false;
}

// The folders form one pool: each image is equally likely whichever folder holds it.
bool selectRandomSleepFile(const char* const* dirPaths, size_t dirCount, const SleepRecentKind recentKind,
                           std::string& selectedPath) {
  auto name = makeUniqueNoThrow<char[]>(MAX_SLEEP_FILE_NAME_LEN);
  if (!name) {
    LOG_ERR("SLP", "OOM: sleep filename buffer");
    return false;
  }

  constexpr size_t MAX_DIRECTORIES = 4;
  dirCount = std::min(dirCount, MAX_DIRECTORIES);
  uint16_t directoryCounts[MAX_DIRECTORIES] = {};
  uint16_t fileCount = 0;
  for (size_t i = 0; i < dirCount; i++) {
    auto dir = Storage.open(dirPaths[i]);
    if (!dir || !dir.isDirectory()) continue;

    const auto remainingCapacity = static_cast<uint16_t>(UINT16_MAX - fileCount);
    while (directoryCounts[i] < remainingCapacity && findNextValidSleepImage(dir, recentKind, name.get())) {
      directoryCounts[i]++;
    }
    fileCount = static_cast<uint16_t>(fileCount + directoryCounts[i]);
  }

  if (fileCount == 0) return false;

  // Pick a random wallpaper, excluding recently shown ones.
  // Window: up to SLEEP_RECENT_COUNT entries, capped at fileCount-1.
  const uint8_t recentFill =
      recentKind == SleepRecentKind::Overlay ? APP_STATE.recentOverlaySleepFill : APP_STATE.recentSleepFill;
  const uint8_t window = static_cast<uint8_t>(std::min<uint16_t>(recentFill, fileCount - 1));
  auto randomFileIndex = static_cast<uint16_t>(random(fileCount));
  for (uint8_t attempt = 0; attempt < 20 && isRecentSleepIndex(recentKind, randomFileIndex, window); attempt++) {
    randomFileIndex = static_cast<uint16_t>(random(fileCount));
  }

  size_t directoryIndex = 0;
  uint16_t localIndex = randomFileIndex;
  while (directoryIndex + 1 < dirCount && localIndex >= directoryCounts[directoryIndex]) {
    localIndex = static_cast<uint16_t>(localIndex - directoryCounts[directoryIndex]);
    directoryIndex++;
  }

  auto dir = Storage.open(dirPaths[directoryIndex]);
  if (!dir || !dir.isDirectory()) {
    LOG_ERR("SLP", "Failed to open selected sleep image directory: %s", dirPaths[directoryIndex]);
    return false;
  }
  for (uint16_t index = 0; index <= localIndex; ++index) {
    if (!findNextValidSleepImage(dir, recentKind, name.get())) return false;
  }

  const char* selectedDirPath = dirPaths[directoryIndex];
  selectedPath.reserve(strlen(selectedDirPath) + 1 + strlen(name.get()));
  selectedPath = selectedDirPath;
  selectedPath += "/";
  selectedPath += name.get();
  pushRecentSleepIndex(recentKind, randomFileIndex);
  APP_STATE.saveToFile();
  return true;
}

bool drawSleepPopupPreservingFrame(GfxRenderer& renderer) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int frameThickness = metrics.popupFrameThickness;
  const int popupY = static_cast<int>(renderer.getScreenHeight() * metrics.popupTopOffsetRatio);
  const int popupHeight = renderer.getLineHeight(UI_12_FONT_ID) + metrics.popupMarginY * 2;
  const int bandTop = std::max(0, popupY - frameThickness);
  const int bandBottom = std::min(renderer.getScreenHeight(), popupY + popupHeight + frameThickness);
  const int bandHeight = bandBottom - bandTop;
  const size_t bandBytes = renderer.getRegionByteSize(0, bandTop, renderer.getScreenWidth(), bandHeight);

  auto savedBand = makeUniqueNoThrow<uint8_t[]>(bandBytes);
  if (!savedBand) {
    LOG_ERR("SLP", "OOM: sleep popup background (%u bytes)", static_cast<unsigned>(bandBytes));
    return false;
  }
  if (!renderer.copyRegionToBuffer(0, bandTop, renderer.getScreenWidth(), bandHeight, savedBand.get(), bandBytes)) {
    LOG_ERR("SLP", "Failed to save sleep popup background");
    return false;
  }

  GUI.drawPopup(renderer, tr(STR_ENTERING_SLEEP));
  if (!renderer.copyBufferToRegion(0, bandTop, renderer.getScreenWidth(), bandHeight, savedBand.get(), bandBytes)) {
    LOG_ERR("SLP", "Failed to restore sleep popup background");
    return false;
  }
  return true;
}

void releaseSdFontCachesForDecode(const GfxRenderer& renderer) {
  if (auto* fcm = renderer.getFontCacheManager()) {
    LOG_DBG("SLP", "Before SD font cache release: free=%u largest=%u", static_cast<unsigned>(ESP.getFreeHeap()),
            static_cast<unsigned>(ESP.getMaxAllocHeap()));
    // releaseSdFontCaches() was renamed/widened to releaseAllFontMemory() during the fork's
    // font-cache work: same SD-cache release this call wants, plus the persistent advance
    // tables and FontDecompressor glyph slab, which only helps an image-decode heap squeeze.
    fcm->releaseAllFontMemory();
    LOG_DBG("SLP", "Before sleep image decode: free=%u largest=%u", static_cast<unsigned>(ESP.getFreeHeap()),
            static_cast<unsigned>(ESP.getMaxAllocHeap()));
  }
}

}  // namespace

void SleepActivity::onEnter() {
  Activity::onEnter();

  const bool renderQuickResume =
      SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::QUICK_RESUME ||
      (fromTimeout &&
       SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT);

  if (renderQuickResume) {
    // Quick Resume keeps the current frame as-is, so the driver's inversion
    // state stays too: a night-mode page sleeps in night polarity, and the
    // moon icon inverts with it at transfer like any other draw.
    return renderLastScreenSleepScreen();
  }

  const bool frameWasInverted = display.isInverted();

  // The remaining sleep screens draw fresh content in normal polarity. This
  // activity draws directly from onEnter (outside ActivityManager's
  // per-render polarity resolution), so clear any inversion left over from a
  // night-mode reader render.
  display.setInverted(false);

  const uint8_t sleepScreen = SETTINGS.sleepScreen;
  if (sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::TRANSPARENT_CUSTOM ||
      sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::COVER_TRANSPARENT) {
    // Transparent mode retains the current framebuffer. Materialize any
    // output-level inversion first so the retained content keeps its visible
    // polarity after the display driver returns to normal.
    if (frameWasInverted) renderer.invertScreen();
    if (APP_STATE.lastSleepFromReader) {
      ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
    }
    drawSleepPopupPreservingFrame(renderer);
    if (APP_STATE.lastSleepFromReader) {
      renderer.setOrientation(GfxRenderer::Orientation::Portrait);
    }
    releaseSdFontCachesForDecode(renderer);
    if (sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::COVER_TRANSPARENT) {
      return renderCoverSleepScreen();
    }
    return renderTransparentCustomSleepScreen();
  }

  // Show popup with reader orientation only when going to sleep from reader
  if (APP_STATE.lastSleepFromReader) {
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
    GUI.drawPopup(renderer, tr(STR_ENTERING_SLEEP));
    renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  } else {
    GUI.drawPopup(renderer, tr(STR_ENTERING_SLEEP));
  }

  switch (SETTINGS.sleepScreen) {
    case (CrossPointSettings::SLEEP_SCREEN_MODE::BLANK):
      return renderBlankSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::STATS):
      return renderDeparturesSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM):
      return renderCustomSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER):
      return renderCoverSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM):
      if (APP_STATE.lastSleepFromReader) {
        return renderCoverSleepScreen();
      } else {
        return renderCustomSleepScreen();
      }
    default:
      return renderDefaultSleepScreen();
  }
}

void SleepActivity::renderCustomSleepScreen() const {
  // Look for sleep.bmp on the root of the sd card to determine if we should
  // render a custom sleep screen instead of the default.
  // This takes priority over both custom sleep directories.
  HalFile file;
  if (Storage.openFileForRead("SLP", "/sleep.bmp", file)) {
    Bitmap bitmap(file, true,
                  renderer.grayscaleCapabilities(sleepGrayscaleMode(renderer)).supported() &&
                      display.getController() == HalDisplay::Controller::SSD1677 &&
                      SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      LOG_DBG("SLP", "Loading: /sleep.bmp");
      renderBitmapSleepScreen(bitmap);
      file.close();
      return;
    }
    file.close();
  }

  std::string selectedPath;
  selectRandomSleepFile(SLEEP_DIRS, std::size(SLEEP_DIRS), SleepRecentKind::Standard, selectedPath);

  if (!selectedPath.empty()) {
    HalFile randFile;
    if (Storage.openFileForRead("SLP", selectedPath, randFile)) {
      LOG_DBG("SLP", "Randomly loading: %s", selectedPath.c_str());
      delay(100);
      Bitmap bitmap(randFile, true,
                    renderer.grayscaleCapabilities(sleepGrayscaleMode(renderer)).supported() &&
                        display.getController() == HalDisplay::Controller::SSD1677 &&
                        SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        renderBitmapSleepScreen(bitmap);
        randFile.close();
        return;
      }
      randFile.close();
    }
  }

  renderDefaultSleepScreen();
}

// Sleep screens paint with a single HALF refresh (stock parity): the OEM X4
// firmware's only clean refresh in normal operation is the single-pass 0xD7
// sequence, used once for the sleep image. It never runs the multi-flash GC
// waveform (0xF7) that FULL_REFRESH selects (#2471's blinking complaint).
void SleepActivity::renderDefaultSleepScreen() const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  const int logoX = (pageWidth - GrayLogo::WIDTH) / 2;
  const int logoY = GrayLogo::top(pageHeight);
  const bool inverted = SETTINGS.sleepScreen != CrossPointSettings::SLEEP_SCREEN_MODE::LIGHT;
  renderer.clearScreen();
  GrayLogo::drawBase(renderer, logoX, logoY, inverted);
  renderer.drawCenteredText(UI_12_FONT_ID, pageHeight - GrayLogo::WORDMARK_FROM_BOTTOM, tr(STR_CROSSPOINT), true,
                            EpdFontFamily::BOLD);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - GrayLogo::STATUS_FROM_BOTTOM, tr(STR_SLEEPING));

  if (inverted) renderer.invertScreen();
  renderer.displayGrayscaleBase(HalDisplay::HALF_REFRESH);
  GrayLogo::flushGrayPasses(renderer, logoX, logoY, inverted);
}

void SleepActivity::renderBitmapSleepScreen(const Bitmap& bitmap, const bool preserveBackground) const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto placement = calculateBitmapPlacement(bitmap.getWidth(), bitmap.getHeight(), renderer);
  const int x = placement.x;
  const int y = placement.y;
  const float cropX = placement.cropX;
  const float cropY = placement.cropY;

  LOG_DBG("SLP", "bitmap %d x %d, screen %d x %d", bitmap.getWidth(), bitmap.getHeight(), pageWidth, pageHeight);
  LOG_DBG("SLP", "drawing to %d x %d", x, y);
  if (!preserveBackground) renderer.clearScreen();

  const bool hasGreyscale = (bitmap.hasGreyscale() || (preserveBackground && overlayGrayCover)) &&
                            (preserveBackground || SETTINGS.sleepScreenCoverFilter ==
                                                       CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER);

  // A cover under an overlay on a panel without absolute gray planes is shown B/W only (see the
  // overlay renderers), so dither its grays rather than letting them all turn black.
  const bool ditherCover = overlayCover == &bitmap && hasGreyscale &&
                           !renderer.grayscaleCapabilities(sleepGrayscaleMode(renderer)).supported();
  if (!renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY, /*allowUpscale=*/false,
                           /*whiteAsTransparent=*/preserveBackground, ditherCover)) {
    renderer.displayBuffer(kSleepClean);
    return;
  }

  if (!preserveBackground &&
      SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::INVERTED_BLACK_AND_WHITE) {
    renderer.invertScreen();
  }

  // Cover + Transparent: the B/W cover is the overlay's background; the overlay renderers redraw its gray planes.
  if (overlayCover == &bitmap) {
    if (hasGreyscale && renderer.grayscaleCapabilities(sleepGrayscaleMode(renderer)).supported()) {
      overlayGrayCover = &bitmap;
    }
    renderTransparentCustomSleepScreen();
    overlayGrayCover = nullptr;
    return;
  }

  const bool absolute = hasGreyscale && renderer.grayscaleCapabilities(sleepGrayscaleMode(renderer)).supported();
  const BwFrameKeeper bwFrame(renderer, hasGreyscale && !absolute);
  if (!bwFrame.ready()) {
    renderer.displayBuffer(kSleepClean);
    return;
  }
  if (absolute) {
    cleanBeforeAbsoluteGray(renderer);
    if (!renderer.displayGrayscaleBase(sleepGrayscaleMode(renderer))) {
      // The B/W sleep image is already in the framebuffer; show that rather than leaving the
      // previous screen up.
      LOG_ERR("SLEEP", "Absolute grayscale base failed; showing the B/W sleep image");
      renderer.displayBuffer(kSleepClean);
      return;
    }
  } else if (hasGreyscale) {
    // OEM grayscale pipeline base. Must stay HALF: the gray nudge LUT is
    // calibrated against the pixel state the single-pass HALF waveform leaves
    // behind. A FULL (GC) base parks pixels in a different charge state and
    // the differential nudge then lands unevenly (blotchy noise in gray areas).
    renderer.displayGrayscaleBase(HalDisplay::HALF_REFRESH);
  } else {
    renderer.displayBuffer(kSleepClean);
  }

  if (hasGreyscale) {
    bool ready = true;
    for (const auto plane : {GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
      if (bitmap.rewindToData() != BmpReaderError::Ok) {
        ready = false;
        break;
      }
      if (preserveBackground) {
        if (!beginOverlayGrayPlane(renderer, overlayGrayCover, plane, absolute)) {
          ready = false;
          break;
        }
      } else {
        renderer.clearScreen(absolute ? 0xFF : 0x00);
        renderer.setRenderMode(plane);
      }
      if (!renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY, /*allowUpscale=*/false,
                               /*whiteAsTransparent=*/preserveBackground)) {
        ready = false;
        break;
      }
      if (plane == GfxRenderer::GRAYSCALE_LSB)
        renderer.copyGrayscaleLsbBuffers();
      else
        renderer.copyGrayscaleMsbBuffers();
    }
    if (ready)
      renderer.displayGrayBuffer();
    else
      LOG_ERR("SLP", "Incomplete grayscale image; keeping the current display");
    renderer.setRenderMode(GfxRenderer::BW);
  }
}

bool SleepActivity::renderSleepOverlayFile(HalFile& file, const char* pathForLog) const {
  const auto alphaResult = tryRenderTransparentOverlayBmp(file, renderer, pathForLog, overlayGrayCover);
  if (alphaResult == AlphaOverlayResult::Rendered) return true;
  if (alphaResult == AlphaOverlayResult::Error) return false;

  Bitmap bitmap(file);
  const auto parseResult = bitmap.parseHeaders();
  if (parseResult != BmpReaderError::Ok) {
    LOG_ERR("SLP", "Invalid sleep overlay BMP %s: %s", pathForLog, Bitmap::errorToString(parseResult));
    return false;
  }

  LOG_DBG("SLP", "Rendering regular BMP sleep overlay: %s (%dx%d)", pathForLog, bitmap.getWidth(), bitmap.getHeight());
  // drawBitmap leaves white pixels untouched; skipping the initial clear makes
  // them transparent while retaining the existing grayscale pipeline.
  renderBitmapSleepScreen(bitmap, true);
  return true;
}

bool SleepActivity::renderTransparentOverlayPng(const std::string& path) const {
  // The decoder is a single ~60 KB object, and sleeping from inside a book left the largest free
  // block at ~55 KB: plenty free, no room for it, so the overlay was skipped and the plain screen
  // drawn instead. Everything released here is rebuildable and the RAM is lost to deep sleep
  // anyway, so nothing is paid for it -- the device reboots on wake and reloads what it needs.
  LOG_DBG("SLP", "Overlay PNG, before release: free=%u largest=%u", static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getMaxAllocHeap()));
  if (auto* fcm = renderer.getFontCacheManager()) fcm->releaseAllFontMemory();
  sdFontSystem.releaseAllResidentFonts(renderer);
  DictIndex::releaseCaches();
  LOG_DBG("SLP", "Overlay PNG, after release: free=%u largest=%u", static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getMaxAllocHeap()));

  ImageDimensions dims{};
  if (!PngToFramebufferConverter::getDimensionsStatic(path, dims)) return false;
  if (dims.width <= 0 || dims.height <= 0) return false;

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  const float widthScale = static_cast<float>(pageWidth) / static_cast<float>(dims.width);
  const float heightScale = static_cast<float>(pageHeight) / static_cast<float>(dims.height);
  float scale = std::min(widthScale, heightScale);
  if (scale > 1.0f) scale = 1.0f;

  const int dstWidth = static_cast<int>(dims.width * scale);
  const int dstHeight = static_cast<int>(dims.height * scale);

  RenderConfig config{};
  config.x = (pageWidth - dstWidth) / 2;
  config.y = (pageHeight - dstHeight) / 2;
  config.maxWidth = pageWidth;
  config.maxHeight = pageHeight;
  config.useGrayscale = false;
  config.preserveAlpha = true;

  PngToFramebufferConverter converter;
  LOG_DBG("SLP", "Rendering transparent PNG overlay: %s (%dx%d)", path.c_str(), dims.width, dims.height);

  if (!converter.decodeToFramebuffer(path, renderer, config)) return false;
  // The overlay itself is drawn 1-bit, so it needs no gray passes. Only a grayscale cover under it
  // (Cover + Transparent on an absolute-plane panel) does.
  if (!overlayGrayCover) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    return true;
  }
  const bool absolute = renderer.grayscaleCapabilities(sleepGrayscaleMode(renderer)).supported();
  const BwFrameKeeper bwFrame(renderer, !absolute);
  if (!bwFrame.ready()) {
    renderer.displayBuffer(kSleepClean);
    return true;
  }
  if (absolute) {
    // The cover replaces the previous screen, so clean it like the BMP paths do.
    if (overlayCover) cleanBeforeAbsoluteGray(renderer);
    if (!renderer.displayGrayscaleBase(sleepGrayscaleMode(renderer))) return false;
  } else {
    renderer.displayGrayscaleBase(HalDisplay::HALF_REFRESH);
  }

  // Absolute planes retain B/W background bits; each visible overlay pixel is rewritten in both passes.
  if (!beginOverlayGrayPlane(renderer, overlayGrayCover, GfxRenderer::GRAYSCALE_LSB, absolute) ||
      !converter.decodeToFramebuffer(path, renderer, config)) {
    renderer.setRenderMode(GfxRenderer::BW);
    return true;
  }
  renderer.copyGrayscaleLsbBuffers();

  if (!beginOverlayGrayPlane(renderer, overlayGrayCover, GfxRenderer::GRAYSCALE_MSB, absolute) ||
      !converter.decodeToFramebuffer(path, renderer, config)) {
    renderer.setRenderMode(GfxRenderer::BW);
    return true;
  }
  renderer.copyGrayscaleMsbBuffers();

  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);
  return true;
}

bool SleepActivity::renderSleepOverlayPath(const std::string& path) const {
  if (FsHelpers::hasPngExtension(path)) {
    return Storage.exists(path.c_str()) && renderTransparentOverlayPng(path);
  }

  HalFile file;
  return Storage.openFileForRead("SLP", path, file) && renderSleepOverlayFile(file, path.c_str());
}

void SleepActivity::renderTransparentCustomSleepScreen() const {
  if (renderSleepOverlayPath(TRANSPARENT_SLEEP_ROOT_BMP)) return;
  if (renderSleepOverlayPath(TRANSPARENT_SLEEP_ROOT_PNG)) return;

  std::string selectedPath;
  selectRandomSleepFile(TRANSPARENT_SLEEP_DIRS, std::size(TRANSPARENT_SLEEP_DIRS), SleepRecentKind::Overlay,
                        selectedPath);

  if (!selectedPath.empty() && renderSleepOverlayPath(selectedPath)) return;

  LOG_ERR("SLP", "No valid transparent sleep overlay found");
  if (overlayCover) {
    // Show the plain cover.
    const Bitmap& cover = *overlayCover;
    overlayCover = nullptr;
    if (cover.rewindToData() == BmpReaderError::Ok) return renderBitmapSleepScreen(cover);
  }
  renderDefaultSleepScreen();
}

void SleepActivity::renderCoverSleepScreen() const {
  void (SleepActivity::*renderNoCoverSleepScreen)() const;
  bool withOverlay = false;
  switch (SETTINGS.sleepScreen) {
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM):
      renderNoCoverSleepScreen = &SleepActivity::renderCustomSleepScreen;
      break;
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER_TRANSPARENT):
      renderNoCoverSleepScreen = &SleepActivity::renderTransparentCustomSleepScreen;
      withOverlay = true;
      break;
    default:
      renderNoCoverSleepScreen = &SleepActivity::renderDefaultSleepScreen;
      break;
  }

  if (APP_STATE.openEpubPath.empty()) {
    return (this->*renderNoCoverSleepScreen)();
  }

  // SSD absolute images keep the original dither thresholds; other panels use the newer tuning.
  const bool originalThresholds =
      renderer.grayscaleCapabilities(sleepGrayscaleMode(renderer)).supported() &&
      display.getController() == HalDisplay::Controller::SSD1677 &&
      SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER;
  std::string coverBmpPath;
  bool cropped = SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP;

  // Check if the current book is XTC, TXT, or EPUB
  if (FsHelpers::hasXtcExtension(APP_STATE.openEpubPath)) {
    // Handle XTC file
    Xtc lastXtc(APP_STATE.openEpubPath, "/.crosspoint");
    if (!lastXtc.load()) {
      LOG_ERR("SLP", "Failed to load last XTC");
      return (this->*renderNoCoverSleepScreen)();
    }

    if (!lastXtc.generateCoverBmp()) {
      LOG_ERR("SLP", "Failed to generate XTC cover bmp");
      return (this->*renderNoCoverSleepScreen)();
    }

    coverBmpPath = lastXtc.getCoverBmpPath();
  } else if (FsHelpers::hasReflowableBookExtension(APP_STATE.openEpubPath)) {
    // Handle EPUB, TXT, or Markdown file
    Epub lastEpub(APP_STATE.openEpubPath, "/.crosspoint");
    // Skip loading css since we only need metadata here
    if (!lastEpub.load(true, true)) {
      LOG_ERR("SLP", "Failed to load last book");
      return (this->*renderNoCoverSleepScreen)();
    }

    if (!lastEpub.generateCoverBmp(cropped, originalThresholds)) {
      LOG_ERR("SLP", "Failed to generate cover bmp");
      return (this->*renderNoCoverSleepScreen)();
    }

    coverBmpPath = lastEpub.getCoverBmpPath(cropped, originalThresholds);
  } else {
    return (this->*renderNoCoverSleepScreen)();
  }

  HalFile file;
  if (Storage.openFileForRead("SLP", coverBmpPath, file)) {
    Bitmap bitmap(file);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      LOG_DBG("SLP", "Rendering sleep cover: %s", coverBmpPath.c_str());
      if (withOverlay) overlayCover = &bitmap;
      renderBitmapSleepScreen(bitmap);
      overlayCover = nullptr;
      return;
    }
  }

  return (this->*renderNoCoverSleepScreen)();
}

void SleepActivity::renderLastScreenSleepScreen() const {
  const auto pageHeight = renderer.getScreenHeight();
  renderer.drawImage(MoonIcon, 0, pageHeight - MOONICON_HEIGHT, MOONICON_WIDTH, MOONICON_HEIGHT);
  // Only the moon differs from the displayed frame, so a differential FAST
  // update adds it without the flashing clean pass (which sweeps the panel
  // through the inverse — a full white flash on a night-mode page).
  if (gpio.deviceIsX3()) {
    renderer.displayGrayscaleBase(HalDisplay::FAST_REFRESH);
  } else {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }
}

void SleepActivity::renderBlankSleepScreen() const {
  renderer.clearScreen();
  renderer.displayBuffer(kSleepClean);
}

namespace {

// The Departures screen is drawn light and inverted at the end, like the dark logo screen: what is
// drawn as ink here shows white, so the tiles are ink and their digits and split lines are paper.
constexpr int DEP_MARGIN = 40;
constexpr int DEP_TILES = 4;
constexpr int DEP_LABEL_GAP = 9;
constexpr int DEP_MAX_ROW_STEP = 136;
// Screens shorter than this use the compact tiles.
constexpr int DEP_COMPACT_BELOW = 760;

struct DepartureTiles {
  int tileW;
  int tileH;
  int tileGap;
  int firstRow;
  const DepartureBitmap* digits;
};

constexpr DepartureTiles DEP_REGULAR = {58, 90, 6, 100, DepDigits};
constexpr DepartureTiles DEP_COMPACT = {44, 68, 5, 88, DepDigitsCompact};

struct DepartureCat {
  GrayLogo::Size size;
  int width;
  int height;
};

// Largest first: the screen uses the biggest cat that leaves the rows room.
constexpr DepartureCat DEP_CATS[] = {{GrayLogo::Size::Regular, GrayLogo::WIDTH, GrayLogo::HEIGHT},
                                     {GrayLogo::Size::Small, GrayLogo::SMALL_WIDTH, GrayLogo::SMALL_HEIGHT}};

// Draws one bitmap from the inflated lettering, a horizontal run of ink at a time.
void drawDepartureBitmap(const GfxRenderer& renderer, const uint8_t* type, const DepartureBitmap& bitmap, const int x,
                         const int y, const bool ink) {
  const int rowBytes = (bitmap.width + 7) / 8;
  const uint8_t* row = type + bitmap.offset;
  for (int py = 0; py < bitmap.height; py++, row += rowBytes) {
    int px = 0;
    while (px < bitmap.width) {
      if (!(row[px >> 3] & (0x80 >> (px & 7)))) {
        px++;
        continue;
      }
      const int start = px;
      while (px < bitmap.width && (row[px >> 3] & (0x80 >> (px & 7)))) px++;
      renderer.fillRect(x + start, y + py, px - start, 1, ink);
    }
  }
}

// A row of flap tiles showing value right-aligned, blank tiles in front; capped at 9999.
void drawFlapRow(const GfxRenderer& renderer, const uint8_t* type, const DepartureTiles& layout, const int x,
                 const int y, uint32_t value) {
  if (value > 9999) value = 9999;
  char digits[DEP_TILES + 1];
  snprintf(digits, sizeof(digits), "%4u", static_cast<unsigned>(value));
  for (int t = 0; t < DEP_TILES; t++) {
    const int tx = x + t * (layout.tileW + layout.tileGap);
    renderer.fillRoundedRect(tx, y, layout.tileW, layout.tileH, 5, Color::Black);
    if (digits[t] != ' ') {
      const DepartureBitmap& d = layout.digits[digits[t] - '0'];
      drawDepartureBitmap(renderer, type, d, tx + (layout.tileW - d.width) / 2, y + (layout.tileH - d.height) / 2,
                          false);
    }
    renderer.fillRect(tx, y + layout.tileH / 2 - 1, layout.tileW, 3, false);
  }
}

}  // namespace

void SleepActivity::renderDeparturesSleepScreen() const {
  auto type = makeUniqueNoThrow<uint8_t[]>(DEP_TYPE_INFLATED_SIZE);
  if (!type) {
    LOG_ERR("SLP", "OOM: %d bytes for the Departures lettering", DEP_TYPE_INFLATED_SIZE);
    return renderDefaultSleepScreen();
  }
  {
    InflateReader reader;
    reader.init(false);
    reader.setSource(DepTypeDeflated, sizeof(DepTypeDeflated));
    if (!reader.read(type.get(), DEP_TYPE_INFLATED_SIZE)) {
      LOG_ERR("SLP", "Departures lettering failed to inflate");
      return renderDefaultSleepScreen();
    }
  }

  const StatsWidgets::Today today = StatsWidgets::getToday();
  uint32_t hours = 0;
  int streak = 0;
  uint16_t weekMinutes = 0;
  {
    // The reader has exited by now, so the tables have the heap to themselves.
    const ReadingStatsStore::Loaded stats;
    hours = READING_STATS_STORE.getTotalMinutes() / 60;
    streak = READING_STATS_STORE.getStreak(today.year, today.month, today.day);
    weekMinutes = READING_STATS_STORE.getMinutesThisWeek(today.year, today.month, today.day);
  }

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const DepartureTiles& layout = pageHeight < DEP_COMPACT_BELOW ? DEP_COMPACT : DEP_REGULAR;
  renderer.clearScreen();

  const int headerY = layout.firstRow - 56;
  drawDepartureBitmap(renderer, type.get(), DepWordDepartures, DEP_MARGIN, headerY, true);
  drawDepartureBitmap(renderer, type.get(), DepWordShuppatsu, DEP_MARGIN + DepWordDepartures.width + 14,
                      headerY + DepWordDepartures.height - DepWordShuppatsu.height + 2, true);

  // The cat sits in the bottom-right corner with NEXT / ONE MORE CHAPTER centred on its height. The
  // rows close up to clear that block, and, where the rows reach across into the cat's column, to
  // keep their last label above the cat's top -- down to the point where a label would touch the
  // next row's tiles. The biggest cat that allows that is used; with none the cat is left out.
  const int labelH = DEP_LABEL_GAP + DepWordHoursRead.height;
  const int minRowStep = layout.tileH + labelH + 3;
  const int blockH = DepWordNext.height + 16 + DepWordOneMore.height + 14 + DepWordChapter.height;
  const int rowsRight =
      DEP_MARGIN + std::max<int>(DEP_TILES * (layout.tileW + layout.tileGap), DepWordMinutesThisWeek.width + 2);
  const DepartureCat* cat = nullptr;
  int catX = 0;
  int catY = 0;
  int rowStep = 0;
  for (const DepartureCat& candidate : DEP_CATS) {
    const int x = pageWidth - candidate.width - 10;
    const int top = pageHeight - candidate.height - 10;
    int limit = top + candidate.height / 2 - blockH / 2 - 16;
    if (rowsRight + 8 > x) limit = std::min(limit, top + 8);
    const int step = (limit - layout.firstRow - layout.tileH - labelH) / 2;
    if (step >= minRowStep) {
      cat = &candidate;
      catX = x;
      catY = top;
      rowStep = step;
      break;
    }
  }
  if (!cat) rowStep = (pageHeight - blockH - 60 - layout.firstRow - layout.tileH) / 2;
  rowStep = std::min(rowStep, DEP_MAX_ROW_STEP);

  const DepartureBitmap* labels[] = {&DepWordHoursRead, &DepWordDayStreak, &DepWordMinutesThisWeek};
  const uint32_t values[] = {hours, static_cast<uint32_t>(std::max(streak, 0)), weekMinutes};
  for (int row = 0; row < 3; row++) {
    const int y = layout.firstRow + row * rowStep;
    drawFlapRow(renderer, type.get(), layout, DEP_MARGIN, y, values[row]);
    drawDepartureBitmap(renderer, type.get(), *labels[row], DEP_MARGIN + 2, y + layout.tileH + DEP_LABEL_GAP, true);
  }

  // NEXT / ONE MORE CHAPTER, centred on the cat's height, or at the foot of the screen without it.
  int y = cat ? catY + cat->height / 2 - blockH / 2 : pageHeight - 40 - blockH;
  drawDepartureBitmap(renderer, type.get(), DepWordNext, DEP_MARGIN, y, true);
  y += DepWordNext.height + 16;
  drawDepartureBitmap(renderer, type.get(), DepWordOneMore, DEP_MARGIN, y, true);
  y += DepWordOneMore.height + 14;
  drawDepartureBitmap(renderer, type.get(), DepWordChapter, DEP_MARGIN, y, true);
  type.reset();  // before the small cat inflates its own buffer
  if (cat) GrayLogo::drawBase(renderer, catX, catY, true, cat->size);

  renderer.invertScreen();
  if (!cat) {
    renderer.displayBuffer(kSleepClean);
    return;
  }
  renderer.displayGrayscaleBase(HalDisplay::HALF_REFRESH);
  GrayLogo::flushGrayPasses(renderer, catX, catY, true, cat->size);
}
