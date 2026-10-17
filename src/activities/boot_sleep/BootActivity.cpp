#include "BootActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "GrayLogo.h"
#include "fontIds.h"

void BootActivity::onEnter() {
  Activity::onEnter();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  // BW on purpose -- see GrayLogo::drawBw. The full refresh here also clears any gray charge
  // states left by the (grayscale) sleep screen when waking reboots through this activity.
  const int logoY = GrayLogo::top(pageHeight);
  GrayLogo::drawBw(renderer, (pageWidth - GrayLogo::WIDTH) / 2, logoY);
  renderer.drawCenteredText(UI_10_FONT_ID, logoY + GrayLogo::HEIGHT + 14, tr(STR_CROSSPOINT), true,
                            EpdFontFamily::BOLD);
  renderer.drawCenteredText(SMALL_FONT_ID, logoY + GrayLogo::HEIGHT + 39, tr(STR_BOOTING));
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 30, CROSSPOINT_VERSION);
  renderer.displayBuffer();
}
