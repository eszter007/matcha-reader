#include "GrayLogo.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <InflateReader.h>
#include <Logging.h>
#include <Memory.h>
#include <SdSystemDir.h>

#include <cstring>
#include <memory>

#include "LogoArt.h"
#include "images/CatLogo.h"

static_assert(GrayLogo::WIDTH == CAT_LOGO_WIDTH && GrayLogo::HEIGHT == CAT_LOGO_HEIGHT,
              "GrayLogo dimensions must match the generated bitmap");
static_assert(GrayLogo::SMALL_WIDTH == CAT_LOGO_SMALL_WIDTH && GrayLogo::SMALL_HEIGHT == CAT_LOGO_SMALL_HEIGHT,
              "GrayLogo small dimensions must match the generated bitmap");

namespace {

constexpr char CUSTOM_LOGO_NAME[] = "logo.bmp";  // in the SD card's system folder
// Sources up to a screen a side, either orientation. Only the rows the scaled logo samples are read.
constexpr int CUSTOM_LOGO_MAX_SIDE = 800;
constexpr uint8_t FOOTPRINT = 0x4;
// White border kept around a custom logo; on the dark screen it becomes the sticker edge.
constexpr int STICKER_EDGE = 4;

struct Art {
  int width = 0;
  int height = 0;
  // Where the art sits inside the logo box: a custom logo smaller than the box is centred in it.
  int offsetX = 0;
  int offsetY = 0;
  // Exactly one is set. rle: the cat's run-length code. packed: a custom logo, 2 bits per pixel in
  // row-major order.
  const uint8_t* rle = nullptr;
  const uint8_t* packed = nullptr;
  // A custom logo's footprint, 1 bit per pixel in the same order; only built for the dark screen.
  const uint8_t* footprint = nullptr;
  std::unique_ptr<uint8_t[]> owned;  // the inflated small cat or the decoded custom logo
  std::unique_ptr<uint8_t[]> ownedFootprint;
};

using logo_art::maskBit;
using logo_art::packedLevel;

// Walks a logo in pixel order. Every pass visits the whole logo row by row, so a cursor that
// expands one run at a time replaces random access without a decode buffer.
class LogoCursor {
 public:
  explicit LogoCursor(const Art& art) : rle(art.rle), packed(art.packed), footprint(art.footprint) {}

