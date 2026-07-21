#include "ImageDecoderFactory.h"

#include <Logging.h>

#include <cctype>
#include <memory>
#include <new>
#include <string>

#include "BmpToFramebufferConverter.h"
#include "JpegToFramebufferConverter.h"
#include "PngToFramebufferConverter.h"

std::unique_ptr<JpegToFramebufferConverter> ImageDecoderFactory::jpegDecoder = nullptr;
std::unique_ptr<PngToFramebufferConverter> ImageDecoderFactory::pngDecoder = nullptr;
std::unique_ptr<BmpToFramebufferConverter> ImageDecoderFactory::bmpDecoder = nullptr;

namespace {
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

  // new (std::nothrow): bare new aborts the firmware on OOM under -fno-exceptions (see CLAUDE.md).
  // A null decoder propagates through get() and every caller already handles a null decoder
  // (renderFullPage/renderPanelZoom show a load error / fall back; prefetch skips).
  if (JpegToFramebufferConverter::supportsFormat(ext)) {
    if (!jpegDecoder) {
      jpegDecoder.reset(new (std::nothrow) JpegToFramebufferConverter());
      if (!jpegDecoder) LOG_ERR("DEC", "OOM allocating JPEG decoder for: %s", imagePath.c_str());
    }
    return jpegDecoder.get();
  } else if (PngToFramebufferConverter::supportsFormat(ext)) {
    if (!pngDecoder) {
      pngDecoder.reset(new (std::nothrow) PngToFramebufferConverter());
      if (!pngDecoder) LOG_ERR("DEC", "OOM allocating PNG decoder for: %s", imagePath.c_str());
    }
    return pngDecoder.get();
  } else if (BmpToFramebufferConverter::supportsFormat(ext)) {
    if (!bmpDecoder) {
      bmpDecoder.reset(new (std::nothrow) BmpToFramebufferConverter());
      if (!bmpDecoder) LOG_ERR("DEC", "OOM allocating BMP decoder for: %s", imagePath.c_str());
    }
    return bmpDecoder.get();
  }

  LOG_ERR("DEC", "No decoder found for image: %s", imagePath.c_str());
  return nullptr;
}

bool ImageDecoderFactory::isFormatSupported(const std::string& imagePath) {
  // Format support is a static property of the extension -- decide it WITHOUT allocating a decoder.
  // (getDecoder now returns nullptr on OOM via nothrow allocation, so delegating here would wrongly
  // report a supported format as unsupported under memory pressure.)
  const std::string ext = lowerExtension(imagePath);
  return JpegToFramebufferConverter::supportsFormat(ext) || PngToFramebufferConverter::supportsFormat(ext) ||
         BmpToFramebufferConverter::supportsFormat(ext);
}
