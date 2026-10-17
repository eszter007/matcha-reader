#pragma once

class GfxRenderer;

// Draws the 4-level grayscale boot/sleep logo (images/CatLogo.h) using the same two-plane
// grayscale nudge pipeline the sleep-cover BMP path uses (see SleepActivity::
// renderBitmapSleepScreen). drawImage can't be used: it writes 1-bit panel-native bytes and
// ignores the grayscale render modes entirely.
//
// Usage (mirrors the cover flow):
//   renderer.clearScreen();
//   GrayLogo::drawBase(renderer, x, y, inverted);
//   ...draw any text...
//   if (inverted) renderer.invertScreen();
//   renderer.displayGrayscaleBase(<refresh fallback>);
//   GrayLogo::flushGrayPasses(renderer, x, y, inverted);   // ends back in BW render mode
//
// invertedScreen: pass true when the caller invertScreen()s the frame before display (dark
// sleep mode). The artwork is not inverted with the screen: inside its footprint (the cat, the
// books and a white edge around them) it keeps its own black, white and grays, gray pixels
// starting black post-invert -- the calibrated start state the gray nudge expects.
namespace GrayLogo {
constexpr int WIDTH = 130;
constexpr int HEIGHT = 187;
// Logo top for a screen of this height: the logo and the two text lines under it, centred as one.
constexpr int top(const int pageHeight) { return (pageHeight - HEIGHT - 60) / 2; }

void drawBase(const GfxRenderer& renderer, int x, int y, bool invertedScreen);
// Plain 1-bit rendering (dark gray -> black, light gray -> white), no gray passes needed.
// Used on the boot screen: wake-from-sleep reboots through it, so it must stay fast, and its
// full refresh is what wipes the sleep screen's gray charge states -- putting gray-nudged
// pixels on the boot screen itself made every unlock slow AND left the logo ghosting through
// the next activity's partial refresh.
void drawBw(const GfxRenderer& renderer, int x, int y);
// Call AFTER displayGrayscaleBase(). Destroys the BW framebuffer contents (plane scratch),
// same as the sleep-cover flow -- callers redraw from scratch anyway. Restores BW render mode.
void flushGrayPasses(GfxRenderer& renderer, int x, int y, bool invertedScreen);
}  // namespace GrayLogo
