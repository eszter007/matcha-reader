#pragma once

#include <cstdint>

class GfxRenderer;

// Draws the 4-level grayscale boot/sleep logo using the same two-plane
// grayscale nudge pipeline the sleep-cover BMP path uses (see SleepActivity::
// renderBitmapSleepScreen). drawImage can't be used: it writes 1-bit panel-native bytes and
// ignores the grayscale render modes entirely.
//
// The logo is logo.bmp in the SD card's system folder (sdsystem::dir(), beside the reading stats) when that file loads,
// scaled down to fit and centred in the WIDTH x HEIGHT (or SMALL_*) box, and the cat (images/CatLogo.h) otherwise.
//
// Usage (mirrors the cover flow):
//   renderer.clearScreen();
//   GrayLogo::drawBase(renderer, x, y, inverted);
//   ...draw any text...
//   if (inverted) renderer.invertScreen();
//   renderer.displayGrayscaleBase(<refresh fallback>);
//   GrayLogo::flushGrayPasses(renderer, x, y, inverted);   // ends back in BW render mode
// drawBase keeps the decoded art until flushGrayPasses uses and frees it, so call the two as a pair.
//
// invertedScreen: pass true when the caller invertScreen()s the frame before display (dark
// sleep mode). The artwork is not inverted with the screen: inside its footprint (the cat, the
// books and a white edge around them) it keeps its own black, white and grays, gray pixels
// starting black post-invert -- the calibrated start state the gray nudge expects. A custom logo
// gets the same treatment: its footprint is computed from its white background on load.
namespace GrayLogo {
constexpr int WIDTH = 230;
constexpr int HEIGHT = 324;
// The smaller copy, for the Departures sleep screen on short panels.
constexpr int SMALL_WIDTH = 130;
constexpr int SMALL_HEIGHT = 183;

enum class Size : uint8_t { Regular, Small };
// Wordmark and status line sit at the bottom of the screen, under the poster-size cat.
constexpr int WORDMARK_FROM_BOTTOM = 96;
constexpr int STATUS_FROM_BOTTOM = 62;
// Logo top: a little above the centre, and never closer than 12 px to the wordmark (landscape is
// only 480 px tall).
constexpr int top(const int pageHeight) {
  const int centred = pageHeight * 45 / 100 - HEIGHT / 2;
  const int lowest = pageHeight - WORDMARK_FROM_BOTTOM - 12 - HEIGHT;
  return centred < lowest ? centred : (lowest > 0 ? lowest : 0);
}

void drawBase(const GfxRenderer& renderer, int x, int y, bool invertedScreen, Size size = Size::Regular);
// Plain 1-bit rendering (dark gray -> black, light gray -> white), no gray passes needed.
// Used on the boot screen: wake-from-sleep reboots through it, so it must stay fast, and its
// full refresh is what wipes the sleep screen's gray charge states -- putting gray-nudged
// pixels on the boot screen itself made every unlock slow AND left the logo ghosting through
// the next activity's partial refresh.
void drawBw(const GfxRenderer& renderer, int x, int y);
// Call AFTER displayGrayscaleBase(). Destroys the BW framebuffer contents (plane scratch),
// same as the sleep-cover flow -- callers redraw from scratch anyway. Restores BW render mode.
void flushGrayPasses(GfxRenderer& renderer, int x, int y, bool invertedScreen, Size size = Size::Regular);
}  // namespace GrayLogo
