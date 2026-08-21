#pragma once

#include <HalStorage.h>

class Print;
class ZipFile;

// Polled between decoded blocks; returning true aborts the conversion (the caller must drop
// the partial output). Lets a long thumbnail conversion give way to a button press.
using BmpConvertCancelFn = bool (*)(void* ctx);

class JpegToBmpConverter {
  static bool jpegFileToBmpStreamInternal(HalFile& jpegFile, Print& bmpOut, int targetWidth, int targetHeight,
                                          bool oneBit, bool crop = true, BmpConvertCancelFn shouldCancel = nullptr,
                                          void* cancelCtx = nullptr, bool* outUnsupported = nullptr);

 public:
  static bool jpegFileToBmpStream(HalFile& jpegFile, Print& bmpOut, bool crop = true);
  // Convert with custom target size (for thumbnails)
  static bool jpegFileToBmpStreamWithSize(HalFile& jpegFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight);
  // Convert to 1-bit BMP (black and white only, no grays) for fast home screen rendering
  // outUnsupported (optional): set true when the conversion failed for a reason that will NEVER
  // succeed for this file -- currently a source image beyond MAX_IMAGE_WIDTH/HEIGHT. Left alone
  // for every other failure (out of memory, cancelled by a button press, decode error), which
  // are all "not right now" and must stay retryable. Without this the caller cannot tell the two
  // apart and has to assume transient, which retries an unconvertible cover on every pass
  // forever (X3 report).
  static bool jpegFileTo1BitBmpStreamWithSize(HalFile& jpegFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight,
                                              BmpConvertCancelFn shouldCancel = nullptr, void* cancelCtx = nullptr,
                                              bool* outUnsupported = nullptr);
};
