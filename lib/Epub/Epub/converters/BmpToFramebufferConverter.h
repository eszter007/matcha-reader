#pragma once

#include "ImageToFramebufferDecoder.h"

// Renders Windows BMP images to the framebuffer for the manga reader (full-page BMP pages).
// Delegates the actual pixel work to the shared Bitmap parser + GfxRenderer::drawBitmap, which
// already handles every BMP bit depth, aspect-fit scaling, and the current render mode
// (BW / GRAYSCALE_LSB / GRAYSCALE_MSB), and auto-selects the optimized 1-bit path for monochrome
// BMPs. Unlike the JPEG/PNG converters this does not stream a .2bp pixel cache: BMP is
// uncompressed, so re-decoding a plane is a plain row read rather than an inflate/IDCT, and the
// common case (1-bit line-art manga) renders BW-only in a single pass anyway.
class BmpToFramebufferConverter final : public ImageToFramebufferDecoder {
 public:
  static bool getDimensionsStatic(const std::string& imagePath, ImageDimensions& out);

  bool decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer, const RenderConfig& config) override;

  bool getDimensions(const std::string& imagePath, ImageDimensions& dims) const override {
    return getDimensionsStatic(imagePath, dims);
  }

  static bool supportsFormat(const std::string& extension);
  const char* getFormatName() const override { return "BMP"; }

  // True when the BMP at imagePath is 1-bit monochrome, i.e. renderable with a single BW e-ink
  // pass and no grayscale planes. Cheap header-only probe; false on any read/parse error or a
  // non-1-bit BMP. The manga full-page path uses this to skip the 4-level gray refresh.
  static bool isMonochromeStatic(const std::string& imagePath);
};
