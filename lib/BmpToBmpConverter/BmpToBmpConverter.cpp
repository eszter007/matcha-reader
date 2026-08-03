#include "BmpToBmpConverter.h"

#include <Bitmap.h>
#include <BitmapHelpers.h>
#include <Logging.h>
#include <Memory.h>

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
