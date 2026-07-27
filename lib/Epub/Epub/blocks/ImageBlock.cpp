#include "ImageBlock.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>

#include <cstdlib>
#include <cstring>
#include <new>

#include "Epub/converters/DirectPixelWriter.h"
#include "Epub/converters/ImageDecoderFactory.h"

// Cache file format:
// - uint16_t width
// - uint16_t height
// - uint8_t pixels[...] - 2 bits per pixel, packed (4 pixels per byte), row-major order

ImageBlock::ImageBlock(const std::string& imagePath, const std::string& srcPath, int16_t width, int16_t height)
    : imagePath(imagePath), srcPath(srcPath), width(width), height(height) {}

void* ImageBlock::extractCtx = nullptr;
ImageBlock::ExtractFn ImageBlock::extractFn = nullptr;

void ImageBlock::setExtractor(void* ctx, ExtractFn fn) {
  extractCtx = ctx;
  extractFn = fn;
}

bool ImageBlock::imageExists() const { return Storage.exists(imagePath.c_str()); }

namespace {

std::string getCachePath(const std::string& imagePath) {
  // Replace extension with .pxc (pixel cache)
  size_t dotPos = imagePath.rfind('.');
  if (dotPos != std::string::npos) {
    return imagePath.substr(0, dotPos) + ".pxc";
  }
  return imagePath + ".pxc";
}

bool readValidCacheHeader(HalFile& cacheFile, const int expectedWidth, const int expectedHeight, uint16_t& cachedWidth,
                          uint16_t& cachedHeight) {
  if (cacheFile.read(&cachedWidth, 2) != 2 || cacheFile.read(&cachedHeight, 2) != 2) {
    return false;
  }

  const int widthDiff = abs(cachedWidth - expectedWidth);
  const int heightDiff = abs(cachedHeight - expectedHeight);
  if (widthDiff > 1 || heightDiff > 1) {
    return false;
  }

  const size_t bytesPerRow = (cachedWidth + 3) / 4;
  const size_t expectedSize = 4 + bytesPerRow * cachedHeight;
  return cacheFile.size() >= expectedSize;
}

// Pages are deserialized afresh on each visit. Keep a bounded, allocation-free
// record so an image that failed renders its placeholder directly for the rest
// of the reader session instead of paying another placeholder refresh and
// decode. The reader clears this on entry so transient memory/storage failures
// are retried.
constexpr size_t MAX_SESSION_IMAGE_FAILURES = 16;
uint64_t failedImageHashes[MAX_SESSION_IMAGE_FAILURES];
size_t failedImageCount = 0;

uint64_t imagePathHash(const std::string& path) {
  uint64_t hash = 14695981039346656037ull;
  for (const char c : path) {
    hash ^= static_cast<uint8_t>(c);
    hash *= 1099511628211ull;
  }
  return hash;
}

bool imageFailedThisSession(const std::string& path) {
  const uint64_t hash = imagePathHash(path);
  for (size_t i = 0; i < failedImageCount; i++) {
    if (failedImageHashes[i] == hash) return true;
  }
  return false;
}

void rememberImageFailure(const std::string& path) {
  if (failedImageCount == MAX_SESSION_IMAGE_FAILURES || imageFailedThisSession(path)) return;
  failedImageHashes[failedImageCount++] = imagePathHash(path);
}

// --- Per-page-render RAM slot for the pixel cache ----------------------------
// The tiled grayscale flow re-renders an image page once for the BW
// double-refresh and again for every band of both gray planes, and each pass
// re-read the whole .pxc off SD (~100 ms for a full-page image, ~13 passes).
// Column clipping cannot reduce the SD traffic: the row stride (~100 B) is
// smaller than an SD sector, so every sector is touched regardless of the band
// window. Instead the first pass loads the payload into RAM and later passes
// render from it. Chunked allocation because a single full-image block (up to
// 96 KB) rarely fits the fragmented mid-render heap; each chunk is heap-gated
// and any failure falls back to the streaming path unchanged. The reader
// releases the slot when the page render completes, so nothing stays resident
// across page turns.
constexpr size_t PXC_CHUNK_SHIFT = 14;  // 16 KB chunks
constexpr size_t PXC_CHUNK_SIZE = 1u << PXC_CHUNK_SHIFT;
constexpr size_t PXC_MAX_CHUNKS = 6;  // 96 KB: a full-screen 2bpp image
// Headroom left free after the payload. 24KB was too conservative to ever help the case it exists
// for: a warm rotated image page measured free=79980 against a 63126-byte payload, declining by
// 7722 bytes and then re-streaming that payload 13 times (~110ms each, ~1.4s of a 4887ms page).
// 16KB still leaves a real margin, and every failure here is graceful -- a declined or partially
// allocated slot falls back to the streaming path, which is exactly today's behaviour.
constexpr size_t PXC_HEAP_RESERVE = 16 * 1024;
constexpr size_t PXC_MAX_ALLOC_RESERVE = 8 * 1024;
// Rows can straddle a chunk boundary; they are reassembled into a stack
// buffer. (screenWidth + 3) / 4 caps at 200 B for an 800px panel.
constexpr int PXC_MAX_BYTES_PER_ROW = 208;

std::unique_ptr<uint8_t[]> pxcChunks[PXC_MAX_CHUNKS];
uint64_t pxcSlotHash = 0;
uint16_t pxcSlotWidth = 0;
uint16_t pxcSlotHeight = 0;
// Rows of the payload actually held in RAM. May be fewer than pxcSlotHeight: the slot keeps
// whatever chunks fit and the caller streams the tail. All-or-nothing was measured discarding two
// successfully allocated 16KB chunks because the third was refused, paying the allocation cost and
// then re-reading the whole payload anyway.
uint16_t pxcSlotRows = 0;

void releasePxcSlot() {
  for (auto& chunk : pxcChunks) chunk.reset();
  pxcSlotHash = 0;
  pxcSlotWidth = 0;
  pxcSlotHeight = 0;
  pxcSlotRows = 0;
}

const uint8_t* pxcRowPtr(size_t rowStart, int bytesPerRow, uint8_t* tempRow) {
  const size_t chunk = rowStart >> PXC_CHUNK_SHIFT;
  const size_t offset = rowStart & (PXC_CHUNK_SIZE - 1);
  if (offset + bytesPerRow <= PXC_CHUNK_SIZE) {
    return pxcChunks[chunk].get() + offset;
  }
  const size_t firstPart = PXC_CHUNK_SIZE - offset;
  memcpy(tempRow, pxcChunks[chunk].get() + offset, firstPart);
  memcpy(tempRow + firstPart, pxcChunks[chunk + 1].get(), bytesPerRow - firstPart);
  return tempRow;
}

// cacheFile is positioned just past the header. True when the slot holds the
// full pixel payload for this cache path afterward.
// cacheFile is positioned just past the header. Returns how many leading rows ended up in RAM
// (0 = nothing). A short result is normal and useful: those rows come from RAM and the caller
// streams the remainder, which still removes that fraction of the SD traffic on every later pass.
int loadPxcSlot(uint64_t cacheHash, HalFile& cacheFile, uint16_t cachedWidth, uint16_t cachedHeight, int bytesPerRow) {
  releasePxcSlot();
  if (bytesPerRow > PXC_MAX_BYTES_PER_ROW) {
    return 0;
  }
  size_t remaining = (size_t)bytesPerRow * cachedHeight;
  const size_t chunkCount = (remaining + PXC_CHUNK_SIZE - 1) >> PXC_CHUNK_SHIFT;
  if (chunkCount == 0 || chunkCount > PXC_MAX_CHUNKS) {
    return 0;
  }
  size_t resident = 0;
  for (size_t i = 0; i < chunkCount; i++) {
    const size_t want = remaining < PXC_CHUNK_SIZE ? remaining : PXC_CHUNK_SIZE;
    // Gate on THIS chunk, not the whole payload: the point of chunking is that a partial hold is
    // still worth having. maxAlloc is the binding constraint in practice -- measured declining at
    // maxAlloc=20468 for a 16KB chunk, i.e. the chunk fit and only the margin did not.
    if (ESP.getFreeHeap() < want + PXC_HEAP_RESERVE || ESP.getMaxAllocHeap() < want + PXC_MAX_ALLOC_RESERVE) {
      break;
    }
    pxcChunks[i] = makeUniqueNoThrow<uint8_t[]>(want);
    if (!pxcChunks[i]) break;
    if (cacheFile.read(pxcChunks[i].get(), want) != static_cast<int>(want)) {
      pxcChunks[i].reset();  // short read: this chunk holds nothing trustworthy
      break;
    }
    resident += want;
    remaining -= want;
  }

  const int rows = static_cast<int>(resident / bytesPerRow);  // a straddling tail row is streamed
  if (rows <= 0) {
    releasePxcSlot();
    return 0;
  }
  pxcSlotHash = cacheHash;
  pxcSlotWidth = cachedWidth;
  pxcSlotHeight = cachedHeight;
  pxcSlotRows = static_cast<uint16_t>(rows);
  LOG_DBG("IMG", "PXC slot: %d/%u rows in RAM (%u bytes), free now %u, maxAlloc %u", rows, (unsigned)cachedHeight,
          (unsigned)resident, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  return rows;
}

void renderRowsFromPxcSlot(GfxRenderer& renderer, int x, int y) {
  const int bytesPerRow = (pxcSlotWidth + 3) / 4;
  uint8_t tempRow[PXC_MAX_BYTES_PER_ROW];

  DirectPixelWriter pw;
  pw.init(renderer);

  for (int row = 0; row < pxcSlotRows; row++) {
    const uint8_t* rowBuffer = pxcRowPtr((size_t)row * bytesPerRow, bytesPerRow, tempRow);
    pw.beginRow(y + row);
    int colStart, colEnd;
    pw.bandColRange(x, pxcSlotWidth, colStart, colEnd);
    for (int col = colStart; col < colEnd; col++) {
      const int byteIdx = col >> 2;            // col / 4
      const int bitShift = 6 - (col & 3) * 2;  // MSB first within byte
      const uint8_t pixelValue = (rowBuffer[byteIdx] >> bitShift) & 0x03;
      pw.writePixel(x + col, pixelValue);
    }
  }
}

bool renderFromCache(GfxRenderer& renderer, const std::string& cachePath, int x, int y, int expectedWidth,
                     int expectedHeight) {
  // A later pass of the same page render: the payload is already in RAM, skip
  // the file entirely.
  const uint64_t cacheHash = imagePathHash(cachePath);
  if (pxcSlotHash == cacheHash && pxcSlotWidth != 0 && pxcSlotRows == pxcSlotHeight) {
    renderRowsFromPxcSlot(renderer, x, y);
    return true;
  }

  HalFile cacheFile;
  if (!Storage.openFileForRead("IMG", cachePath, cacheFile)) {
    return false;
  }

  uint16_t cachedWidth, cachedHeight;
  if (!readValidCacheHeader(cacheFile, expectedWidth, expectedHeight, cachedWidth, cachedHeight)) {
    LOG_ERR("IMG", "Invalid image cache: %s", cachePath.c_str());
    return false;
  }

  // Use cached dimensions for rendering (they're the actual decoded size)
  expectedWidth = cachedWidth;
  expectedHeight = cachedHeight;

  LOG_DBG("IMG", "Loading from cache: %s (%dx%d)", cachePath.c_str(), cachedWidth, cachedHeight);

  const int bytesPerRow = (cachedWidth + 3) / 4;  // 2 bits per pixel, 4 pixels per byte

  // First pass of a page render: try to pull the payload into the RAM slot so
  // the remaining ~12 passes skip SD entirely. Only an EMPTY slot is claimed:
  // the slot lives until the page render completes, so a populated slot with a
  // different hash means another image on this same page owns it. Evicting it
  // here would make 2+ image pages reload each other from SD on every pass
  // (all the SD traffic of streaming plus the slot alloc churn); instead later
  // images take the streaming path below, unchanged from pre-cache behavior.
  int fromRow = 0;
  if (pxcSlotHash == cacheHash && pxcSlotWidth != 0) {
    // A partial slot filled by an earlier pass of this same page render.
    renderRowsFromPxcSlot(renderer, x, y);
    fromRow = pxcSlotRows;
  } else if (pxcSlotHash == 0) {
    fromRow = loadPxcSlot(cacheHash, cacheFile, cachedWidth, cachedHeight, bytesPerRow);
    if (fromRow > 0) {
      renderRowsFromPxcSlot(renderer, x, y);
      if (fromRow >= cachedHeight) {
        LOG_DBG("IMG", "Cache render complete (payload now in RAM)");
        return true;
      }
    }
  }

  // Stream whatever the slot does not hold. Seek explicitly rather than trusting the file
  // position: a partial load stops on a chunk boundary, which generally falls mid-row, and that
  // straddling row is streamed rather than half-drawn from RAM.
  cacheFile.seek(4 + (size_t)fromRow * bytesPerRow);

  // Read several rows per SD access. A one-row-per-read loop here means
  // cachedHeight (~728) tiny reads through the storage mutex + SdFat; batching
  // rows into a ~4KB buffer cuts that to ~20 reads per pass without holding the
  // whole image.
  int rowsPerRead = 4096 / bytesPerRow;
  if (rowsPerRead < 1) rowsPerRead = 1;
  if (rowsPerRead > cachedHeight) rowsPerRead = cachedHeight;
  uint8_t* readBuffer = (uint8_t*)malloc((size_t)rowsPerRead * bytesPerRow);
  if (!readBuffer) {
    // Fall back to a single-row buffer under memory pressure.
    rowsPerRead = 1;
    readBuffer = (uint8_t*)malloc(bytesPerRow);
  }
  if (!readBuffer) {
    LOG_ERR("IMG", "Failed to allocate row buffer");
    return false;
  }

  DirectPixelWriter pw;
  pw.init(renderer);

  int rowsInBuffer = 0;
  int bufferRow = 0;
  for (int row = fromRow; row < cachedHeight; row++) {
    if (bufferRow >= rowsInBuffer) {
      const int toRead = (cachedHeight - row < rowsPerRead) ? (cachedHeight - row) : rowsPerRead;
      const size_t bytes = (size_t)toRead * bytesPerRow;
      if (cacheFile.read(readBuffer, bytes) != static_cast<int>(bytes)) {
        LOG_ERR("IMG", "Cache read error at row %d", row);
        free(readBuffer);
        return false;
      }
      rowsInBuffer = toRead;
      bufferRow = 0;
    }

    const uint8_t* rowBuffer = readBuffer + (size_t)bufferRow * bytesPerRow;
    bufferRow++;

    const int destY = y + row;
    pw.beginRow(destY);
    // On a grayscale strip pass only a narrow column window of the image is in
    // the active band; skip the rest instead of unpacking+clipping every pixel.
    int colStart, colEnd;
    pw.bandColRange(x, cachedWidth, colStart, colEnd);
    for (int col = colStart; col < colEnd; col++) {
      const int byteIdx = col >> 2;            // col / 4
      const int bitShift = 6 - (col & 3) * 2;  // MSB first within byte
      uint8_t pixelValue = (rowBuffer[byteIdx] >> bitShift) & 0x03;

      pw.writePixel(x + col, pixelValue);
    }
  }

  free(readBuffer);
  LOG_DBG("IMG", "Cache render complete");
  return true;
}

}  // namespace

bool ImageBlock::hasValidCache() const {
  const auto cachePath = getCachePath(imagePath);
  HalFile cacheFile;
  if (!Storage.openFileForRead("IMG", cachePath, cacheFile)) {
    return false;
  }

  uint16_t cachedWidth, cachedHeight;
  return readValidCacheHeader(cacheFile, width, height, cachedWidth, cachedHeight);
}

bool ImageBlock::needsDecode() const { return !imageFailedThisSession(imagePath) && !hasValidCache(); }

void ImageBlock::clearSessionRenderFailures() { failedImageCount = 0; }

void ImageBlock::releaseRenderCache() { releasePxcSlot(); }

void ImageBlock::renderPlaceholder(GfxRenderer& renderer, const int x, const int y) const {
  renderPlaceholderAt(renderer, x, y, width, height);
}

// Sized form: render() needs the placeholder to match the geometry it actually drew at, which for
// a rotated block is the fitted size in the rotated frame, not the stored natural size.
void ImageBlock::renderPlaceholderAt(GfxRenderer& renderer, const int x, const int y, const int w, const int h) const {
  renderer.fillRect(x, y, w, h, true);
  if (w > 2 && h > 2) {
    renderer.fillRect(x + 1, y + 1, w - 2, h - 2, false);
  }
}

void ImageBlock::render(GfxRenderer& renderer, const int x, const int y) {
  // The font-prewarm scan pass only accumulates glyphs; an image contributes
  // none, and its DirectPixelWriter output bypasses the renderer's scan-mode
  // suppression, so it would otherwise do a full (discarded) cache render every
  // page view. Skip it here. The image still draws in the real BW/grayscale
  // passes; on first view this just moves the one-time decode to the BW pass.
  FontCacheManager* fcm = renderer.getFontCacheManager();
  if (fcm && fcm->isScanning()) return;

  LOG_DBG("IMG", "Rendering image at %d,%d: %s (%dx%d)", x, y, imagePath.c_str(), width, height);

  // Rotated images draw UPRIGHT IN THE ADJACENT ORIENTATION: the panel is physically the same, so
  // turning the logical frame 90 degrees turns the picture on screen and the reader tilts the
  // device. The same trick MangaReaderActivity uses for full pages ((o + 3) % 4).
  //
  // This is what the `rotated` flag always promised and never did -- render() used to bounds-check
  // the block's natural dimensions against the upright screen, reject them, and draw nothing
  // ("Invalid render position: ... size (1920x848) screen (480x800)"). The block deliberately
  // stores natural dims: only here is the target frame known.
  //
  // The fit MUST match warmCache()'s exactly, or the .pxc it wrote is rejected on a dimension
  // mismatch and every pass re-decodes. warmCache computes it from the unswapped screen
  // (getScreenHeight() for width, getScreenWidth() for height); after setOrientation those are
  // simply getScreenWidth()/getScreenHeight() here, so the two agree by construction.
  struct OrientationGuard {
    GfxRenderer& r;
    GfxRenderer::Orientation saved;
    bool active = false;
    ~OrientationGuard() {
      if (active) r.setOrientation(saved);
    }
  } orientation{renderer, static_cast<GfxRenderer::Orientation>(renderer.getOrientation())};

  int drawX = x, drawY = y, drawW = width, drawH = height;
  if (rotated) {
    renderer.setOrientation(static_cast<GfxRenderer::Orientation>((static_cast<int>(orientation.saved) + 3) % 4));
    orientation.active = true;
    fitWithin(std::max(1, renderer.getScreenWidth() - 2 * reserveMargin_),
              std::max(1, renderer.getScreenHeight() - 2 * reserveMargin_), drawW, drawH);
    // Centred in the rotated frame; the caller's x/y are upright-frame coordinates and mean
    // nothing here, which is why callers pass (0, 0) for a rotated block.
    drawX = (renderer.getScreenWidth() - drawW) / 2;
    drawY = (renderer.getScreenHeight() - drawH) / 2;
    LOG_DBG("IMG", "Rotated fit: %dx%d -> %dx%d at %d,%d (frame %dx%d)", width, height, drawW, drawH, drawX, drawY,
            renderer.getScreenWidth(), renderer.getScreenHeight());
  }

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();

  // Bounds check the DRAWN geometry (a rotated block was fitted above), not the stored natural size
  if (drawX < 0 || drawY < 0 || drawX + drawW > screenWidth || drawY + drawH > screenHeight) {
    LOG_ERR("IMG", "Invalid render position: (%d,%d) size (%dx%d) screen (%dx%d)", drawX, drawY, drawW, drawH,
            screenWidth, screenHeight);
    return;
  }

  // Tiled grayscale (#2190): skip the whole image when it doesn't touch the
  // active band. The per-pixel writer already clips off-band pixels, but without
  // this each of the ~7 bands per plane re-ran the full cache load / pixel walk
  // and discarded the result — the dominant cost of AA on image pages. The check
  // is orientation-aware and returns true when no strip is active, so the BW
  // pass and non-tiled controllers render the image exactly as before.
  if (!renderer.glyphIntersectsStrip(drawX, drawY, drawX + drawW - 1, drawY + drawH - 1)) {
    return;
  }

  if (imageFailedThisSession(imagePath)) {
    renderPlaceholderAt(renderer, drawX, drawY, drawW, drawH);
    return;
  }

  // Try to render from cache first
  std::string cachePath = getCachePath(imagePath);
  if (renderFromCache(renderer, cachePath, drawX, drawY, drawW, drawH)) {
    return;  // Successfully rendered from cache
  }

  // The build only header-probed the image for dimensions; pull the actual
  // file out of the book now, on first visit to the page.
  if (!srcPath.empty() && extractFn && !Storage.exists(imagePath.c_str())) {
    LOG_DBG("IMG", "Lazy-extracting %s -> %s (maxAlloc=%u)", srcPath.c_str(), imagePath.c_str(),
            (unsigned)ESP.getMaxAllocHeap());
    if (!extractFn(extractCtx, srcPath.c_str(), imagePath.c_str())) {
      // Nearly always the zip inflate window -- one contiguous 32KB block -- not the entry.
      // Measured on the horizontal path: free=68620 but maxAlloc=25588..29684, because the
      // incremental section build stays resident for as long as the chapter is open.
      //
      // Releasing font caches here was tried and does NOTHING: maxAlloc 29684 -> 29684 measured,
      // i.e. not one contiguous byte, while costing a glyph reload. Do not re-add it. The block
      // is held by the build, so the only lever that moves this number is suspendBuild() (the
      // word-lookup path takes it from 6132 to 38900 that way), which has to be driven from a
      // context that owns the build, not from inside a render.
      LOG_ERR("IMG", "Lazy extraction failed (maxAlloc=%u, inflate window needs 32768): %s",
              (unsigned)ESP.getMaxAllocHeap(), srcPath.c_str());
    }
  }

  // No cache - need to decode the image
  // Check if image file exists
  HalFile file;
  if (!Storage.openFileForRead("IMG", imagePath, file)) {
    LOG_ERR("IMG", "Image file not found: %s", imagePath.c_str());
    rememberImageFailure(imagePath);
    renderPlaceholderAt(renderer, drawX, drawY, drawW, drawH);
    return;
  }
  size_t fileSize = file.size();
  file.close();

  if (fileSize == 0) {
    LOG_ERR("IMG", "Image file is empty: %s", imagePath.c_str());
    rememberImageFailure(imagePath);
    renderPlaceholderAt(renderer, drawX, drawY, drawW, drawH);
    return;
  }

  LOG_DBG("IMG", "Decoding and caching: %s", imagePath.c_str());

  RenderConfig config;
  config.x = drawX;
  config.y = drawY;
  config.maxWidth = drawW;
  config.maxHeight = drawH;
  config.useGrayscale = true;
  config.useDithering = true;
  config.performanceMode = false;
  config.useExactDimensions = true;  // Use pre-calculated dimensions to avoid rounding mismatches
  config.cachePath = cachePath;      // Enable caching during decode

  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(imagePath);
  if (!decoder) {
    LOG_ERR("IMG", "No decoder found for image: %s", imagePath.c_str());
    rememberImageFailure(imagePath);
    renderPlaceholderAt(renderer, drawX, drawY, drawW, drawH);
    return;
  }

  LOG_DBG("IMG", "Using %s decoder", decoder->getFormatName());

  bool success = decoder->decodeToFramebuffer(imagePath, renderer, config);
  if (!success) {
    LOG_ERR("IMG", "Failed to decode image: %s", imagePath.c_str());
    rememberImageFailure(imagePath);
    renderPlaceholderAt(renderer, drawX, drawY, drawW, drawH);
    return;
  }

  LOG_DBG("IMG", "Decode successful");
}

bool ImageBlock::serialize(HalFile& file) {
  serialization::writeString(file, imagePath);
  serialization::writeString(file, srcPath);
  serialization::writePod(file, width);
  serialization::writePod(file, height);
  // rotated/reserveMargin_ were NOT persisted before v55, which is why the rotate-to-fill path
  // never worked in a cached section: the parser set the flag, serialize() dropped it, and every
  // load produced an unrotated block still holding NATURAL dimensions -- which render() then
  // rejected as out of bounds, drawing nothing. The flag is layout state, not a render-time
  // decision, so it has to survive the round trip with the dims it belongs to.
  serialization::writePod(file, rotated);
  serialization::writePod(file, reserveMargin_);
  return true;
}

std::unique_ptr<ImageBlock> ImageBlock::deserialize(HalFile& file) {
  std::string path;
  std::string src;
  serialization::readString(file, path);
  serialization::readString(file, src);
  int16_t w, h;
  serialization::readPod(file, w);
  serialization::readPod(file, h);
  bool rot = false;
  int16_t reserve = 0;
  serialization::readPod(file, rot);
  serialization::readPod(file, reserve);
  auto block = std::unique_ptr<ImageBlock>(new (std::nothrow) ImageBlock(path, src, w, h));
  if (block && rot) block->setRotated(true, reserve);
  return block;
}

// Shared by the rotated render path, the vertical reader's image-page fit and warmCache(), so a
// background warm computes EXACTLY the dimensions the later render expects from the pixel cache.
void ImageBlock::fitWithin(const int availW, const int availH, int& w, int& h) {
  if (w > availW || h > availH) {
    const float sx = static_cast<float>(availW) / w;
    const float sy = static_cast<float>(availH) / h;
    const float s = (sx < sy) ? sx : sy;
    w = static_cast<int>(w * s + 0.5f);
    h = static_cast<int>(h * s + 0.5f);
  }
  if (w < 1) w = 1;
  if (h < 1) h = 1;
}

ImageBlock::WarmResult ImageBlock::warmCache(GfxRenderer& renderer, bool (*shouldCancel)(const void*),
                                             const void* cancelCtx) const {
  // BMP never streams a pixel cache (the BMP converter rejects cacheOnly) and renders fast
  // without one -- nothing to warm.
  const size_t dotPos = imagePath.rfind('.');
  if (dotPos != std::string::npos) {
    std::string ext = imagePath.substr(dotPos);
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext == ".bmp") return WarmResult::NotApplicable;
  }

