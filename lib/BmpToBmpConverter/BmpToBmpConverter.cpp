#include "BmpToBmpConverter.h"

#include <Arduino.h>
#include <Bitmap.h>
#include <BitmapHelpers.h>
#include <GifImageLayout.h>
#include <HalDisplay.h>
#include <Logging.h>
#include <Memory.h>
#include <Print.h>

#include <algorithm>
#include <cstring>

namespace {

void write16(Print& out, const uint16_t v) {
  out.write(static_cast<uint8_t>(v & 0xFF));
  out.write(static_cast<uint8_t>(v >> 8));
}

void write32(Print& out, const uint32_t v) {
  out.write(static_cast<uint8_t>(v & 0xFF));
  out.write(static_cast<uint8_t>((v >> 8) & 0xFF));
  out.write(static_cast<uint8_t>((v >> 16) & 0xFF));
  out.write(static_cast<uint8_t>((v >> 24) & 0xFF));
}

// A monochrome manga page is already spatially dithered. Box-filtering it back to gray and then
// feeding that gray into Atkinson a second time otherwise biases small thumbnails toward black:
// Atkinson intentionally propagates only 75% of the quantization error, so dark midtones lose
// part of the error that would have restored their white-dot density. Apply a gentle, endpoint-
// preserving midtone lift before the second dither. Pure line work (0) and paper (255) remain
// byte-for-byte unchanged; the maximum lift is 32 at mid-gray.
int restoreMonochromeMidtone(const int gray) { return gray + (gray * (255 - gray) + 255) / 510; }

// `topDown` follows the SOURCE's row order: rows are emitted in the order they are read, so
// letting the header describe that order is what avoids buffering the whole image just to flip
// it (a 266x400 thumb would be ~14KB of it).
void writeBmpHeader1bit(Print& out, const int width, const int height, const bool topDown) {
  const int bytesPerRow = (width + 31) / 32 * 4;
  const int imageSize = bytesPerRow * height;

  out.write('B');
  out.write('M');
  write32(out, 62 + imageSize);
  write32(out, 0);
  write32(out, 62);  // 14 (file header) + 40 (DIB header) + 8 (palette)

  write32(out, 40);
  write32(out, static_cast<uint32_t>(width));
  write32(out, static_cast<uint32_t>(topDown ? -height : height));
  write16(out, 1);
  write16(out, 1);  // 1 bit per pixel
  write32(out, 0);  // BI_RGB
  write32(out, static_cast<uint32_t>(imageSize));
  write32(out, 2835);
  write32(out, 2835);
  write32(out, 2);
  write32(out, 2);

  const uint8_t palette[8] = {0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0x00};  // BGRA: 0 black, 1 white
  for (const uint8_t b : palette) out.write(b);
}

}  // namespace

