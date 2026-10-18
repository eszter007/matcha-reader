#include "GrayLogo.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <InflateReader.h>
#include <Logging.h>
#include <Memory.h>
#include <SdSystemDir.h>

#include <algorithm>
#include <cstring>
#include <memory>

#include "images/CatLogo.h"

static_assert(GrayLogo::WIDTH == CAT_LOGO_WIDTH && GrayLogo::HEIGHT == CAT_LOGO_HEIGHT,
              "GrayLogo dimensions must match the generated bitmap");
static_assert(GrayLogo::SMALL_WIDTH == CAT_LOGO_SMALL_WIDTH && GrayLogo::SMALL_HEIGHT == CAT_LOGO_SMALL_HEIGHT,
              "GrayLogo small dimensions must match the generated bitmap");

namespace {

constexpr char CUSTOM_LOGO_NAME[] = "logo.bmp";  // in the SD card's system folder
// Every source row is read on every draw (boot decodes once, sleep twice), so cap the read at
// roughly one screen.
constexpr int CUSTOM_LOGO_MAX_SIDE = 800;
constexpr uint8_t FOOTPRINT = 0x4;
// White border kept around a custom logo; on the dark screen it becomes the sticker edge.
constexpr int STICKER_EDGE = 4;
// The background fill converges in a few sweeps for real drawings; the cap bounds a pathological one.
constexpr int MAX_FILL_SWEEPS = 64;

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

bool maskBit(const uint8_t* mask, const size_t i) { return mask[i >> 3] & (0x80 >> (i & 7)); }
void setMaskBit(uint8_t* mask, const size_t i) { mask[i >> 3] |= 0x80 >> (i & 7); }
uint8_t packedLevel(const uint8_t* packed, const size_t i) { return (packed[i >> 2] >> (6 - (i & 3) * 2)) & 0x3; }

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
  const int w = art.width;
  const int h = art.height;
  const size_t count = static_cast<size_t>(w) * h;
  const size_t maskSize = (count + 7) / 8;
  // Two 1-bit masks, at most 230 * 324 / 8 = 9,315 bytes each; the scratch one is freed on return.
  auto outside = makeUniqueNoThrow<uint8_t[]>(maskSize);
  auto scratch = makeUniqueNoThrow<uint8_t[]>(maskSize);
  if (!outside || !scratch) {
    LOG_ERR("LOGO", "OOM: 2 x %u bytes for the logo footprint", static_cast<unsigned>(maskSize));
    return false;
  }
  memset(outside.get(), 0, maskSize);
  const uint8_t* packed = art.owned.get();
  const auto white = [packed](const size_t i) { return packedLevel(packed, i) == 3; };

  // Background: white pixels connected to the image border. Alternate forward and backward sweeps
  // until nothing changes, which needs no fill stack.
  for (int x = 0; x < w; x++) {
    if (white(x)) setMaskBit(outside.get(), x);
    const size_t bottom = static_cast<size_t>(h - 1) * w + x;
    if (white(bottom)) setMaskBit(outside.get(), bottom);
  }
  for (int y = 0; y < h; y++) {
    const size_t row = static_cast<size_t>(y) * w;
    if (white(row)) setMaskBit(outside.get(), row);
    if (white(row + w - 1)) setMaskBit(outside.get(), row + w - 1);
  }
  bool changed = true;
  for (int sweep = 0; changed && sweep < MAX_FILL_SWEEPS; sweep++) {
    changed = false;
    for (size_t i = 0; i < count; i++) {
      if (maskBit(outside.get(), i) || !white(i)) continue;
      const int x = static_cast<int>(i % w);
      if ((x > 0 && maskBit(outside.get(), i - 1)) || (i >= static_cast<size_t>(w) && maskBit(outside.get(), i - w))) {
        setMaskBit(outside.get(), i);
        changed = true;
      }
    }
    for (size_t i = count; i-- > 0;) {
      if (maskBit(outside.get(), i) || !white(i)) continue;
      const int x = static_cast<int>(i % w);
      if ((x < w - 1 && maskBit(outside.get(), i + 1)) || (i + w < count && maskBit(outside.get(), i + w))) {
        setMaskBit(outside.get(), i);
        changed = true;
      }
    }
  }

