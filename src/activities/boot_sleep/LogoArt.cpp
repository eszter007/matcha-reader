#include "LogoArt.h"

#include <algorithm>
#include <cstring>

namespace logo_art {

namespace {
// The background fill converges in a few sweeps for real drawings; the cap bounds a pathological one.
constexpr int MAX_FILL_SWEEPS = 64;
}  // namespace

Fit fitInBox(const int srcW, const int srcH, const int boxW, const int boxH) {
  if (srcW <= boxW && srcH <= boxH) return {srcW, srcH};
  if (srcW * boxH > srcH * boxW) return {boxW, std::max(1, srcH * boxW / srcW)};
  return {std::max(1, srcW * boxH / srcH), boxH};
}

int targetRow(const int srcY, const int srcH, const int h) {
  // Target row ty samples source row ty * srcH / h; h <= srcH, so a source row feeds at most one.
  const int ty = (srcY * h + srcH - 1) / srcH;
  return ty < h && ty * srcH / h == srcY ? ty : -1;
}

bool buildFootprint(const uint8_t* packed, const int w, const int h, const int grow, uint8_t* footprint,
                    uint8_t* scratch) {
  const size_t count = static_cast<size_t>(w) * h;
  const size_t maskSize = (count + 7) / 8;
  uint8_t* outside = footprint;
  memset(outside, 0, maskSize);
  const auto white = [packed](const size_t i) { return packedLevel(packed, i) == 3; };

  // Background: white pixels connected to the image border. Alternate forward and backward sweeps
  // until nothing changes, which needs no fill stack.
  for (int x = 0; x < w; x++) {
    if (white(x)) setMaskBit(outside, x);
    const size_t bottom = static_cast<size_t>(h - 1) * w + x;
    if (white(bottom)) setMaskBit(outside, bottom);
  }
  for (int y = 0; y < h; y++) {
    const size_t row = static_cast<size_t>(y) * w;
    if (white(row)) setMaskBit(outside, row);
    if (white(row + w - 1)) setMaskBit(outside, row + w - 1);
  }
  bool changed = true;
  for (int sweep = 0; changed && sweep < MAX_FILL_SWEEPS; sweep++) {
    changed = false;
    for (size_t i = 0; i < count; i++) {
      if (maskBit(outside, i) || !white(i)) continue;
      const int x = static_cast<int>(i % w);
      if ((x > 0 && maskBit(outside, i - 1)) || (i >= static_cast<size_t>(w) && maskBit(outside, i - w))) {
        setMaskBit(outside, i);
        changed = true;
      }
    }
    for (size_t i = count; i-- > 0;) {
      if (maskBit(outside, i) || !white(i)) continue;
      const int x = static_cast<int>(i % w);
      if ((x < w - 1 && maskBit(outside, i + 1)) || (i + w < count && maskBit(outside, i + w))) {
        setMaskBit(outside, i);
        changed = true;
      }
    }
  }

  // The footprint is the rest; grow it one pixel per step, alternating 4- and 8-neighbour steps so the
  // edge comes out rounded rather than square.
  for (size_t b = 0; b < maskSize; b++) footprint[b] = ~outside[b];
  for (int step = 0; step < grow; step++) {
    memcpy(scratch, footprint, maskSize);
    const bool diagonals = step % 2 == 1;
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        const size_t i = static_cast<size_t>(y) * w + x;
        if (maskBit(scratch, i)) continue;
        bool grows = false;
        for (int dy = -1; dy <= 1 && !grows; dy++) {
          for (int dx = -1; dx <= 1 && !grows; dx++) {
            if ((dx == 0 && dy == 0) || (!diagonals && dx != 0 && dy != 0)) continue;
            const int nx = x + dx;
            const int ny = y + dy;
            if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
            grows = maskBit(scratch, static_cast<size_t>(ny) * w + nx);
          }
        }
        if (grows) setMaskBit(footprint, i);
      }
    }
  }
  return !changed;
}

}  // namespace logo_art