  // The dimensions the eventual render will ask renderFromCache for: the stored fit dims, or
  // for a rotated image the same rotated-frame fit render() computes. The rotated render draws
  // upright in the ADJACENT orientation, whose screen box is the current one with W/H swapped.
  int dstW = width;
  int dstH = height;
  if (rotated) {
    const int usableW = std::max(1, renderer.getScreenHeight() - 2 * reserveMargin_);
    const int usableH = std::max(1, renderer.getScreenWidth() - 2 * reserveMargin_);
    fitWithin(usableW, usableH, dstW, dstH);
  }

  const std::string cachePath = getCachePath(imagePath);
  {
    HalFile cacheFile;
    if (Storage.openFileForRead("IMG", cachePath, cacheFile)) {
      uint16_t cachedWidth = 0, cachedHeight = 0;
      if (cacheFile.read(&cachedWidth, 2) == 2 && cacheFile.read(&cachedHeight, 2) == 2 &&
          abs(cachedWidth - dstW) <= 1 && abs(cachedHeight - dstH) <= 1) {
        return WarmResult::AlreadyWarm;  // same 1px tolerance as renderFromCache
      }
      // Unreadable header or dimension mismatch (layout changed): the decode below rewrites it.
    }
  }

  if (!Storage.exists(imagePath.c_str())) {
    // Same recovery render() performs, and for the same reason: the build extracts eagerly and can
    // lose an image to a transient OOM (the 32KB inflate window vs. the layout's own heap peak).
    // Doing it here too means the retry lands on the idle warm task instead of inline in the first
    // page render that reaches the image -- measured at ~3.5s when render had to do it (1.5s
    // extract + 1.4s decode). Without this the warm simply reported the miss and gave up, leaving
    // that cost on the page turn.
    if (srcPath.empty() || !extractFn) {
      LOG_ERR("IMG", "Warm: image file not found and no source to re-extract: %s", imagePath.c_str());
      return WarmResult::Failed;
    }
    LOG_DBG("IMG", "Warm: lazy-extracting %s -> %s", srcPath.c_str(), imagePath.c_str());
    if (!extractFn(extractCtx, srcPath.c_str(), imagePath.c_str()) || !Storage.exists(imagePath.c_str())) {
      LOG_ERR("IMG", "Warm: lazy extraction failed: %s", srcPath.c_str());
      return WarmResult::Failed;
    }
  }
  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(imagePath);
  if (!decoder) {
    return WarmResult::Failed;  // unsupported extension
  }

  RenderConfig config;
  config.x = 0;
  config.y = 0;
  config.maxWidth = dstW;
  config.maxHeight = dstH;
  config.useGrayscale = true;
  config.useDithering = true;
  config.performanceMode = false;
  config.useExactDimensions = true;
  config.cacheOnly = true;  // stream only the .2bp cache; never touch the framebuffer
  config.cachePath = cachePath;
  config.shouldCancel = shouldCancel;
  config.cancelCtx = cancelCtx;

  LOG_DBG("IMG", "Warming image cache: %s (%dx%d)", cachePath.c_str(), dstW, dstH);
  if (decoder->decodeToFramebuffer(imagePath, renderer, config)) {
    return WarmResult::Warmed;
  }
  // The converter already dropped any partial cache file on both cancel and failure.
  return (shouldCancel && shouldCancel(cancelCtx)) ? WarmResult::Cancelled : WarmResult::Failed;
}
