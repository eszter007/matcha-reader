#include "ImageBlock.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Serialization.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "Epub/converters/ImageDecoderFactory.h"
#include "Epub/converters/PixelCache.h"

// Cache file format:
// - uint16_t width
// - uint16_t height
// - uint8_t pixels[...] - 2 bits per pixel, packed (4 pixels per byte), row-major order

ImageBlock::ImageBlock(const std::string& imagePath, int16_t width, int16_t height)
    : imagePath(imagePath), width(width), height(height) {}

bool ImageBlock::imageExists() const { return Storage.exists(imagePath.c_str()); }

void ImageBlock::fitWithin(const int availW, const int availH, int& w, int& h) {
  if (w > availW || h > availH) {
    const float sx = static_cast<float>(availW) / w;
    const float sy = static_cast<float>(availH) / h;
    const float s = (sx < sy) ? sx : sy;
    w = static_cast<int>(w * s + 0.5f);
    h = static_cast<int>(h * s + 0.5f);
  }
  if (w < 1) w = 1;
  if (h < 1) h = 1;
}

ImageBlock::WarmResult ImageBlock::warmCache(GfxRenderer& renderer, bool (*shouldCancel)(const void*),
                                             const void* cancelCtx) const {
  // BMP never streams a pixel cache (the BMP converter rejects cacheOnly) and renders fast
  // without one -- nothing to warm.
  const size_t dotPos = imagePath.rfind('.');
  if (dotPos != std::string::npos) {
    std::string ext = imagePath.substr(dotPos);
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext == ".bmp") return WarmResult::Failed;
  }

  // The dimensions the eventual render will ask renderFromCache for: the stored fit dims, or
  // for a rotated image the same rotated-frame fit render() computes. The rotated render draws
  // upright in the ADJACENT orientation, whose screen box is the current one with W/H swapped.
  int dstW = width;
  int dstH = height;
  if (rotated) {
    const int usableW = std::max(1, renderer.getScreenHeight() - 2 * reserveMargin_);
    const int usableH = std::max(1, renderer.getScreenWidth() - 2 * reserveMargin_);
    fitWithin(usableW, usableH, dstW, dstH);
  }

  const std::string cachePath = PixelCacheIO::pathFor(imagePath);
  {
    HalFile cacheFile;
    if (Storage.openFileForRead("IMG", cachePath, cacheFile)) {
      uint16_t cachedWidth = 0, cachedHeight = 0;
      if (cacheFile.read(&cachedWidth, 2) == 2 && cacheFile.read(&cachedHeight, 2) == 2 &&
          abs(cachedWidth - dstW) <= 1 && abs(cachedHeight - dstH) <= 1) {
        return WarmResult::AlreadyWarm;  // same 1px tolerance as renderFromCache
      }
      // Unreadable header or dimension mismatch (layout changed): the decode below rewrites it.
    }
  }

  if (!Storage.exists(imagePath.c_str())) {
    LOG_ERR("IMG", "Warm: image file not found: %s", imagePath.c_str());
    return WarmResult::Failed;
  }
  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(imagePath);
  if (!decoder) {
    return WarmResult::Failed;  // unsupported extension
  }

  RenderConfig config;
  config.x = 0;
  config.y = 0;
  config.maxWidth = dstW;
  config.maxHeight = dstH;
  config.useGrayscale = true;
  config.useDithering = true;
  config.performanceMode = false;
  config.useExactDimensions = true;
  config.cacheOnly = true;  // stream only the .2bp cache; never touch the framebuffer
  config.cachePath = cachePath;
  config.shouldCancel = shouldCancel;
  config.cancelCtx = cancelCtx;

  LOG_DBG("IMG", "Warming image cache: %s (%dx%d)", cachePath.c_str(), dstW, dstH);
  if (decoder->decodeToFramebuffer(imagePath, renderer, config)) {
    return WarmResult::Warmed;
  }
  // The converter already dropped any partial cache file on both cancel and failure.
  return (shouldCancel && shouldCancel(cancelCtx)) ? WarmResult::Cancelled : WarmResult::Failed;
}

