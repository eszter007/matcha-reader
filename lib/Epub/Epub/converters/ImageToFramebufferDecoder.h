#pragma once
#include <HalStorage.h>

#include <memory>
#include <string>

class GfxRenderer;

struct ImageDimensions {
  int16_t width;
  int16_t height;
};

struct RenderConfig {
  int x, y;
  int maxWidth, maxHeight;
  bool useGrayscale = true;
  bool useDithering = true;
  bool performanceMode = false;
  bool useExactDimensions = false;  // If true, use maxWidth/maxHeight as exact output size (no recalculation)
  std::string cachePath;            // If non-empty, decoder will write pixel cache to this path

  // Aspect-fill ("cover") mode: scale by max(scaleX, scaleY) instead of
  // min(scaleX, scaleY), so the image fills maxWidth x maxHeight completely
  // (may upscale) instead of fitting inside it with letterbox space. Pair
  // with cropWidth/cropHeight to clip the overflowing dimension -- output is
  // top-left anchored, so only the bottom/right get cropped.
  bool fillCrop = false;
  int cropWidth = 0;   // If >0, clip final output width to this value
  int cropHeight = 0;  // If >0, clip final output height to this value

  // Treat the source alpha channel as a hard mask (sleep-screen overlays): pixels with
  // alpha < 128 are skipped so the retained framebuffer shows through, and every other
  // pixel is written opaquely -- including white, which the normal BW path leaves
  // untouched. Sources without an alpha channel render fully opaque. PNG only.
  bool alphaMask = false;

  // Background-prefetch mode: stream ONLY the .2bp pixel cache (cachePath, required) to SD and
  // never touch the framebuffer or read any renderer state. Because nothing shared with the
  // render task is accessed, the decode needs no rendering mutex and no framebuffer snapshot --
  // this is what lets the manga prefetch worker run off the render/input tasks. JPEG/PNG only
  // (BMP never streams a cache); decodeToFramebuffer fails fast if cachePath is empty or the
  // cache stream can't start, since the decode would produce nothing.
  bool cacheOnly = false;

  // Cooperative cancellation, polled once per decode block/scanline. Return true to abort: the
  // decode stops within one block and the partial cache file is dropped, so a background warm
  // gets out of the way the moment a real render (or activity teardown) needs the CPU/SD.
  // Plain function pointer + context, not std::function (see CLAUDE.md on closure bloat).
  // NOTE (verified against JPEGDEC @86282979 jpeg.inl DecodeJPEG): an aborted decode still
  // returns success (iErr stays 0 on early exit), so converters must track cancellation
  // themselves and treat an aborted decode as failure -- never finalize a partial cache.
  bool (*shouldCancel)(const void* ctx) = nullptr;
  const void* cancelCtx = nullptr;
};

class ImageToFramebufferDecoder {
 public:
  virtual ~ImageToFramebufferDecoder() = default;

  virtual bool decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer, const RenderConfig& config) = 0;

  virtual bool getDimensions(const std::string& imagePath, ImageDimensions& dims) const = 0;

  virtual const char* getFormatName() const = 0;

 protected:
  // Size validation helpers
  static constexpr int MAX_SOURCE_PIXELS = 3145728;  // 2048 * 1536

  bool validateImageDimensions(int width, int height, const std::string& format);
  void warnUnsupportedFeature(const std::string& feature, const std::string& imagePath);
};