bool BmpToBmpConverter::bmpFileTo1BitBmpStreamWithSize(HalFile& srcFile, Print& bmpOut, const int targetWidth,
                                                       const int targetHeight, BmpConvertCancelFn shouldCancel,
                                                       void* cancelCtx) {
  if (targetWidth <= 0 || targetHeight <= 0) return false;

  Bitmap src(srcFile);
  if (src.parseHeaders() != BmpReaderError::Ok) {
    LOG_ERR("BMP", "Cannot parse source bitmap headers");
    return false;
  }
  const int srcW = src.getWidth();
  const int srcH = src.getHeight();
  if (srcW <= 0 || srcH <= 0) return false;
  // Upscaling would only blur an already-quantised page; the caller keeps the raw path instead.
  if (srcW < targetWidth || srcH < targetHeight) {
    LOG_DBG("BMP", "Source %dx%d smaller than target %dx%d; not converting", srcW, srcH, targetWidth, targetHeight);
    return false;
  }

  // Cover the box: crop the axis that overflows once the other one fits exactly.
  int cropX = 0, cropY = 0, cropW = srcW, cropH = srcH;
  if (static_cast<int64_t>(srcW) * targetHeight > static_cast<int64_t>(srcH) * targetWidth) {
    cropW = static_cast<int>(static_cast<int64_t>(srcH) * targetWidth / targetHeight);
    cropX = (srcW - cropW) / 2;
  } else {
    cropH = static_cast<int>(static_cast<int64_t>(srcW) * targetHeight / targetWidth);
    cropY = (srcH - cropH) / 2;
  }
  if (cropW <= 0 || cropH <= 0) return false;

  const int outRowBytes = (targetWidth + 31) / 32 * 4;
  auto packedRow = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>((srcW + 3) / 4));
  auto rowBuffer = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(src.getRowBytes()));
  auto sum = makeUniqueNoThrow<uint32_t[]>(static_cast<size_t>(targetWidth));
  auto count = makeUniqueNoThrow<uint32_t[]>(static_cast<size_t>(targetWidth));
  auto outRow = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(outRowBytes));
  if (!packedRow || !rowBuffer || !sum || !count || !outRow) {
    LOG_ERR("BMP", "OOM building %dx%d thumbnail", targetWidth, targetHeight);
    return false;
  }
  Atkinson1BitDitherer dither(targetWidth);
  if (!dither.isValid()) {
    LOG_ERR("BMP", "OOM allocating dither rows for %d px", targetWidth);
    return false;
  }
  const bool restoreTone = src.is1Bit();

  const bool topDown = src.isTopDown();
  writeBmpHeader1bit(bmpOut, targetWidth, targetHeight, topDown);

  int rowsWritten = 0;
  int accumulatedFor = -1;  // destination row the accumulators currently hold
  auto emitRow = [&]() {
    memset(outRow.get(), 0, static_cast<size_t>(outRowBytes));
    for (int dx = 0; dx < targetWidth; dx++) {
      // No source pixel landed here (possible only at a rounding edge): treat as paper white.
      int gray = count[dx] ? static_cast<int>(sum[dx] / count[dx]) : 255;
      if (restoreTone) gray = restoreMonochromeMidtone(gray);
      if (dither.processPixel(gray, dx)) {
        outRow[dx / 8] |= static_cast<uint8_t>(0x80 >> (dx % 8));  // palette index 1 = white
      }
    }
    dither.nextRow();
    bmpOut.write(outRow.get(), static_cast<size_t>(outRowBytes));
    rowsWritten++;
  };

  for (int i = 0; i < srcH; i++) {
    if (src.readNextRow(packedRow.get(), rowBuffer.get()) != BmpReaderError::Ok) {
      LOG_ERR("BMP", "Failed to read source row %d", i);
      return false;
    }
    if (shouldCancel && shouldCancel(cancelCtx)) {
      LOG_DBG("BMP", "Conversion cancelled");
      return false;
    }
    // readNextRow yields rows in FILE order; the crop is expressed in visual coordinates.
    const int visualY = topDown ? i : (srcH - 1 - i);
    if (visualY < cropY || visualY >= cropY + cropH) continue;
    const int dstY = (visualY - cropY) * targetHeight / cropH;
    if (dstY >= targetHeight) continue;

    if (dstY != accumulatedFor) {
      if (accumulatedFor >= 0) emitRow();
      memset(sum.get(), 0, static_cast<size_t>(targetWidth) * sizeof(uint32_t));
      memset(count.get(), 0, static_cast<size_t>(targetWidth) * sizeof(uint32_t));
      accumulatedFor = dstY;
    }

    for (int sx = cropX; sx < cropX + cropW; sx++) {
      // readNextRow quantises every source depth to 2 bits, so one branch covers 1/8/24-bit
      // sources: 0..3 spread over 0..255 is the grey this pixel contributes.
      const uint8_t val = (packedRow[sx / 4] >> (6 - ((sx * 2) % 8))) & 0x3;
      const int dx = (sx - cropX) * targetWidth / cropW;
      if (dx >= targetWidth) continue;
      sum[dx] += static_cast<uint32_t>(val) * 85;
      count[dx]++;
    }
  }
  if (accumulatedFor >= 0) emitRow();

  // A short image would leave the tail of the declared pixel array unwritten -- the same
  // half-drawn thumbnail the JPEG path guards against, so refuse it rather than cache it.
  if (rowsWritten != targetHeight) {
    LOG_ERR("BMP", "Produced %d of %d rows", rowsWritten, targetHeight);
    return false;
  }
  return true;
}

