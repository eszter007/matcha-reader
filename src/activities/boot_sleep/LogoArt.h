#pragma once

#include <cstddef>
#include <cstdint>

// Pure helpers behind the custom boot/sleep logo (GrayLogo.cpp): no SD card, renderer or heap, so
// they run in host tests.
namespace logo_art {

struct Fit {
  int width;
  int height;
};

// srcW x srcH scaled down (never up) to fit boxW x boxH, keeping the aspect ratio.
Fit fitInBox(int srcW, int srcH, int boxW, int boxH);

// The target row that source row srcY feeds when srcH rows shrink to h (nearest neighbour), or -1
// when no target row samples it. Each target row in [0, h) is fed by exactly one source row.
int targetRow(int srcY, int srcH, int h);

// Images are 2 bits per pixel, row-major: 0 black, 1 dark gray, 2 light gray, 3 white.
inline uint8_t packedLevel(const uint8_t* packed, const size_t i) {
  return (packed[i >> 2] >> (6 - (i & 3) * 2)) & 0x3;
}
// Masks are 1 bit per pixel, row-major.
inline bool maskBit(const uint8_t* mask, const size_t i) { return mask[i >> 3] & (0x80 >> (i & 7)); }
inline void setMaskBit(uint8_t* mask, const size_t i) { mask[i >> 3] |= 0x80 >> (i & 7); }

// The dark-screen footprint of a w x h image, written to `footprint` ((w * h + 7) / 8 bytes): every
// pixel except the white connected to the image border, grown by `grow` px. `scratch` is a second
// mask of the same size. Returns false when the background fill hit its sweep cap before it
// converged; the footprint is still complete, but may include white the fill did not reach.
bool buildFootprint(const uint8_t* packed, int w, int h, int grow, uint8_t* footprint, uint8_t* scratch);

}  // namespace logo_art
