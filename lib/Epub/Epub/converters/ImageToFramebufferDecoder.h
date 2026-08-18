#pragma once
#include <HalStorage.h>
#include <stdint.h>

#include <cstdint>
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
  float sourceCropX = 0.0f;         // Fraction cropped equally from the left and right edges
  float sourceCropY = 0.0f;         // Fraction cropped equally from the top and bottom edges
  bool preserveAlpha = false;       // Skip transparent pixels instead of compositing them against white
  std::string cachePath;            // If non-empty, decoder will write pixel cache to this path

  // Aspect-fill ("cover") mode: scale by max(scaleX, scaleY) instead of
  // min(scaleX, scaleY), so the image fills maxWidth x maxHeight completely
  // (may upscale) instead of fitting inside it with letterbox space. Pair
  // with cropWidth/cropHeight to clip the overflowing dimension -- output is
  // top-left anchored, so only the bottom/right get cropped.
  bool fillCrop = false;
  int cropWidth = 0;   // If >0, clip final output width to this value
  int cropHeight = 0;  // If >0, clip final output height to this value

  // Lifts the source tone before dithering (0 = off). The framebuffer path renders through a
  // 4-level Bayer screen, which on the 1-bit framebuffer reads noticeably darker than the
  // Atkinson 1-bit dithering used for the cached BMP cover thumbnails -- the same manga cover
  // looked heavy on the home screen and light in the Library. Lifting the midtones brings the
  // two in line without touching the shared dither routine.
  uint8_t lightenBy = 0;

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

  // Call from per-row/per-MCU decode callbacks (free functions, hence public):
  // yields one tick at most every 250 ms so multi-second decodes keep the idle
  // task (and its watchdog) fed. `lastYieldMs` is caller-held state,
  // initialized to the decode start time.
  static void yieldDuringDecode(uint32_t& lastYieldMs);

  // Validate decoder/header dimensions before narrowing them into the layout
  // representation. Shared by header probing and decoder fallbacks.
  static bool validateAndStoreDimensions(int64_t width, int64_t height, ImageDimensions& out, const char* format);

 protected:
  // Size validation helpers. The cap bounds decode TIME, not memory: both decoders
  // stream (JPEG in MCU bands at 1/2..1/8 coarse scale, PNG scanline-by-scanline
  // with its own width-based row-buffer guard), so RAM never scales with source
  // area. 8 MP admits real-world ebook covers (KDP recommends 1600x2560 and
  // 2000x3000) while keeping a worst-case single decode in single-digit seconds;
  // the row callbacks yield periodically so a long decode cannot starve the idle
  // task's watchdog. Also comfortably covers the on-device manga-scan case that
  // motivated the fork's previous 2048x3072 area cap (e.g. a 1500x2250 page).
  static constexpr int64_t MAX_SOURCE_DIMENSION = INT16_MAX;
  static constexpr int64_t MAX_SOURCE_PIXELS = 8388608;  // 8 MP (e.g. 2048 * 4096)

  void warnUnsupportedFeature(const std::string& feature, const std::string& imagePath);
};