namespace {

bool writeLE(Print& out, const uint32_t value, const int bytes) {
  for (int i = 0; i < bytes; ++i) {
    if (out.write(static_cast<uint8_t>(value >> (8 * i))) != 1) return false;
  }
  return true;
}

bool writeBmpHeader(Print& out, const int width, const int height, const bool topDown, const int bits,
                    const uint32_t rowBytes) {
  const uint32_t colors = 1u << bits;
  const uint32_t headerSize = 54 + colors * 4;
  const uint32_t imageSize = rowBytes * height;
  if (out.write('B') != 1 || out.write('M') != 1 || !writeLE(out, headerSize + imageSize, 4) || !writeLE(out, 0, 4) ||
      !writeLE(out, headerSize, 4) || !writeLE(out, 40, 4) || !writeLE(out, width, 4) ||
      !writeLE(out, static_cast<uint32_t>(topDown ? -height : height), 4) || !writeLE(out, 1, 2) ||
      !writeLE(out, bits, 2) || !writeLE(out, 0, 4) || !writeLE(out, imageSize, 4) || !writeLE(out, 2835, 4) ||
      !writeLE(out, 2835, 4) || !writeLE(out, colors, 4) || !writeLE(out, colors, 4)) {
    return false;
  }
  for (uint32_t i = 0; i < colors; ++i) {
    const uint32_t gray = i * 255 / (colors - 1);
    if (!writeLE(out, gray | gray << 8 | gray << 16, 4)) return false;
  }
  return true;
}

// Source position of output index i in 8.8 fixed point, pixel centers aligned.
int upscaleSource(const int i, const int dst, const int src) {
  return static_cast<int>(std::max<int64_t>(0, (static_cast<int64_t>(2 * i + 1) * src - dst) * 128 / dst));
}

}  // namespace