void ImageBlock::render(GfxRenderer& renderer, const int x, const int y) {
  // Rotated images (aspect-mismatched, on a dedicated page): switch the display
  // to the adjacent orientation, scale to fit inside a status-bar-safe inset box,
  // center, render, and restore. width/height here are the natural (unrotated)
  // fit dimensions; after rotation, height maps to screen width and vice versa.
  if (rotated) {
    FontCacheManager* fcm = renderer.getFontCacheManager();
    if (fcm && fcm->isScanning()) return;

    const auto savedOrientation = renderer.getOrientation();
    const auto rotatedOrientation = static_cast<GfxRenderer::Orientation>((savedOrientation + 3) % 4);
    renderer.setOrientation(rotatedOrientation);

    const int rsW = renderer.getScreenWidth();
    const int rsH = renderer.getScreenHeight();
    const int usableW = std::max(1, rsW - 2 * reserveMargin_);
    const int usableH = std::max(1, rsH - 2 * reserveMargin_);

    // The decoder draws the image upright (no content rotation); the display's
    // orientation transform produces the visual 90° rotation. So the decode box
    // must keep the image's NATURAL aspect ratio (width:height), scaled to fit
    // the rotated screen. usableW/usableH are already in the rotated frame.
    int fitW = width;
    int fitH = height;
    fitWithin(usableW, usableH, fitW, fitH);

    const int16_t savedW = width;
    const int16_t savedH = height;
    const bool savedRotated = rotated;
    width = static_cast<int16_t>(fitW);
    height = static_cast<int16_t>(fitH);
    rotated = false;  // avoid recursion; render upright in the rotated orientation
    render(renderer, reserveMargin_ + (usableW - fitW) / 2, reserveMargin_ + (usableH - fitH) / 2);
    width = savedW;
    height = savedH;
    rotated = savedRotated;

    renderer.setOrientation(savedOrientation);
    return;
  }

  // The font-prewarm scan pass only accumulates glyphs; an image contributes
  // none, and its DirectPixelWriter output bypasses the renderer's scan-mode
  // suppression, so it would otherwise do a full (discarded) cache render every
  // page view. Skip it here. The image still draws in the real BW/grayscale
  // passes; on first view this just moves the one-time decode to the BW pass.
  FontCacheManager* fcm = renderer.getFontCacheManager();
  if (fcm && fcm->isScanning()) return;

  LOG_DBG("IMG", "Rendering image at %d,%d: %s (%dx%d)", x, y, imagePath.c_str(), width, height);

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();

  // Bounds check render position using logical screen dimensions
  if (x < 0 || y < 0 || x + width > screenWidth || y + height > screenHeight) {
    LOG_ERR("IMG", "Invalid render position: (%d,%d) size (%dx%d) screen (%dx%d)", x, y, width, height, screenWidth,
            screenHeight);
    return;
  }

  // Tiled grayscale (#2190): skip the whole image when it doesn't touch the
  // active band. The per-pixel writer already clips off-band pixels, but without
  // this each of the ~7 bands per plane re-ran the full cache load / pixel walk
  // and discarded the result — the dominant cost of AA on image pages. The check
  // is orientation-aware and returns true when no strip is active, so the BW
  // pass and non-tiled controllers render the image exactly as before.
  if (!renderer.glyphIntersectsStrip(x, y, x + width - 1, y + height - 1)) {
    return;
  }

  // Try to render from cache first
  std::string cachePath = PixelCacheIO::pathFor(imagePath);
  if (PixelCacheIO::renderFromCache(renderer, cachePath, x, y, width, height)) {
    return;  // Successfully rendered from cache
  }

  // No cache - need to decode the image
  // Check if image file exists
  HalFile file;
  if (!Storage.openFileForRead("IMG", imagePath, file)) {
    LOG_ERR("IMG", "Image file not found: %s", imagePath.c_str());
    return;
  }
  size_t fileSize = file.size();
  file.close();

  if (fileSize == 0) {
    LOG_ERR("IMG", "Image file is empty: %s", imagePath.c_str());
    return;
  }

  LOG_DBG("IMG", "Decoding and caching: %s", imagePath.c_str());

  RenderConfig config;
  config.x = x;
  config.y = y;
  config.maxWidth = width;
  config.maxHeight = height;
  config.useGrayscale = true;
  config.useDithering = true;
  config.performanceMode = false;
  config.useExactDimensions = true;  // Use pre-calculated dimensions to avoid rounding mismatches
  config.cachePath = cachePath;      // Enable caching during decode

  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(imagePath);
  if (!decoder) {
    LOG_ERR("IMG", "No decoder found for image: %s", imagePath.c_str());
    return;
  }

  LOG_DBG("IMG", "Using %s decoder", decoder->getFormatName());

  bool success = decoder->decodeToFramebuffer(imagePath, renderer, config);
  if (!success) {
    LOG_ERR("IMG", "Failed to decode image: %s", imagePath.c_str());
    return;
  }

  LOG_DBG("IMG", "Decode successful");
}

bool ImageBlock::serialize(HalFile& file) {
  serialization::writeString(file, imagePath);
  serialization::writePod(file, width);
  serialization::writePod(file, height);
  serialization::writePod(file, rotated);
  serialization::writePod(file, reserveMargin_);
  return true;
}

std::unique_ptr<ImageBlock> ImageBlock::deserialize(HalFile& file) {
  std::string path;
  serialization::readString(file, path);
  int16_t w, h;
  serialization::readPod(file, w);
  serialization::readPod(file, h);
  bool rot = false;
  int16_t reserve = 0;
  serialization::readPod(file, rot);
  serialization::readPod(file, reserve);
  auto block = std::unique_ptr<ImageBlock>(new ImageBlock(path, w, h));
  block->setRotated(rot, reserve);
  return block;
}
