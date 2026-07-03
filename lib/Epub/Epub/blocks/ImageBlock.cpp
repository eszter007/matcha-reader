#include "ImageBlock.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Serialization.h>

#include "Epub/converters/ImageDecoderFactory.h"
#include "Epub/converters/PixelCache.h"

// Cache file format:
// - uint16_t width
// - uint16_t height
// - uint8_t pixels[...] - 2 bits per pixel, packed (4 pixels per byte), row-major order

ImageBlock::ImageBlock(const std::string& imagePath, int16_t width, int16_t height)
    : imagePath(imagePath), width(width), height(height) {}

bool ImageBlock::imageExists() const { return Storage.exists(imagePath.c_str()); }

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
    if (fitW > usableW || fitH > usableH) {
      const float sx = static_cast<float>(usableW) / fitW;
      const float sy = static_cast<float>(usableH) / fitH;
      const float s = (sx < sy) ? sx : sy;
      fitW = static_cast<int>(fitW * s + 0.5f);
      fitH = static_cast<int>(fitH * s + 0.5f);
    }
    if (fitW < 1) fitW = 1;
    if (fitH < 1) fitH = 1;

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
