#pragma once

#include <HalStorage.h>
#include <JpegToBmpConverter.h>  // BmpConvertCancelFn

class Print;

class PngToBmpConverter {
  static bool pngFileToBmpStreamInternal(HalFile& pngFile, Print& bmpOut, int targetWidth, int targetHeight,
                                         bool oneBit, bool crop = true, bool originalThresholds = false,
                                         BmpConvertCancelFn shouldCancel = nullptr, void* cancelCtx = nullptr);

 public:
  static bool pngFileToBmpStream(HalFile& pngFile, Print& bmpOut, bool crop = true, bool originalThresholds = false);
  static bool pngFileToBmpStreamWithSize(HalFile& pngFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight);
  static bool pngFileTo1BitBmpStreamWithSize(HalFile& pngFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight,
                                             BmpConvertCancelFn shouldCancel = nullptr, void* cancelCtx = nullptr);
};
