#include "GrayLogo.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <InflateReader.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>
#include <memory>

#include "images/CatLogo.h"

static_assert(GrayLogo::WIDTH == CAT_LOGO_WIDTH && GrayLogo::HEIGHT == CAT_LOGO_HEIGHT,
              "GrayLogo dimensions must match the generated bitmap");
static_assert(GrayLogo::SMALL_WIDTH == CAT_LOGO_SMALL_WIDTH && GrayLogo::SMALL_HEIGHT == CAT_LOGO_SMALL_HEIGHT,
              "GrayLogo small dimensions must match the generated bitmap");

namespace {

constexpr char CUSTOM_LOGO_PATH[] = "/logo.bmp";
// Every source row is read on every draw (boot decodes once, sleep twice), so cap the read at
// roughly one screen.
constexpr int CUSTOM_LOGO_MAX_SIDE = 800;
constexpr uint8_t FOOTPRINT = 0x4;

struct Art {
  int width = 0;
  int height = 0;
  // Where the art sits inside the logo box: a custom logo smaller than the box is centred in it.
  int offsetX = 0;
  int offsetY = 0;
  // Exactly one is set. rle: the cat's run-length code. packed: a custom logo, 2 bits per pixel in
  // row-major order, levels already final for the screen polarity, every pixel in the footprint.
  const uint8_t* rle = nullptr;
  const uint8_t* packed = nullptr;
  std::unique_ptr<uint8_t[]> owned;  // the inflated small cat or the decoded custom logo
};

// Walks a logo in pixel order. Every pass visits the whole logo row by row, so a cursor that
// expands one run at a time replaces random access without a decode buffer.
class LogoCursor {
 public:
  explicit LogoCursor(const Art& art) : rle(art.rle), packed(art.packed) {}

