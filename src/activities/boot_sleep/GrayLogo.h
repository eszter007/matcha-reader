#pragma once

class GfxRenderer;

// Draws the 4-level grayscale boot/sleep logo (images/Logo160.h) using the same two-plane
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
// sleep mode). The base then only paints the artwork's black pixels (so they end up white on
// the black screen), leaves gray pixels black post-invert -- the calibrated start state the
// gray nudge expects -- and the nudge planes swap dark/light gray so shading reads correctly
// on the inverted background.
namespace GrayLogo {
constexpr int SIZE = 160;

void drawBase(GfxRenderer& renderer, int x, int y, bool invertedScreen);
// Plain 1-bit rendering (dark gray -> black, light gray -> white), no gray passes needed.
// Used on the boot screen: wake-from-sleep reboots through it, so it must stay fast, and its
// full refresh is what wipes the sleep screen's gray charge states -- putting gray-nudged
// pixels on the boot screen itself made every unlock slow AND left the logo ghosting through
// the next activity's partial refresh.
void drawBw(GfxRenderer& renderer, int x, int y);
// Call AFTER displayGrayscaleBase(). Destroys the BW framebuffer contents (plane scratch),
// same as the sleep-cover flow -- callers redraw from scratch anyway. Restores BW render mode.
void flushGrayPasses(GfxRenderer& renderer, int x, int y, bool invertedScreen);
}  // namespace GrayLogo
