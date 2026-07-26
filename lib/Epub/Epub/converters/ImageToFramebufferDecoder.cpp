#include "ImageToFramebufferDecoder.h"

#include <Logging.h>

#include <cstdint>

bool ImageToFramebufferDecoder::validateImageDimensions(int width, int height, const std::string& format) {
  // 64-bit product: decoder headers can claim up to 65535 per axis, and 65535*65535 overflows a
  // 32-bit int -- a malformed file could wrap the product negative and slip past the cap.
  const int64_t pixels = static_cast<int64_t>(width) * height;
  if (pixels > MAX_SOURCE_PIXELS) {
    LOG_ERR("IMG", "Image too large (%dx%d = %lld pixels %s), max supported: %d pixels", width, height,
            static_cast<long long>(pixels), format.c_str(), MAX_SOURCE_PIXELS);
    return false;
  }
  return true;
}

void ImageToFramebufferDecoder::warnUnsupportedFeature(const std::string& feature, const std::string& imagePath) {
  LOG_ERR("IMG", "Warning: Unsupported feature '%s' in image '%s'. Image may not display correctly.", feature.c_str(),
          imagePath.c_str());
}
