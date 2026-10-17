#include "GrayLogo.h"

#include <GfxRenderer.h>

#include "images/CatLogo.h"

static_assert(GrayLogo::WIDTH == CAT_LOGO_WIDTH && GrayLogo::HEIGHT == CAT_LOGO_HEIGHT,
              "GrayLogo dimensions must match the generated bitmap");

namespace {

// 0 = black, 1 = dark gray, 2 = light gray, 3 = white (see CatLogo.h)
inline uint8_t levelAt(const int px, const int py) {
  const int i = py * GrayLogo::WIDTH + px;
  return (CatLogoGray[i >> 2] >> ((3 - (i & 3)) * 2)) & 0x3;
}

// The cat and books plus a white edge around them (see CatLogo.h).
inline bool inFootprint(const int px, const int py) {
  constexpr int rowBytes = (GrayLogo::WIDTH + 7) / 8;
  return (CatLogoFootprint[py * rowBytes + (px >> 3)] >> (7 - (px & 7))) & 1;
}

}  // namespace

namespace GrayLogo {

void drawBase(const GfxRenderer& renderer, const int x, const int y, const bool invertedScreen) {
  for (int py = 0; py < HEIGHT; py++) {
    for (int px = 0; px < WIDTH; px++) {
      const uint8_t val = levelAt(px, py);
      if (invertedScreen) {
        // The artwork keeps its own colours on the dark screen, set off by a white edge: post-invert
        // targets inside the footprint are white artwork -> white, black -> black, grays -> black
        // start state for the nudge; outside it the background stays black. Pre-invert that means
        // painting only the footprint's white pixels.
        if (val == 3 && inFootprint(px, py)) renderer.drawPixel(x + px, y + py, true);
      } else {
        // Same rule as GfxRenderer's grayscale bitmap base pass: everything but white starts
        // black; the nudge planes lighten the grays afterwards.
        if (val < 3) renderer.drawPixel(x + px, y + py, true);
      }
    }
  }
}

void drawBw(const GfxRenderer& renderer, const int x, const int y) {
  for (int py = 0; py < HEIGHT; py++) {
    for (int px = 0; px < WIDTH; px++) {
      // Threshold between the two grays: dark shading keeps its ink, light shading drops out.
      if (levelAt(px, py) <= 1) renderer.drawPixel(x + px, y + py, true);
    }
  }
}

void flushGrayPasses(GfxRenderer& renderer, const int x, const int y, const bool invertedScreen) {
  // Plane semantics (identical to GfxRenderer::drawBitmap's grayscale modes): buffer cleared
  // to 0, then a set bit means "nudge this pixel". LSB plane = dark gray only; MSB plane =
  // dark or light gray. The dark screen keeps the artwork's own shades, so only its footprint
  // is nudged; outside it stays background black.
  const auto planeLevel = [invertedScreen](const int px, const int py) -> uint8_t {
    if (invertedScreen && !inFootprint(px, py)) return 0;
    return levelAt(px, py);
  };

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  for (int py = 0; py < HEIGHT; py++) {
    for (int px = 0; px < WIDTH; px++) {
      if (planeLevel(px, py) == 1) renderer.drawPixel(x + px, y + py, false);
    }
  }
  renderer.copyGrayscaleLsbBuffers();

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  for (int py = 0; py < HEIGHT; py++) {
    for (int px = 0; px < WIDTH; px++) {
      const uint8_t val = planeLevel(px, py);
      if (val == 1 || val == 2) renderer.drawPixel(x + px, y + py, false);
    }
  }
  renderer.copyGrayscaleMsbBuffers();

  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);
}

}  // namespace GrayLogo
