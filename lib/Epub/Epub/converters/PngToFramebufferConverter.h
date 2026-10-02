#pragma once

#include "ImageToFramebufferDecoder.h"

class PngToFramebufferConverter final : public ImageToFramebufferDecoder {
 public:
  static bool getDimensionsStatic(const std::string& imagePath, ImageDimensions& out);

  bool decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer, const RenderConfig& config) override;

  bool getDimensions(const std::string& imagePath, ImageDimensions& dims) const override {
    return getDimensionsStatic(imagePath, dims);
  }

  static bool supportsFormat(const std::string& extension);
  // Whether the heap can hold the decoder object right now. When it cannot, a cache-only decode
  // still succeeds inside a framebuffer loan (GfxRenderer::FrameBufferLoan).
  static bool decoderFitsHeap();
  const char* getFormatName() const override { return "PNG"; }
};