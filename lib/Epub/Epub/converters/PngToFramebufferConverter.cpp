#include "PngToFramebufferConverter.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <PNGdec.h>

#include <cstdlib>
#include <memory>
#include <new>

#include "DirectPixelWriter.h"
#include "DitherUtils.h"
#include "PixelCache.h"

namespace {

// Context struct passed through PNGdec callbacks to avoid global mutable state.
// The draw callback receives this via pDraw->pUser (set by png.decode()).
// The file I/O callbacks receive the HalFile* via pFile->fHandle (set by pngOpen()).
struct PngContext {
  GfxRenderer* renderer{nullptr};
  const RenderConfig* config{nullptr};
  int screenWidth{0};
  int screenHeight{0};

  // Scaling state
  float scale{1.f};
  int srcWidth{0};
  int srcHeight{0};
  int dstWidth{0};
  int dstHeight{0};
  int lastDstY{-1};  // Track last rendered destination Y to avoid duplicates

  PixelCache cache;
  bool caching{false};

  // Cache-only decode (background prefetch): skip every DirectPixelWriter/renderer access.
  bool cacheOnly{false};
  // Set when config->shouldCancel asked for an abort. PNGdec does surface a callback abort as
  // PNG_QUIT_EARLY, but we track it explicitly anyway (uniform with the JPEG converter, where
  // the library reports success after an abort) so a partial cache can never be finalized.
  bool cancelled{false};

