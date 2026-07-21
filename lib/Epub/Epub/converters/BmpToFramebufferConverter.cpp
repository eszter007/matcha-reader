#include "BmpToFramebufferConverter.h"

#include <Bitmap.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>

bool BmpToFramebufferConverter::supportsFormat(const std::string& extension) {
  return FsHelpers::hasBmpExtension(extension);
}

bool BmpToFramebufferConverter::getDimensionsStatic(const std::string& imagePath, ImageDimensions& out) {
  HalFile file;
  if (!Storage.openFileForRead("BMP", imagePath, file)) return false;
  Bitmap bmp(file);
  if (bmp.parseHeaders() != BmpReaderError::Ok) return false;
  out.width = static_cast<int16_t>(bmp.getWidth());
  out.height = static_cast<int16_t>(bmp.getHeight());
  return out.width > 0 && out.height > 0;
}

bool BmpToFramebufferConverter::isMonochromeStatic(const std::string& imagePath) {
  HalFile file;
  if (!Storage.openFileForRead("BMP", imagePath, file)) return false;
  Bitmap bmp(file);
  if (bmp.parseHeaders() != BmpReaderError::Ok) return false;
  return bmp.is1Bit();
}

bool BmpToFramebufferConverter::decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer,
                                                    const RenderConfig& config) {
  HalFile file;
  if (!Storage.openFileForRead("BMP", imagePath, file)) return false;
  Bitmap bmp(file, config.useDithering);
  if (bmp.parseHeaders() != BmpReaderError::Ok) {
    LOG_ERR("BMP", "Header parse failed: %s", imagePath.c_str());
    return false;
  }
  // One-transaction read of the whole pixel array where it fits, so drawBitmap's row reads become
  // memcpys instead of per-row SD transactions (harmless no-op / row-wise fallback for large
  // images). Must precede the first row read.
  bmp.preload();
  // drawBitmap scales to fit within maxWidth x maxHeight (never upscales), draws at config.x/y,
  // honours the current render mode, and routes 1-bit BMPs through drawBitmap1Bit automatically.
  renderer.drawBitmap(bmp, config.x, config.y, config.maxWidth, config.maxHeight);
  return true;
}
