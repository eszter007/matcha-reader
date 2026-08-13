#pragma once

#include <JPEGDEC.h>

// JPEGDEC reports every failure as a small integer, and the ones that occur on this device mean
// very different things. `Decode failed (rc=0, lastError=2)` reads like memory pressure, which is
// what sent the last investigation looking at the heap for a file that was simply an unusual --
// and, until the JPEGDEC patches, unsupported -- progressive layout (issue #16).
//
// Lives here rather than beside either caller because both converters need it: the framebuffer
// one in lib/Epub/Epub/converters (which already includes this lib) and JpegToBmpConverter itself.
//
// Every return is a string literal, so the text is flash-resident, costs no allocation, and is
// safe to hand straight to LOG_ERR.
inline const char* jpegDecodeErrorText(const int err) {
  switch (err) {
    case JPEG_SUCCESS:
      return "success";
    case JPEG_INVALID_PARAMETER:
      return "invalid parameter";
    case JPEG_DECODE_ERROR:
      // The catch-all: corrupt data, a truncated file, or a bitstream this decoder walked out of
      // step with. NOT an out-of-memory condition, which is what the bare number suggested.
      return "corrupt or truncated JPEG data";
    case JPEG_UNSUPPORTED_FEATURE:
      // Reachable via the 0003 patch, which refuses the one layout it cannot traverse: a
      // progressive JPEG whose DC scan is split per component AND which is chroma-subsampled.
      // Name the remedy here -- it is the whole difference between a two-minute fix and the
      // investigation in issue #16.
      return "unsupported JPEG variant (progressive, split DC scan, chroma subsampled) -- "
             "re-encode baseline, e.g. jpegtran -copy none -optimize";
    case JPEG_INVALID_FILE:
      return "not a JPEG file";
    default:
      return "unknown error";
  }
}