  uint8_t* grayLineBuffer{nullptr};
  uint8_t* alphaLineBuffer{nullptr};  // per-pixel source alpha; only allocated for alphaMask
};

// File I/O callbacks use pFile->fHandle to access the HalFile*,
// avoiding the need for global file state.
void* pngOpenWithHandle(const char* filename, int32_t* size) {
  HalFile* f = new HalFile();
  if (!Storage.openFileForRead("PNG", std::string(filename), *f)) {
    delete f;
    return nullptr;
  }
  *size = f->size();
  return f;
}

void pngCloseWithHandle(void* handle) {
  HalFile* f = reinterpret_cast<HalFile*>(handle);
  if (f) {
    f->close();
    delete f;
  }
}

int32_t pngReadWithHandle(PNGFILE* pFile, uint8_t* pBuf, int32_t len) {
  HalFile* f = reinterpret_cast<HalFile*>(pFile->fHandle);
  if (!f) return 0;
  return f->read(pBuf, len);
}

int32_t pngSeekWithHandle(PNGFILE* pFile, int32_t pos) {
  HalFile* f = reinterpret_cast<HalFile*>(pFile->fHandle);
  if (!f) return -1;
  return f->seek(pos);
}

// The PNG decoder (PNGdec) is ~42 KB due to internal zlib decompression buffers.
// We heap-allocate it on demand rather than using a static instance, so this memory
// is only consumed while actually decoding/querying PNG images. This is critical on
// the ESP32-C3 where total RAM is ~320 KB.
constexpr size_t PNG_DECODER_APPROX_SIZE = 44 * 1024;                          // ~42 KB + overhead
constexpr size_t MIN_FREE_HEAP_FOR_PNG = PNG_DECODER_APPROX_SIZE + 16 * 1024;  // decoder + 16 KB headroom

// PNGdec keeps TWO scanlines in its internal ucPixels buffer (current + previous)
// and each scanline includes a leading filter byte.
// Required storage is therefore approximately: 2 * (pitch + 1) + alignment slack.
// If PNG_MAX_BUFFERED_PIXELS is smaller than this requirement for a given image,
// PNGdec can overrun its internal buffer before our draw callback executes.
int bytesPerPixelFromType(int pixelType) {
  switch (pixelType) {
    case PNG_PIXEL_TRUECOLOR:
      return 3;
    case PNG_PIXEL_GRAY_ALPHA:
      return 2;
    case PNG_PIXEL_TRUECOLOR_ALPHA:
      return 4;
    case PNG_PIXEL_GRAYSCALE:
    case PNG_PIXEL_INDEXED:
    default:
      return 1;
  }
}

int requiredPngInternalBufferBytes(int srcWidth, int pixelType) {
  // +1 filter byte per scanline, *2 for current+previous lines, +32 for alignment margin.
  int pitch = srcWidth * bytesPerPixelFromType(pixelType);
  return ((pitch + 1) * 2) + 32;
}

// Convert entire source line to grayscale with alpha blending to white background.
// For indexed PNGs with tRNS chunk, alpha values are stored at palette[768] onwards.
// Processing the whole line at once improves cache locality and reduces per-pixel overhead.
void convertLineToGray(uint8_t* pPixels, uint8_t* grayLine, int width, int pixelType, uint8_t* palette, int hasAlpha) {
  switch (pixelType) {
    case PNG_PIXEL_GRAYSCALE:
      memcpy(grayLine, pPixels, width);
      break;

    case PNG_PIXEL_TRUECOLOR:
      for (int x = 0; x < width; x++) {
        uint8_t* p = &pPixels[x * 3];
        grayLine[x] = (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
      }
      break;

    case PNG_PIXEL_INDEXED:
      if (palette) {
        if (hasAlpha) {
          for (int x = 0; x < width; x++) {
            uint8_t idx = pPixels[x];
            uint8_t* p = &palette[idx * 3];
            uint8_t gray = (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
            uint8_t alpha = palette[768 + idx];
            grayLine[x] = (uint8_t)((gray * alpha + 255 * (255 - alpha)) / 255);
          }
        } else {
          for (int x = 0; x < width; x++) {
            uint8_t* p = &palette[pPixels[x] * 3];
            grayLine[x] = (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
          }
        }
      } else {
        memcpy(grayLine, pPixels, width);
      }
      break;

    case PNG_PIXEL_GRAY_ALPHA:
      for (int x = 0; x < width; x++) {
        uint8_t gray = pPixels[x * 2];
        uint8_t alpha = pPixels[x * 2 + 1];
        grayLine[x] = (uint8_t)((gray * alpha + 255 * (255 - alpha)) / 255);
      }
      break;

    case PNG_PIXEL_TRUECOLOR_ALPHA:
      for (int x = 0; x < width; x++) {
        uint8_t* p = &pPixels[x * 4];
        uint8_t gray = (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
        uint8_t alpha = p[3];
        grayLine[x] = (uint8_t)((gray * alpha + 255 * (255 - alpha)) / 255);
      }
      break;

    default:
      memset(grayLine, 128, width);
      break;
  }
}

// Extract the per-pixel source alpha for the mask path (no blending -- the mask needs the
// raw coverage decision, not a white-composited gray). Types without alpha are fully opaque.
void extractLineAlpha(uint8_t* pPixels, uint8_t* alphaLine, int width, int pixelType, uint8_t* palette, int hasAlpha) {
  switch (pixelType) {
    case PNG_PIXEL_INDEXED:
      if (palette && hasAlpha) {
        for (int x = 0; x < width; x++) alphaLine[x] = palette[768 + pPixels[x]];
        return;
      }
      break;
    case PNG_PIXEL_GRAY_ALPHA:
      for (int x = 0; x < width; x++) alphaLine[x] = pPixels[x * 2 + 1];
      return;
    case PNG_PIXEL_TRUECOLOR_ALPHA:
      for (int x = 0; x < width; x++) alphaLine[x] = pPixels[x * 4 + 3];
      return;
    default:
      break;
  }
  memset(alphaLine, 255, width);
}

int pngDrawCallback(PNGDRAW* pDraw) {
  PngContext* ctx = reinterpret_cast<PngContext*>(pDraw->pUser);
  if (!ctx || !ctx->config || !ctx->renderer || !ctx->grayLineBuffer) return 0;

  // Cooperative cancel: polled once per scanline. Returning 0 makes PNGdec stop with
  // PNG_QUIT_EARLY (verified in png.inl 1.1.6).
  if (ctx->config->shouldCancel && ctx->config->shouldCancel(ctx->config->cancelCtx)) {
    ctx->cancelled = true;
    return 0;
  }

  int srcY = pDraw->y;
  int srcWidth = ctx->srcWidth;

  // Calculate destination Y with scaling
  int dstY = (int)(srcY * ctx->scale);

  // Skip if we already rendered this destination row (multiple source rows map to same dest)
  if (dstY == ctx->lastDstY) return 1;
  ctx->lastDstY = dstY;

  // Check bounds
  if (dstY >= ctx->dstHeight) return 1;
  if (ctx->config->cropHeight > 0 && dstY >= ctx->config->cropHeight) return 1;

  int outY = ctx->config->y + dstY;
  if (outY >= ctx->screenHeight) return 1;

  // Convert entire source line to grayscale (improves cache locality)
  convertLineToGray(pDraw->pPixels, ctx->grayLineBuffer, srcWidth, pDraw->iPixelType, pDraw->pPalette,
                    pDraw->iHasAlpha);
  const bool alphaMask = ctx->config->alphaMask && ctx->alphaLineBuffer != nullptr;
  if (alphaMask) {
    extractLineAlpha(pDraw->pPixels, ctx->alphaLineBuffer, srcWidth, pDraw->iPixelType, pDraw->pPalette,
                     pDraw->iHasAlpha);
  }

  // Render scaled row using Bresenham-style integer stepping (no floating-point division).
  // dstWidth is also the ratio denominator below (error >= dstWidth) -- must stay the true,
  // uncropped width. loopWidth is the (possibly smaller) crop bound for the pixel-write loop.
  int dstWidth = ctx->dstWidth;
  int loopWidth = (ctx->config->cropWidth > 0 && ctx->config->cropWidth < dstWidth) ? ctx->config->cropWidth : dstWidth;
  int outXBase = ctx->config->x;
  int screenWidth = ctx->screenWidth;
  bool useDithering = ctx->config->useDithering;
  bool caching = ctx->caching;
  // Cache-only prefetch never touches the framebuffer: pw stays uninitialized (init reads
  // renderer state, which a lock-free background decode must not do), so every pw call below is
  // guarded. Constant per decode -- perfectly predicted, same cost class as `if (caching)`.
  const bool cacheOnly = ctx->cacheOnly;

  // Pre-compute orientation and render-mode state once per row
  DirectPixelWriter pw{};  // value-init: cacheOnly leaves it untouched (all uses below are guarded)
  if (!cacheOnly) {
    pw.init(*ctx->renderer);
    pw.beginRow(outY);
  }

  // The cache streams to disk one row at a time. Flushing rows below this one
  // (PNGdec delivers scanlines top to bottom) repositions the single-row band.
  // A flush failure stops caching for the rest of the decode so we never write
  // past the band buffer; finalize() then drops the partial file.
  DirectCacheWriter cw;
  if (caching) {
    if (!ctx->cache.advanceTo(dstY)) {
      caching = false;
      ctx->caching = false;
    } else {
      cw.init(ctx->cache.buffer, ctx->cache.bytesPerRow, ctx->cache.bandRows, ctx->cache.originX);
      cw.beginRow(outY, ctx->config->y + ctx->cache.bandStart);
    }
  }

  int srcX = 0;
  int error = 0;

  for (int dstX = 0; dstX < loopWidth; dstX++) {
    int outX = outXBase + dstX;
    // Alpha-masked pixels are dropped entirely (the retained framebuffer shows through);
    // everything else is written opaquely, including white -- see RenderConfig::alphaMask.
    if (outX < screenWidth && !(alphaMask && ctx->alphaLineBuffer[srcX] < 128)) {
      uint8_t gray = ctx->grayLineBuffer[srcX];

      uint8_t ditheredGray;
      if (useDithering) {
        ditheredGray = applyBayerDither4Level(gray, outX, outY);
      } else {
        ditheredGray = gray / 85;
        if (ditheredGray > 3) ditheredGray = 3;
      }
      if (!cacheOnly) {
        if (alphaMask) {
          pw.writePixelOpaque(outX, ditheredGray);
        } else {
          pw.writePixel(outX, ditheredGray);
        }
      }
      if (caching) cw.writePixel(outX, ditheredGray);
    }

    // Bresenham-style stepping: advance srcX based on ratio srcWidth/dstWidth
    error += srcWidth;
    while (error >= dstWidth) {
      error -= dstWidth;
      srcX++;
    }
  }

  return 1;
}

}  // namespace

bool PngToFramebufferConverter::getDimensionsStatic(const std::string& imagePath, ImageDimensions& out) {
  size_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < MIN_FREE_HEAP_FOR_PNG) {
    LOG_ERR("PNG", "Not enough heap for PNG decoder (%u free, need %u)", freeHeap, MIN_FREE_HEAP_FOR_PNG);
    return false;
  }

  std::unique_ptr<PNG> png(new (std::nothrow) PNG());
  if (!png) {
    LOG_ERR("PNG", "Failed to allocate PNG decoder for dimensions");
    return false;
  }

  int rc = png->open(imagePath.c_str(), pngOpenWithHandle, pngCloseWithHandle, pngReadWithHandle, pngSeekWithHandle,
                     nullptr);
  const ScopedCleanup cleanup{[&png]() { png->close(); }};

  if (rc != 0) {
    LOG_ERR("PNG", "Failed to open PNG for dimensions: %d", rc);
    return false;
  }

  out.width = png->getWidth();
  out.height = png->getHeight();

  return true;
}

bool PngToFramebufferConverter::decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer,
                                                    const RenderConfig& config) {
  LOG_DBG("PNG", "Decoding PNG: %s", imagePath.c_str());

  size_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < MIN_FREE_HEAP_FOR_PNG) {
    LOG_ERR("PNG", "Not enough heap for PNG decoder (%u free, need %u)", freeHeap, MIN_FREE_HEAP_FOR_PNG);
    return false;
  }

  // Heap-allocate PNG decoder (~42 KB) - freed at end of function
  std::unique_ptr<PNG> png(new (std::nothrow) PNG());
  if (!png) {
    LOG_ERR("PNG", "Failed to allocate PNG decoder");
    return false;
  }

  PngContext ctx;
  ctx.renderer = &renderer;
  ctx.config = &config;
  ctx.cacheOnly = config.cacheOnly;
  if (ctx.cacheOnly) {
    // Cache-only decode produces nothing without a cache stream, and it must not read renderer
    // state (it runs without the rendering mutex): screen bounds are substituted with exact
    // no-clip values once the destination size is known, further down.
    if (config.cachePath.empty()) {
      LOG_ERR("PNG", "cacheOnly decode without cachePath: %s", imagePath.c_str());
      return false;
    }
  } else {
    ctx.screenWidth = renderer.getScreenWidth();
    ctx.screenHeight = renderer.getScreenHeight();
  }

  int rc = png->open(imagePath.c_str(), pngOpenWithHandle, pngCloseWithHandle, pngReadWithHandle, pngSeekWithHandle,
                     pngDrawCallback);
  const ScopedCleanup cleanup{[&png]() { png->close(); }};
  if (rc != PNG_SUCCESS) {
    LOG_ERR("PNG", "Failed to open PNG: %d", rc);
    return false;
  }

  if (!validateImageDimensions(png->getWidth(), png->getHeight(), "PNG")) {
    return false;
  }

  // Calculate output dimensions
  ctx.srcWidth = png->getWidth();
  ctx.srcHeight = png->getHeight();

  if (config.useExactDimensions && config.maxWidth > 0 && config.maxHeight > 0) {
    // Use exact dimensions as specified (avoids rounding mismatches with pre-calculated sizes)
    ctx.dstWidth = config.maxWidth;
    ctx.dstHeight = config.maxHeight;
    ctx.scale = (float)ctx.dstWidth / ctx.srcWidth;
  } else if (config.fillCrop && config.maxWidth > 0 && config.maxHeight > 0) {
    // Aspect-fill: scale by whichever axis needs LESS shrinkage (may upscale),
    // so the box is fully covered and the other axis overflows for cropping.
    float scaleX = (float)config.maxWidth / ctx.srcWidth;
    float scaleY = (float)config.maxHeight / ctx.srcHeight;
    ctx.scale = (scaleX > scaleY) ? scaleX : scaleY;

    ctx.dstWidth = (int)(ctx.srcWidth * ctx.scale + 0.5f);
    ctx.dstHeight = (int)(ctx.srcHeight * ctx.scale + 0.5f);
  } else {
    // Calculate scale factor to fit within maxWidth/maxHeight
    float scaleX = (float)config.maxWidth / ctx.srcWidth;
    float scaleY = (float)config.maxHeight / ctx.srcHeight;
    ctx.scale = (scaleX < scaleY) ? scaleX : scaleY;
    if (ctx.scale > 1.0f) ctx.scale = 1.0f;  // Don't upscale

    ctx.dstWidth = (int)(ctx.srcWidth * ctx.scale);
    ctx.dstHeight = (int)(ctx.srcHeight * ctx.scale);
  }
  ctx.lastDstY = -1;  // Reset row tracking
  if (ctx.cacheOnly) {
    // Exact no-clip screen bounds so the full dstWidth x dstHeight lands in the cache -- same
    // rationale as the JPEG converter (manga geometry is always on-screen anyway).
    ctx.screenWidth = config.x + ctx.dstWidth;
    ctx.screenHeight = config.y + ctx.dstHeight;
  }

  LOG_DBG("PNG", "PNG %dx%d -> %dx%d (scale %.2f), bpp: %d", ctx.srcWidth, ctx.srcHeight, ctx.dstWidth, ctx.dstHeight,
          ctx.scale, png->getBpp());

  const int pixelType = png->getPixelType();
  const int requiredInternal = requiredPngInternalBufferBytes(ctx.srcWidth, pixelType);
  if (requiredInternal > PNG_MAX_BUFFERED_PIXELS) {
    LOG_ERR("PNG",
            "PNG row buffer too small: need %d bytes for width=%d type=%d, configured PNG_MAX_BUFFERED_PIXELS=%d",
            requiredInternal, ctx.srcWidth, pixelType, PNG_MAX_BUFFERED_PIXELS);
    LOG_ERR("PNG", "Aborting decode to avoid PNGdec internal buffer overflow");
    return false;
  }

  if (png->getBpp() != 8) {
    warnUnsupportedFeature("bit depth (" + std::to_string(png->getBpp()) + "bpp)", imagePath);
  }

  // Allocate grayscale line buffer on demand (~3.2 KB) - freed after decode
  const size_t grayBufSize = PNG_MAX_BUFFERED_PIXELS / 2;
  ctx.grayLineBuffer = static_cast<uint8_t*>(malloc(grayBufSize));
  if (!ctx.grayLineBuffer) {
    LOG_ERR("PNG", "Failed to allocate gray line buffer");
    return false;
  }
  if (config.alphaMask) {
    ctx.alphaLineBuffer = static_cast<uint8_t*>(malloc(grayBufSize));
    if (!ctx.alphaLineBuffer) {
      LOG_ERR("PNG", "Failed to allocate alpha line buffer");
      free(ctx.grayLineBuffer);
      ctx.grayLineBuffer = nullptr;
      return false;
    }
  }

  // Stream the pixel cache to disk. PNGdec delivers source scanlines top to
  // bottom and we emit at most one (downscaled) output row per callback, so the
  // band only needs a single row. Streaming keeps the working set tiny, so
  // unlike the old full-image buffer it neither competes with the ~44KB decoder
  // nor forces larger images to skip caching - which previously meant a full
  // re-decode on every one of an image page's ~14 render passes.
  ctx.caching = !config.cachePath.empty();
  if (ctx.caching) {
    if (!ctx.cache.begin(config.cachePath, ctx.dstWidth, ctx.dstHeight, config.x, config.y, 1)) {
      if (ctx.cacheOnly) {
        // The cache IS the output here -- decoding without it would be pure wasted work.
        LOG_ERR("PNG", "cacheOnly: cache stream failed to start, skipping decode");
        free(ctx.grayLineBuffer);
        ctx.grayLineBuffer = nullptr;
        free(ctx.alphaLineBuffer);
        ctx.alphaLineBuffer = nullptr;
        return false;
      }
      LOG_ERR("PNG", "Failed to start cache stream, continuing without caching");
      ctx.caching = false;
    }
  }

  unsigned long decodeStart = millis();
  rc = png->decode(&ctx, 0);
  unsigned long decodeTime = millis() - decodeStart;

  free(ctx.grayLineBuffer);
  ctx.grayLineBuffer = nullptr;
  free(ctx.alphaLineBuffer);
  ctx.alphaLineBuffer = nullptr;

  // A cancelled decode surfaces as PNG_QUIT_EARLY; ctx.cancelled double-checks it. Either way the
  // image is incomplete: drop the partial cache and report failure.
  if (rc != PNG_SUCCESS || ctx.cancelled) {
    if (ctx.cancelled) {
      LOG_DBG("PNG", "Decode cancelled: %s", imagePath.c_str());
    } else {
      LOG_ERR("PNG", "Decode failed: %d", rc);
    }
    if (ctx.caching) ctx.cache.abort();
    return false;
  }

  LOG_DBG("PNG", "PNG decoding complete - render time: %lu ms", decodeTime);

  // Finalize the streamed cache (caching may have been cleared on a flush error).
  if (ctx.caching) {
    ctx.cache.finalize();
  }

  return true;
}

bool PngToFramebufferConverter::supportsFormat(const std::string& extension) {
  return FsHelpers::hasPngExtension(extension);
}
