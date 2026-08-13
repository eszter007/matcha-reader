#include "PixelCache.h"

#include <Arduino.h>
#include <GfxRenderer.h>

#include "DirectPixelWriter.h"

namespace {
// Per render mode, "does this packed byte (4 pixels) contain any pixel that draws?" -- the exact
// complement of writePixel()'s early-out, evaluated four pixels at a time. Manga and scanned
// pages are mostly flat runs (0xFF pure white, 0x00 pure black), and in a grayscale plane pass
// only the two mid-levels draw at all, so most bytes answer no and the whole unpack is skipped.
// 768 bytes of flash; constexpr so it never lands in DRAM.
struct DrawByteTables {
  bool bw[256];
  bool lsb[256];
  bool msb[256];
};

constexpr DrawByteTables buildDrawByteTables() {
  DrawByteTables t{};
  for (int b = 0; b < 256; ++b) {
    for (int p = 0; p < 4; ++p) {
      const int v = (b >> (6 - p * 2)) & 0x03;
      if (v < 3) t.bw[b] = true;
      if (v == 1) t.lsb[b] = true;
      if (v == 1 || v == 2) t.msb[b] = true;
    }
  }
  return t;
}

constexpr DrawByteTables DRAW_BYTE_TABLES = buildDrawByteTables();
}  // namespace

std::string PixelCacheIO::pathFor(const std::string& imagePath) {
  // Replace extension with .pxc (pixel cache)
  size_t dotPos = imagePath.rfind('.');
  if (dotPos != std::string::npos) {
    return imagePath.substr(0, dotPos) + ".pxc";
  }
  return imagePath + ".pxc";
}

bool PixelCacheIO::Reader::open(const std::string& cachePath) {
  close();
  if (!Storage.openFileForRead("IMG", cachePath, file)) {
    return false;
  }
  uint16_t w16, h16;
  if (file.read(&w16, 2) != 2 || file.read(&h16, 2) != 2) {
    close();
    return false;
  }
  if (w16 == 0 || h16 == 0) {
    close();
    return false;
  }
  width_ = w16;
  height_ = h16;
  return true;
}

void PixelCacheIO::Reader::close() {
  if (file.isOpen()) file.close();
  width_ = 0;
  height_ = 0;
}

bool PixelCacheIO::renderFromCache(GfxRenderer& renderer, const std::string& cachePath, int x, int y, int expectedWidth,
                                   int expectedHeight) {
  Reader reader;
  if (!reader.open(cachePath)) return false;
  return reader.render(renderer, x, y, expectedWidth, expectedHeight);
}

bool PixelCacheIO::Reader::render(GfxRenderer& renderer, int x, int y, int expectedWidth, int expectedHeight) {
  if (!file.isOpen()) return false;

  const int cachedWidth = width_;
  const int cachedHeight = height_;

  // Verify dimensions are close (allow 1 pixel tolerance for rounding differences)
  if (abs(cachedWidth - expectedWidth) > 1 || abs(cachedHeight - expectedHeight) > 1) {
    LOG_ERR("IMG", "Cache dimension mismatch: %dx%d vs %dx%d", cachedWidth, cachedHeight, expectedWidth,
            expectedHeight);
    return false;
  }

  // Rewind past the header: a second or third pass over the same handle starts where the previous
  // one stopped.
  if (!file.seekSet(HEADER_BYTES)) {
    LOG_ERR("IMG", "Cache seek to body failed");
    return false;
  }

  LOG_DBG("IMG", "Rendering from cache (%dx%d)", cachedWidth, cachedHeight);

  // Read several rows per SD access. A full-page image is re-rendered on every
  // grayscale strip pass (~14x per page), and a one-row-per-read loop here means
  // cachedHeight (~728) tiny reads through the storage mutex + SdFat each time —
  // the dominant cost of displaying an image page. Batching rows into a ~4KB
  // buffer cuts that to ~20 reads per pass without holding the whole image.
  const int bytesPerRow = (cachedWidth + 3) / 4;  // 2 bits per pixel, 4 pixels per byte
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

  // Whole-byte skip table for the mode this pass renders in (see DRAW_BYTE_TABLES). Taken from
  // the writer, not the renderer, so it cannot disagree with the predicate writePixel() applies.
  const bool* byteHasDraw = DRAW_BYTE_TABLES.bw;
  if (pw.mode == GfxRenderer::GRAYSCALE_LSB) {
    byteHasDraw = DRAW_BYTE_TABLES.lsb;
  } else if (pw.mode == GfxRenderer::GRAYSCALE_MSB) {
    byteHasDraw = DRAW_BYTE_TABLES.msb;
  }

  const unsigned long startMs = millis();
  unsigned long readMs = 0;
  uint32_t skippedBytes = 0, drawnBytes = 0;

  int rowsInBuffer = 0;
  int bufferRow = 0;
  for (int row = 0; row < cachedHeight; row++) {
    if (bufferRow >= rowsInBuffer) {
      const int toRead = (cachedHeight - row < rowsPerRead) ? (cachedHeight - row) : rowsPerRead;
      const size_t bytes = (size_t)toRead * bytesPerRow;
      const unsigned long readStart = millis();
      const bool readOk = file.read(readBuffer, bytes) == static_cast<int>(bytes);
      readMs += millis() - readStart;
      if (!readOk) {
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
    // Step a packed byte at a time: one load and one table lookup decide four pixels. A byte no
    // pixel of which would draw is skipped whole -- that is the common case on flat artwork, and
    // it was previously four loads, four shift/mask unpacks and four writePixel() calls that all
    // fell out at the early-out. A byte that does draw still walks only its in-range lanes, so a
    // partial byte at either end of the band range never spills outside [colStart, colEnd).
    int col = colStart;
    while (col < colEnd) {
      const uint8_t packed = rowBuffer[col >> 2];
      const int lanesLeft = 4 - (col & 3);
      const int run = (colEnd - col < lanesLeft) ? (colEnd - col) : lanesLeft;
      if (!byteHasDraw[packed]) {
        skippedBytes++;
        col += run;
        continue;
      }
      drawnBytes++;
      for (int i = 0; i < run; i++) {
        const int bitShift = 6 - ((col + i) & 3) * 2;  // MSB first within byte
        pw.writePixel(x + col + i, (packed >> bitShift) & 0x03);
      }
      col += run;
    }
  }

  free(readBuffer);
  // Splits the pass into its two costs, so "manga page turns are slow" can be attributed without
  // another guess: SD throughput versus the per-pixel write loop. skipped/drawn is how much of
  // the image the byte-skip above removed from that loop.
  LOG_DBG("IMG", "Cache render complete: %lums total, %lums SD, %u bytes skipped / %u drawn", millis() - startMs,
          readMs, skippedBytes, drawnBytes);
  return true;
}