  // One pixel: bits 0-1 the level (0 = black, 1 = dark gray, 2 = light gray, 3 = white), bit 2
  // whether it lies in the footprint (the cat, the books and the sticker edge around them).
  uint8_t next() {
    if (packed) {
      const uint8_t value = (packed[pos >> 2] >> (6 - (pos & 3) * 2)) & 0x3;
      pos++;
      return FOOTPRINT | value;
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
  size_t pos = 0;
  uint8_t symbol = 0;
  uint8_t left = 0;
};

constexpr uint8_t level(const uint8_t symbol) { return symbol & 0x3; }
constexpr bool inFootprint(const uint8_t symbol) { return symbol & FOOTPRINT; }

// Decodes /logo.bmp, scaled down (never up) to fit the logo box. The whole image counts as footprint,
// with its levels pre-inverted on the dark screen, so dark artwork on white shows light on black.
bool loadCustomArt(const GrayLogo::Size size, const bool invertedScreen, Art& art) {
  HalFile file;
  if (!Storage.openFileForRead("LOGO", CUSTOM_LOGO_PATH, file)) return false;
  Bitmap bitmap(file);
  const auto parsed = bitmap.parseHeaders();
  if (parsed != BmpReaderError::Ok) {
    LOG_ERR("LOGO", "Invalid %s: %s", CUSTOM_LOGO_PATH, Bitmap::errorToString(parsed));
    return false;
  }

  if (bitmap.getWidth() > CUSTOM_LOGO_MAX_SIDE || bitmap.getHeight() > CUSTOM_LOGO_MAX_SIDE) {
    LOG_ERR("LOGO", "%s is %dx%d, over %d px a side", CUSTOM_LOGO_PATH, bitmap.getWidth(), bitmap.getHeight(),
            CUSTOM_LOGO_MAX_SIDE);
    return false;
  }

  const int boxW = size == GrayLogo::Size::Regular ? GrayLogo::WIDTH : GrayLogo::SMALL_WIDTH;
  const int boxH = size == GrayLogo::Size::Regular ? GrayLogo::HEIGHT : GrayLogo::SMALL_HEIGHT;
  const int srcW = bitmap.getWidth();
  const int srcH = bitmap.getHeight();
  int w = srcW;
  int h = srcH;
  if (w > boxW || h > boxH) {
    if (srcW * boxH > srcH * boxW) {
      w = boxW;
      h = std::max(1, srcH * boxW / srcW);
    } else {
      h = boxH;
      w = std::max(1, srcW * boxH / srcH);
    }
  }

  // Packed art: at most 230 * 324 / 4 = 18,630 bytes. Rows: one 2-bit output row plus one raw row
  // (at most 800 px * 4 bytes). Both live only for this draw.
  const size_t packedSize = (static_cast<size_t>(w) * h + 3) / 4;
  const size_t outRowSize = (srcW + 3) / 4;
  art.owned = makeUniqueNoThrow<uint8_t[]>(packedSize);
  auto rows = makeUniqueNoThrow<uint8_t[]>(outRowSize + bitmap.getRowBytes());
  if (!art.owned || !rows) {
    LOG_ERR("LOGO", "OOM: %u + %u bytes for %s", static_cast<unsigned>(packedSize),
            static_cast<unsigned>(outRowSize + bitmap.getRowBytes()), CUSTOM_LOGO_PATH);
    return false;
  }
  memset(art.owned.get(), 0, packedSize);
  uint8_t* outRow = rows.get();
  uint8_t* rawRow = rows.get() + outRowSize;

  for (int fileRow = 0; fileRow < srcH; fileRow++) {
    const auto rowResult = bitmap.readNextRow(outRow, rawRow);
    if (rowResult != BmpReaderError::Ok) {
      LOG_ERR("LOGO", "Reading %s failed: %s", CUSTOM_LOGO_PATH, Bitmap::errorToString(rowResult));
      return false;
    }
    // Nearest-neighbour: target row ty samples source row ty * srcH / h. Since h <= srcH, each
    // source row feeds at most one target row.
    const int srcY = bitmap.isTopDown() ? fileRow : srcH - 1 - fileRow;
    const int ty = (srcY * h + srcH - 1) / srcH;
    if (ty >= h || ty * srcH / h != srcY) continue;
    for (int tx = 0; tx < w; tx++) {
      const int srcX = tx * srcW / w;
      uint8_t value = (outRow[srcX >> 2] >> (6 - (srcX & 3) * 2)) & 0x3;
      if (invertedScreen) value = 3 - value;
      const size_t pos = static_cast<size_t>(ty) * w + tx;
      art.owned[pos >> 2] |= value << (6 - (pos & 3) * 2);
    }
  }

  art.width = w;
  art.height = h;
  art.offsetX = (boxW - w) / 2;
  art.offsetY = (boxH - h) / 2;
  art.packed = art.owned.get();
  return true;
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

// What drawBase drew, so flushGrayPasses nudges the same art.
bool baseIsCustom = false;

// The custom logo when /logo.bmp loads, the cat otherwise. Nothing is remembered between draws, so
// a logo that failed once is tried again next time.
bool loadArt(const GrayLogo::Size size, const bool invertedScreen, Art& art) {
  if (Storage.exists(CUSTOM_LOGO_PATH) && loadCustomArt(size, invertedScreen, art)) return true;
  art = Art{};
  return loadBuiltinArt(size, art);
}

}  // namespace

namespace GrayLogo {

void drawBase(const GfxRenderer& renderer, const int x, const int y, const bool invertedScreen, const Size size) {
  Art art;
  baseIsCustom = false;
  if (!loadArt(size, invertedScreen, art)) return;
  baseIsCustom = art.packed != nullptr;
  LogoCursor logo(art);
  const int left = x + art.offsetX;
  const int top = y + art.offsetY;
  for (int py = 0; py < art.height; py++) {
    for (int px = 0; px < art.width; px++) {
      const uint8_t symbol = logo.next();
      const uint8_t val = level(symbol);
      if (invertedScreen) {
        // The artwork keeps its own colours on the dark screen, set off by a white edge: post-invert
        // targets inside the footprint are white artwork -> white, black -> black, grays -> black
        // start state for the nudge; outside it the background stays black. Pre-invert that means
        // painting only the footprint's white pixels.
        if (val == 3 && inFootprint(symbol)) renderer.drawPixel(left + px, top + py, true);
      } else {
        // Same rule as GfxRenderer's grayscale bitmap base pass: everything but white starts
        // black; the nudge planes lighten the grays afterwards.
        if (val < 3) renderer.drawPixel(left + px, top + py, true);
      }
    }
  }
}

void drawBw(const GfxRenderer& renderer, const int x, const int y) {
  Art art;
  if (!loadArt(Size::Regular, false, art)) return;
  LogoCursor logo(art);
  const int left = x + art.offsetX;
  const int top = y + art.offsetY;
  for (int py = 0; py < art.height; py++) {
    for (int px = 0; px < art.width; px++) {
      // Threshold between the two grays: dark shading keeps its ink, light shading drops out.
      if (level(logo.next()) <= 1) renderer.drawPixel(left + px, top + py, true);
    }
  }
}

void flushGrayPasses(GfxRenderer& renderer, const int x, const int y, const bool invertedScreen, const Size size) {
  Art art;
  // Planes for other art would land on this base, so a load failure leaves the BW base as it is.
  if (!(baseIsCustom ? loadCustomArt(size, invertedScreen, art) : loadBuiltinArt(size, art))) {
    LOG_ERR("LOGO", "Logo art unavailable for the gray passes");
    renderer.setRenderMode(GfxRenderer::BW);
    return;
  }
  const int left = x + art.offsetX;
  const int top = y + art.offsetY;
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
      if (planeLevel(lsb.next()) == 1) renderer.drawPixel(left + px, top + py, false);
    }
  }
  renderer.copyGrayscaleLsbBuffers();

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  LogoCursor msb(art);
  for (int py = 0; py < art.height; py++) {
    for (int px = 0; px < art.width; px++) {
      const uint8_t val = planeLevel(msb.next());
      if (val == 1 || val == 2) renderer.drawPixel(left + px, top + py, false);
    }
  }
  renderer.copyGrayscaleMsbBuffers();

  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);
}

}  // namespace GrayLogo
