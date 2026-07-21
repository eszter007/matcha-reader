#include "ImageDecoderFactory.h"

#include <Logging.h>

#include <memory>
#include <new>
#include <string>

#include "BmpToFramebufferConverter.h"
#include "JpegToFramebufferConverter.h"
#include "PngToFramebufferConverter.h"

std::unique_ptr<JpegToFramebufferConverter> ImageDecoderFactory::jpegDecoder = nullptr;
std::unique_ptr<PngToFramebufferConverter> ImageDecoderFactory::pngDecoder = nullptr;
std::unique_ptr<BmpToFramebufferConverter> ImageDecoderFactory::bmpDecoder = nullptr;

ImageToFramebufferDecoder* ImageDecoderFactory::getDecoder(const std::string& imagePath) {
  std::string ext = imagePath;
  size_t dotPos = ext.rfind('.');
  if (dotPos != std::string::npos) {
    ext = ext.substr(dotPos);
    for (auto& c : ext) {
      c = tolower(c);
    }
  } else {
    ext = "";
  }

  // new (std::nothrow): bare new aborts the firmware on OOM under -fno-exceptions (see CLAUDE.md).
  // A null decoder propagates through get() and every caller already handles a null decoder
  // (renderFullPage/renderPanelZoom show a load error / fall back; prefetch skips).
  if (JpegToFramebufferConverter::supportsFormat(ext)) {
    if (!jpegDecoder) {
      jpegDecoder.reset(new (std::nothrow) JpegToFramebufferConverter());
    }
    return jpegDecoder.get();
  } else if (PngToFramebufferConverter::supportsFormat(ext)) {
    if (!pngDecoder) {
      pngDecoder.reset(new (std::nothrow) PngToFramebufferConverter());
    }
    return pngDecoder.get();
  } else if (BmpToFramebufferConverter::supportsFormat(ext)) {
    if (!bmpDecoder) {
      bmpDecoder.reset(new (std::nothrow) BmpToFramebufferConverter());
    }
    return bmpDecoder.get();
  }

  LOG_ERR("DEC", "No decoder found for image: %s", imagePath.c_str());
  return nullptr;
}

bool ImageDecoderFactory::isFormatSupported(const std::string& imagePath) { return getDecoder(imagePath) != nullptr; }
