#pragma once

#include <HalStorage.h>
#include <JpegToBmpConverter.h>  // BmpConvertCancelFn
#include <Print.h>

// Cover thumbnails from page images that are ALREADY bitmaps.
//
// The other converters start from a continuous-tone source, so they can dither straight to the
// target size. A .bmp page has usually been dithered once already, at full page size, and a
// dithered image cannot simply be sampled down: at a 2.4x reduction nearly every destination
// pixel covers at least one black dot, so the "darkest wins" rule that keeps thin line art alive
// in GfxRenderer::drawBitmap1Bit turns a mid-grey screentone into solid black (device report:
// a manga cover looked near-black in the grid while its full-screen 1:1 draw looked right).
//
// So this measures COVERAGE -- what fraction of the source area a destination pixel spans is
// dark -- and re-dithers that grey. The output is a 1-bit BMP at exactly the requested box, i.e.
// the same artifact the JPEG/PNG converters produce, so the grid draws it through the fast
// packed path and never scales a bitmap at render time.
class BmpToBmpConverter {
 public:
  // Covers the target box and trims the overflow, matching the JPEG/PNG converters' framing.
  // Returns false (writing nothing usable) on a malformed source, an allocation failure, a
  // cancel, or if fewer rows than requested were produced.
  static bool bmpFileTo1BitBmpStreamWithSize(HalFile& srcFile, Print& bmpOut, int targetWidth, int targetHeight,
                                             BmpConvertCancelFn shouldCancel = nullptr, void* cancelCtx = nullptr);
};