bool BmpToBmpConverter::bmpFileToBmpStream(HalFile& bmpFile, Print& bmpOut, const bool crop,
                                           const bool originalThresholds) {
  Bitmap src(bmpFile);
  const BmpReaderError err = src.parseHeaders();
  if (err != BmpReaderError::Ok) {
    LOG_ERR("BMP", "Invalid source BMP: %s", Bitmap::errorToString(err));
    return false;
  }

  // Display dimensions swapped for portrait, as for book covers.
  GifCommon::ImageLayout layout;
  if (!GifCommon::calculateLayout(src.getWidth(), src.getHeight(), display.getDisplayHeight(),
                                  display.getDisplayWidth(), crop, layout)) {
    LOG_ERR("BMP", "Invalid BMP layout %dx%d", src.getWidth(), src.getHeight());
    return false;
  }
  const int srcW = layout.sourceWidth;
  const int srcH = layout.sourceHeight;
  const int dstW = layout.width;
  const int dstH = layout.height;
  const bool upscale = dstW > srcW;
  const bool oneBit = src.is1Bit();
  const int bits = oneBit ? 1 : 2;
  const uint32_t outRowBytes = (static_cast<uint32_t>(dstW) * bits + 31) / 32 * 4;

  const size_t rawBytes = static_cast<size_t>(src.getRowBytes());
  const size_t grayBytes = static_cast<size_t>(src.getWidth()) * 2;  // two rows for bilinear upscaling
  auto raw = makeUniqueNoThrow<uint8_t[]>(rawBytes);
  auto gray = makeUniqueNoThrow<uint8_t[]>(grayBytes);
  auto accum = makeUniqueNoThrow<uint32_t[]>(dstW);
  auto outRow = makeUniqueNoThrow<uint8_t[]>(outRowBytes);
  if (!raw || !gray || !accum || !outRow) {
    LOG_ERR("BMP", "OOM: BMP conversion buffers (%u bytes)",
            static_cast<unsigned>(rawBytes + grayBytes + dstW * sizeof(uint32_t) + outRowBytes));
    return false;
  }
  std::unique_ptr<AtkinsonDitherer> dither2;
  std::unique_ptr<Atkinson1BitDitherer> dither1;
  if (oneBit) {
    dither1 = makeUniqueNoThrow<Atkinson1BitDitherer>(dstW);
  } else {
    dither2 = makeUniqueNoThrow<AtkinsonDitherer>(dstW, originalThresholds);
  }
  if (oneBit ? (!dither1 || !dither1->isValid()) : (!dither2 || !dither2->isValid())) {
    LOG_ERR("BMP", "OOM: BMP conversion ditherer (width %d)", dstW);
    return false;
  }

  // Emit rows in source file order so SD reads stay sequential.
  const bool topDown = src.isTopDown();
  if (!writeBmpHeader(bmpOut, dstW, dstH, topDown, bits, outRowBytes)) {
    LOG_ERR("BMP", "Failed to write BMP header");
    return false;
  }

  // Two cached source rows; a row still needed (`keep`) is never evicted.
  uint8_t* rows[2] = {gray.get(), gray.get() + src.getWidth()};
  int loaded[2] = {-1, -1};
  const auto rowFor = [&](const int y, const int keep) -> uint8_t* {
    if (loaded[0] == y) return rows[0];
    if (loaded[1] == y) return rows[1];
    const int slot = loaded[0] == keep ? 1 : 0;
    if (src.readRowGray(layout.sourceY + y, rows[slot], raw.get()) != BmpReaderError::Ok) {
      LOG_ERR("BMP", "Failed to read source row %d", y);
      return nullptr;
    }
    loaded[slot] = y;
    return rows[slot];
  };

  for (int i = 0; i < dstH; ++i) {
    const int outY = topDown ? i : dstH - 1 - i;
    if (upscale) {
      const int fy = upscaleSource(outY, dstH, srcH);
      const int y0 = std::min(fy >> 8, srcH - 1);
      const int y1 = std::min(y0 + 1, srcH - 1);
      const int wy = y0 < y1 ? fy & 0xFF : 0;
      const uint8_t* row0 = rowFor(y0, y1);
      const uint8_t* row1 = row0 ? rowFor(y1, y0) : nullptr;
      if (!row1) return false;
      for (int outX = 0; outX < dstW; ++outX) {
        const int fx = upscaleSource(outX, dstW, srcW);
        const int x0 = layout.sourceX + std::min(fx >> 8, srcW - 1);
        const int x1 = std::min(x0 + 1, layout.sourceX + srcW - 1);
        const int wx = fx & 0xFF;
        const int top = row0[x0] * (256 - wx) + row0[x1] * wx;
        const int bottom = row1[x0] * (256 - wx) + row1[x1] * wx;
        accum[outX] = (top * (256 - wy) + bottom * wy) >> 16;
      }
    } else {
      // Area average over the source box covered by this output row.
      const int y0 = outY * srcH / dstH;
      const int y1 = std::max(y0 + 1, (outY + 1) * srcH / dstH);
      memset(accum.get(), 0, dstW * sizeof(uint32_t));
      for (int k = y0; k < y1; ++k) {
        const uint8_t* row0 = rowFor(topDown ? k : y0 + y1 - 1 - k, -1);
        if (!row0) return false;
        for (int outX = 0; outX < dstW; ++outX) {
          const int x0 = outX * srcW / dstW;
          const int x1 = std::max(x0 + 1, (outX + 1) * srcW / dstW);
          uint32_t sum = 0;
          for (int x = x0; x < x1; ++x) sum += row0[layout.sourceX + x];
          accum[outX] += sum / (x1 - x0);
        }
      }
      for (int outX = 0; outX < dstW; ++outX) accum[outX] /= (y1 - y0);
    }

    memset(outRow.get(), 0, outRowBytes);
    for (int x = 0; x < dstW; ++x) {
      if (oneBit) {
        outRow[x / 8] |= dither1->processPixel(accum[x], x) << (7 - x % 8);
      } else {
        outRow[x / 4] |= dither2->processPixel(adjustPixel(accum[x]), x) << (6 - (x % 4) * 2);
      }
    }
    if (oneBit)
      dither1->nextRow();
    else
      dither2->nextRow();

    if (bmpOut.write(outRow.get(), outRowBytes) != outRowBytes) {
      LOG_ERR("BMP", "Short BMP row write");
      return false;
    }
    if ((i & 63) == 63) vTaskDelay(1);
  }
  return true;
}
