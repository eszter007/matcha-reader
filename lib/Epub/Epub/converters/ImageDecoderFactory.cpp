#include "ImageDecoderFactory.h"

#include <Logging.h>

#include <cctype>
#include <string>

#include "BmpToFramebufferConverter.h"
#include "JpegToFramebufferConverter.h"
#include "PngToFramebufferConverter.h"

namespace {
// Shared stateless converter instances. Static-init construction (single-threaded, before any
// task starts) instead of lazy allocation -- see the rationale in ImageDecoderFactory.h. The
// constructors are trivial (no data members), so there is no static-init-order hazard.
JpegToFramebufferConverter jpegDecoder;
PngToFramebufferConverter pngDecoder;
BmpToFramebufferConverter bmpDecoder;

// Lower-cased file extension (including the dot), or "" if none. Shared by getDecoder() and
// isFormatSupported() so both agree on what a "supported format" is.
std::string lowerExtension(const std::string& imagePath) {
  const size_t dotPos = imagePath.rfind('.');
  if (dotPos == std::string::npos) return "";
  std::string ext = imagePath.substr(dotPos);
  for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return ext;
}
}  // namespace

ImageToFramebufferDecoder* ImageDecoderFactory::getDecoder(const std::string& imagePath) {
  const std::string ext = lowerExtension(imagePath);

  if (JpegToFramebufferConverter::supportsFormat(ext)) return &jpegDecoder;
  if (PngToFramebufferConverter::supportsFormat(ext)) return &pngDecoder;
  if (BmpToFramebufferConverter::supportsFormat(ext)) return &bmpDecoder;

  LOG_ERR("DEC", "No decoder found for image: %s", imagePath.c_str());
  return nullptr;
}

bool ImageDecoderFactory::isFormatSupported(const std::string& imagePath) {
  const std::string ext = lowerExtension(imagePath);
  return JpegToFramebufferConverter::supportsFormat(ext) || PngToFramebufferConverter::supportsFormat(ext) ||
         BmpToFramebufferConverter::supportsFormat(ext);
}