  // The footprint is the rest; grow it one pixel per step, alternating 4- and 8-neighbour steps so the
  // edge comes out rounded rather than square.
  for (size_t b = 0; b < maskSize; b++) outside[b] = ~outside[b];
  uint8_t* footprint = outside.get();
  for (int step = 0; step < STICKER_EDGE; step++) {
    memcpy(scratch.get(), footprint, maskSize);
    const bool diagonals = step % 2 == 1;
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        const size_t i = static_cast<size_t>(y) * w + x;
        if (maskBit(scratch.get(), i)) continue;
        bool grow = false;
        for (int dy = -1; dy <= 1 && !grow; dy++) {
          for (int dx = -1; dx <= 1 && !grow; dx++) {
            if ((dx == 0 && dy == 0) || (!diagonals && dx != 0 && dy != 0)) continue;
            const int nx = x + dx;
            const int ny = y + dy;
            if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
            grow = maskBit(scratch.get(), static_cast<size_t>(ny) * w + nx);
          }
        }
        if (grow) setMaskBit(footprint, i);
      }
    }
  }
  art.ownedFootprint = std::move(outside);
  art.footprint = art.ownedFootprint.get();
  return true;
}

// Decodes logo.bmp, scaled down (never up) to fit the logo box with a STICKER_EDGE white border.
// On the dark screen it keeps its own colours inside its footprint, like the cat.
bool loadCustomArt(const GrayLogo::Size size, const bool invertedScreen, Art& art) {
  const std::string path = sdsystem::findUserFile(CUSTOM_LOGO_NAME);
  if (!Storage.exists(path.c_str())) return false;
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

  const int boxW = (size == GrayLogo::Size::Regular ? GrayLogo::WIDTH : GrayLogo::SMALL_WIDTH) - 2 * STICKER_EDGE;
  const int boxH = (size == GrayLogo::Size::Regular ? GrayLogo::HEIGHT : GrayLogo::SMALL_HEIGHT) - 2 * STICKER_EDGE;
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

  // The art canvas adds the white border on every side.
  const int canvasW = w + 2 * STICKER_EDGE;
  const int canvasH = h + 2 * STICKER_EDGE;
  // Packed art: at most 230 * 324 / 4 = 18,630 bytes. Rows: one 2-bit output row plus one raw row
  // (at most 800 px * 4 bytes). Both live only for this draw.
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
    const auto rowResult = bitmap.readNextRow(outRow, rawRow);
    if (rowResult != BmpReaderError::Ok) {
      LOG_ERR("LOGO", "Reading %s failed: %s", path.c_str(), Bitmap::errorToString(rowResult));
      return false;
    }
    // Nearest-neighbour: target row ty samples source row ty * srcH / h. Since h <= srcH, each
    // source row feeds at most one target row.
    const int srcY = bitmap.isTopDown() ? fileRow : srcH - 1 - fileRow;
    const int ty = (srcY * h + srcH - 1) / srcH;
    if (ty >= h || ty * srcH / h != srcY) continue;
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

// What drawBase drew, so flushGrayPasses nudges the same art.
bool baseIsCustom = false;

// The custom logo when logo.bmp loads, the cat otherwise. Nothing is remembered between draws, so
// a logo that failed once is tried again next time.
bool loadArt(const GrayLogo::Size size, const bool invertedScreen, Art& art) {
  if (loadCustomArt(size, invertedScreen, art)) return true;
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
  // Planes for other art would land on this base, so a load failure leaves the BW base as it is.
  if (!(baseIsCustom ? loadCustomArt(size, invertedScreen, art) : loadBuiltinArt(size, art))) {
    LOG_ERR("LOGO", "Logo art unavailable for the gray passes");
    renderer.setRenderMode(GfxRenderer::BW);
    return;
  }
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