  // One pixel: bits 0-1 the level (0 = black, 1 = dark gray, 2 = light gray, 3 = white), bit 2
  // whether it lies in the footprint (the cat, the books and the sticker edge around them).
  uint8_t next() {
    if (packed) {
      const bool inside = !footprint || maskBit(footprint, pos);
      const uint8_t value = packedLevel(packed, pos);
      pos++;
      return (inside ? FOOTPRINT : 0) | value;
    }
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
  const uint8_t* packed;
  const uint8_t* footprint;
  size_t pos = 0;
  uint8_t symbol = 0;
  uint8_t left = 0;
};

constexpr uint8_t level(const uint8_t symbol) { return symbol & 0x3; }
constexpr bool inFootprint(const uint8_t symbol) { return symbol & FOOTPRINT; }

// A custom logo's footprint, like the cat's: everything except the white background that reaches the
// edge of the image, grown by STICKER_EDGE px so the dark screen shows a white sticker edge around it.
bool buildFootprint(Art& art) {
  // Two 1-bit masks, at most 230 * 324 / 8 = 9,315 bytes each; the scratch one is freed on return.
  const size_t maskSize = (static_cast<size_t>(art.width) * art.height + 7) / 8;
  auto footprint = makeUniqueNoThrow<uint8_t[]>(maskSize);
  auto scratch = makeUniqueNoThrow<uint8_t[]>(maskSize);
  if (!footprint || !scratch) {
    LOG_ERR("LOGO", "OOM: 2 x %u bytes for the logo footprint", static_cast<unsigned>(maskSize));
    return false;
  }
  if (!logo_art::buildFootprint(art.owned.get(), art.width, art.height, STICKER_EDGE, footprint.get(), scratch.get())) {
    LOG_INF("LOGO", "Footprint fill stopped early; some enclosed white may stay white on the dark screen");
  }
  art.ownedFootprint = std::move(footprint);
  art.footprint = art.ownedFootprint.get();
  return true;
}

// Decodes logo.bmp, scaled down (never up) to fit the logo box with a STICKER_EDGE white border.
// On the dark screen it keeps its own colours inside its footprint, like the cat.
bool loadCustomArt(const GrayLogo::Size size, const bool invertedScreen, Art& art) {
  std::string path;
  if (!sdsystem::findExistingUserFile(CUSTOM_LOGO_NAME, path)) return false;
  HalFile file;
  if (!Storage.openFileForRead("LOGO", path, file)) return false;
  Bitmap bitmap(file);
  const auto parsed = bitmap.parseHeaders();
  if (parsed != BmpReaderError::Ok) {
    LOG_ERR("LOGO", "Invalid %s: %s", path.c_str(), Bitmap::errorToString(parsed));
    return false;
  }

  if (bitmap.getWidth() > CUSTOM_LOGO_MAX_SIDE || bitmap.getHeight() > CUSTOM_LOGO_MAX_SIDE) {
    LOG_ERR("LOGO", "%s is %dx%d, over %d px a side", path.c_str(), bitmap.getWidth(), bitmap.getHeight(),
            CUSTOM_LOGO_MAX_SIDE);
    return false;
  }
  bitmap.setEvenLevels(true);
  bitmap.setTransparentAsWhite(true);

  const int boxW = (size == GrayLogo::Size::Regular ? GrayLogo::WIDTH : GrayLogo::SMALL_WIDTH) - 2 * STICKER_EDGE;
  const int boxH = (size == GrayLogo::Size::Regular ? GrayLogo::HEIGHT : GrayLogo::SMALL_HEIGHT) - 2 * STICKER_EDGE;
  const int srcW = bitmap.getWidth();
  const int srcH = bitmap.getHeight();
  const auto fit = logo_art::fitInBox(srcW, srcH, boxW, boxH);
  const int w = fit.width;
  const int h = fit.height;

  // The art canvas adds the white border on every side.
  const int canvasW = w + 2 * STICKER_EDGE;
  const int canvasH = h + 2 * STICKER_EDGE;
  // Packed art: at most 230 * 324 / 4 = 18,630 bytes, kept until the gray passes. Rows: one 2-bit
  // output row plus one raw row (at most 800 px * 4 bytes), freed before drawing.
  const size_t packedSize = (static_cast<size_t>(canvasW) * canvasH + 3) / 4;
  const size_t outRowSize = (srcW + 3) / 4;
  art.owned = makeUniqueNoThrow<uint8_t[]>(packedSize);
  auto rows = makeUniqueNoThrow<uint8_t[]>(outRowSize + bitmap.getRowBytes());
  if (!art.owned || !rows) {
    LOG_ERR("LOGO", "OOM: %u + %u bytes for %s", static_cast<unsigned>(packedSize),
            static_cast<unsigned>(outRowSize + bitmap.getRowBytes()), path.c_str());
    return false;
  }
  memset(art.owned.get(), 0xFF, packedSize);
  uint8_t* outRow = rows.get();
  uint8_t* rawRow = outRow + outRowSize;

  for (int fileRow = 0; fileRow < srcH; fileRow++) {
    const int srcY = bitmap.isTopDown() ? fileRow : srcH - 1 - fileRow;
    const int ty = logo_art::targetRow(srcY, srcH, h);
    // Rows no target samples are skipped with a seek, so a large source costs little more to read.
    const auto rowResult = ty < 0 ? bitmap.skipRow() : bitmap.readNextRow(outRow, rawRow);
    if (rowResult != BmpReaderError::Ok) {
      LOG_ERR("LOGO", "Reading %s failed: %s", path.c_str(), Bitmap::errorToString(rowResult));
      return false;
    }
    if (ty < 0) continue;
    for (int tx = 0; tx < w; tx++) {
      const int srcX = tx * srcW / w;
      const uint8_t value = packedLevel(outRow, srcX);
      const size_t pos = static_cast<size_t>(ty + STICKER_EDGE) * canvasW + tx + STICKER_EDGE;
      const int shift = 6 - (pos & 3) * 2;
      art.owned[pos >> 2] = (art.owned[pos >> 2] & ~(0x3 << shift)) | (value << shift);
    }
  }

  rows.reset();

  art.width = canvasW;
  art.height = canvasH;
  art.offsetX = (boxW - w) / 2;
  art.offsetY = (boxH - h) / 2;
  art.packed = art.owned.get();
  return !invertedScreen || buildFootprint(art);
}

bool loadBuiltinArt(const GrayLogo::Size size, Art& art) {
  if (size == GrayLogo::Size::Regular) {
    art.width = CAT_LOGO_WIDTH;
    art.height = CAT_LOGO_HEIGHT;
    art.rle = CatLogoRle;
    return true;
  }
  art.owned = makeUniqueNoThrow<uint8_t[]>(CAT_LOGO_SMALL_RLE_SIZE);
  if (!art.owned) {
    LOG_ERR("LOGO", "OOM: %d bytes for the small cat", CAT_LOGO_SMALL_RLE_SIZE);
    return false;
  }
  InflateReader reader;
  reader.init(false);
  reader.setSource(CatLogoSmallDeflated, sizeof(CatLogoSmallDeflated));
  if (!reader.read(art.owned.get(), CAT_LOGO_SMALL_RLE_SIZE)) {
    LOG_ERR("LOGO", "Small cat failed to inflate");
    return false;
  }
  art.width = CAT_LOGO_SMALL_WIDTH;
  art.height = CAT_LOGO_SMALL_HEIGHT;
  art.rle = art.owned.get();
  return true;
}

// The custom logo when logo.bmp loads, the cat otherwise. A failed load is not remembered, so
// the next draw tries again.
bool loadArt(const GrayLogo::Size size, const bool invertedScreen, Art& art) {
  if (loadCustomArt(size, invertedScreen, art)) return true;
  art = Art{};
  return loadBuiltinArt(size, art);
}

// The art drawBase drew, kept for flushGrayPasses so it nudges the same pixels without decoding
// or building the footprint a second time. Released when the gray passes end.
struct BaseArt {
  Art art;
  GrayLogo::Size size = GrayLogo::Size::Regular;
  bool invertedScreen = false;
  bool valid = false;
};
BaseArt baseArt;

}  // namespace

namespace GrayLogo {

void drawBase(const GfxRenderer& renderer, const int x, const int y, const bool invertedScreen, const Size size) {
  baseArt = BaseArt{};
  Art& art = baseArt.art;
  if (!loadArt(size, invertedScreen, art)) return;
  baseArt.size = size;
  baseArt.invertedScreen = invertedScreen;
  baseArt.valid = true;
  LogoCursor logo(art);
  const int artX = x + art.offsetX;
  const int artY = y + art.offsetY;
  for (int py = 0; py < art.height; py++) {
    for (int px = 0; px < art.width; px++) {
      const uint8_t symbol = logo.next();
      const uint8_t val = level(symbol);
      if (invertedScreen) {
        // The artwork keeps its own colours on the dark screen, set off by a white edge: post-invert
        // targets inside the footprint are white artwork -> white, black -> black, grays -> black
        // start state for the nudge; outside it the background stays black. Pre-invert that means
        // painting only the footprint's white pixels.
        if (val == 3 && inFootprint(symbol)) renderer.drawPixel(artX + px, artY + py, true);
      } else {
        // Same rule as GfxRenderer's grayscale bitmap base pass: everything but white starts
        // black; the nudge planes lighten the grays afterwards.
        if (val < 3) renderer.drawPixel(artX + px, artY + py, true);
      }
    }
  }
}

void drawBw(const GfxRenderer& renderer, const int x, const int y) {
  Art art;
  if (!loadArt(Size::Regular, false, art)) return;
  LogoCursor logo(art);
  const int artX = x + art.offsetX;
  const int artY = y + art.offsetY;
  for (int py = 0; py < art.height; py++) {
    for (int px = 0; px < art.width; px++) {
      // Threshold between the two grays: dark shading keeps its ink, light shading drops out.
      if (level(logo.next()) <= 1) renderer.drawPixel(artX + px, artY + py, true);
    }
  }
}

void flushGrayPasses(GfxRenderer& renderer, const int x, const int y, const bool invertedScreen, const Size size) {
  Art art;
  if (baseArt.valid && baseArt.size == size && baseArt.invertedScreen == invertedScreen) {
    art = std::move(baseArt.art);
  } else if (!loadArt(size, invertedScreen, art)) {
    LOG_ERR("LOGO", "Logo art unavailable for the gray passes");
    baseArt = BaseArt{};
    renderer.setRenderMode(GfxRenderer::BW);
    return;
  }
  baseArt = BaseArt{};
  const int artX = x + art.offsetX;
  const int artY = y + art.offsetY;
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
  LogoCursor lsb(art);
  for (int py = 0; py < art.height; py++) {
    for (int px = 0; px < art.width; px++) {
      if (planeLevel(lsb.next()) == 1) renderer.drawPixel(artX + px, artY + py, false);
    }
  }
  renderer.copyGrayscaleLsbBuffers();

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  LogoCursor msb(art);
  for (int py = 0; py < art.height; py++) {
    for (int px = 0; px < art.width; px++) {
      const uint8_t val = planeLevel(msb.next());
      if (val == 1 || val == 2) renderer.drawPixel(artX + px, artY + py, false);
    }
  }
  renderer.copyGrayscaleMsbBuffers();

  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);
}

}  // namespace GrayLogo
