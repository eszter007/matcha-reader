#include "GrayLogo.h"

#include <GfxRenderer.h>

#include "images/Logo160.h"

namespace {

// 0 = black, 1 = dark gray, 2 = light gray, 3 = white (see Logo160.h)
inline uint8_t levelAt(const int px, const int py) {
  const int i = py * GrayLogo::SIZE + px;
  return (Logo160Gray[i >> 2] >> ((3 - (i & 3)) * 2)) & 0x3;
}

}  // namespace

namespace GrayLogo {

void drawBase(GfxRenderer& renderer, const int x, const int y, const bool invertedScreen) {
  for (int py = 0; py < SIZE; py++) {
    for (int px = 0; px < SIZE; px++) {
      const uint8_t val = levelAt(px, py);
      if (invertedScreen) {
        // Post-invert targets: black artwork -> white, white artwork -> background black,
        // grays -> black start state for the nudge. Pre-invert that means: paint only the
        // artwork's black pixels; leave everything else at the white background.
        if (val == 0) renderer.drawPixel(x + px, y + py, true);
      } else {
        // Same rule as GfxRenderer's grayscale bitmap base pass: everything but white starts
        // black; the nudge planes lighten the grays afterwards.
        if (val < 3) renderer.drawPixel(x + px, y + py, true);
      }
    }
  }
}

void drawBw(GfxRenderer& renderer, const int x, const int y) {
  for (int py = 0; py < SIZE; py++) {
    for (int px = 0; px < SIZE; px++) {
      // Threshold between the two grays: dark shading keeps its ink, light shading drops out.
      if (levelAt(px, py) <= 1) renderer.drawPixel(x + px, y + py, true);
    }
  }
}

void flushGrayPasses(GfxRenderer& renderer, const int x, const int y, const bool invertedScreen) {
  // Plane semantics (identical to GfxRenderer::drawBitmap's grayscale modes): buffer cleared
  // to 0, then a set bit means "nudge this pixel". LSB plane = dark gray only; MSB plane =
  // dark or light gray. On an inverted screen dark and light swap so shading keeps its sense.
  const auto planeLevel = [invertedScreen](const int px, const int py) -> uint8_t {
    const uint8_t val = levelAt(px, py);
    if (invertedScreen && (val == 1 || val == 2)) return static_cast<uint8_t>(3 - val);
    return val;
  };

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  for (int py = 0; py < SIZE; py++) {
    for (int px = 0; px < SIZE; px++) {
      if (planeLevel(px, py) == 1) renderer.drawPixel(x + px, y + py, false);
    }
  }
  renderer.copyGrayscaleLsbBuffers();

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  for (int py = 0; py < SIZE; py++) {
    for (int px = 0; px < SIZE; px++) {
      const uint8_t val = planeLevel(px, py);
      if (val == 1 || val == 2) renderer.drawPixel(x + px, y + py, false);
    }
  }
  renderer.copyGrayscaleMsbBuffers();

  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);
}

}  // namespace GrayLogo
