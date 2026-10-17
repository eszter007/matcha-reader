#include "GrayLogo.h"

#include <GfxRenderer.h>
#include <InflateReader.h>
#include <Logging.h>
#include <Memory.h>

#include <memory>

#include "images/CatLogo.h"

static_assert(GrayLogo::WIDTH == CAT_LOGO_WIDTH && GrayLogo::HEIGHT == CAT_LOGO_HEIGHT,
              "GrayLogo dimensions must match the generated bitmap");
static_assert(GrayLogo::SMALL_WIDTH == CAT_LOGO_SMALL_WIDTH && GrayLogo::SMALL_HEIGHT == CAT_LOGO_SMALL_HEIGHT,
              "GrayLogo small dimensions must match the generated bitmap");

namespace {

// Walks a logo's run-length code in pixel order. Every pass visits the whole logo row by row, so a cursor that
// expands one run at a time replaces random access without a decode buffer.
class LogoCursor {
 public:
  explicit LogoCursor(const uint8_t* code) : rle(code) {}

  // One pixel: bits 0-1 the level (0 = black, 1 = dark gray, 2 = light gray, 3 = white), bit 2
  // whether it lies in the footprint (the cat, the books and the sticker edge around them).
  uint8_t next() {
    if (left == 0) {
      const uint8_t code = rle[pos++];
      symbol = code >> 5;
      left = (code & 0x1F) + 1;
    }
    left--;
    return symbol;
  }

 private:
  const uint8_t* rle;
  size_t pos = 0;
  uint8_t symbol = 0;
  uint8_t left = 0;
};

constexpr uint8_t level(const uint8_t symbol) { return symbol & 0x3; }
constexpr bool inFootprint(const uint8_t symbol) { return symbol & 0x4; }

struct Art {
  int width = 0;
  int height = 0;
  const uint8_t* rle = nullptr;
  std::unique_ptr<uint8_t[]> inflated;  // the small cat's run-length code, inflated for this draw
};

bool loadArt(const GrayLogo::Size size, Art& art) {
  if (size == GrayLogo::Size::Regular) {
    art.width = CAT_LOGO_WIDTH;
    art.height = CAT_LOGO_HEIGHT;
    art.rle = CatLogoRle;
    return true;
  }
  art.inflated = makeUniqueNoThrow<uint8_t[]>(CAT_LOGO_SMALL_RLE_SIZE);
  if (!art.inflated) {
    LOG_ERR("LOGO", "OOM: %d bytes for the small cat", CAT_LOGO_SMALL_RLE_SIZE);
    return false;
  }
  InflateReader reader;
  reader.init(false);
  reader.setSource(CatLogoSmallDeflated, sizeof(CatLogoSmallDeflated));
  if (!reader.read(art.inflated.get(), CAT_LOGO_SMALL_RLE_SIZE)) {
    LOG_ERR("LOGO", "Small cat failed to inflate");
    return false;
  }
  art.width = CAT_LOGO_SMALL_WIDTH;
  art.height = CAT_LOGO_SMALL_HEIGHT;
  art.rle = art.inflated.get();
  return true;
}

}  // namespace

namespace GrayLogo {

void drawBase(const GfxRenderer& renderer, const int x, const int y, const bool invertedScreen, const Size size) {
  Art art;
  if (!loadArt(size, art)) return;
  LogoCursor logo(art.rle);
  for (int py = 0; py < art.height; py++) {
    for (int px = 0; px < art.width; px++) {
      const uint8_t symbol = logo.next();
      const uint8_t val = level(symbol);
      if (invertedScreen) {
        // The artwork keeps its own colours on the dark screen, set off by a white edge: post-invert
        // targets inside the footprint are white artwork -> white, black -> black, grays -> black
        // start state for the nudge; outside it the background stays black. Pre-invert that means
        // painting only the footprint's white pixels.
        if (val == 3 && inFootprint(symbol)) renderer.drawPixel(x + px, y + py, true);
      } else {
        // Same rule as GfxRenderer's grayscale bitmap base pass: everything but white starts
        // black; the nudge planes lighten the grays afterwards.
        if (val < 3) renderer.drawPixel(x + px, y + py, true);
      }
    }
  }
}

void drawBw(const GfxRenderer& renderer, const int x, const int y) {
  LogoCursor logo(CatLogoRle);
  for (int py = 0; py < HEIGHT; py++) {
    for (int px = 0; px < WIDTH; px++) {
      // Threshold between the two grays: dark shading keeps its ink, light shading drops out.
      if (level(logo.next()) <= 1) renderer.drawPixel(x + px, y + py, true);
    }
  }
}

void flushGrayPasses(GfxRenderer& renderer, const int x, const int y, const bool invertedScreen, const Size size) {
  Art art;
  if (!loadArt(size, art)) return;
  // Plane semantics (identical to GfxRenderer::drawBitmap's grayscale modes): buffer cleared
  // to 0, then a set bit means "nudge this pixel". LSB plane = dark gray only; MSB plane =
  // dark or light gray. The dark screen keeps the artwork's own shades, so only its footprint
  // is nudged; outside it stays background black.
  const auto planeLevel = [invertedScreen](const uint8_t symbol) -> uint8_t {
    if (invertedScreen && !inFootprint(symbol)) return 0;
    return level(symbol);
  };

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  LogoCursor lsb(art.rle);
  for (int py = 0; py < art.height; py++) {
    for (int px = 0; px < art.width; px++) {
      if (planeLevel(lsb.next()) == 1) renderer.drawPixel(x + px, y + py, false);
    }
  }
  renderer.copyGrayscaleLsbBuffers();

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  LogoCursor msb(art.rle);
  for (int py = 0; py < art.height; py++) {
    for (int px = 0; px < art.width; px++) {
      const uint8_t val = planeLevel(msb.next());
      if (val == 1 || val == 2) renderer.drawPixel(x + px, y + py, false);
    }
  }
  renderer.copyGrayscaleMsbBuffers();

  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);
}

}  // namespace GrayLogo
