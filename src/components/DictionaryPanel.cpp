#include "DictionaryPanel.h"

#include <GfxRenderer.h>

#include <algorithm>

#include "components/UITheme.h"
#include "fontIds.h"

namespace {

// Gap between the panel and the screen edge. Small on purpose: the panel is the content,
// the page behind it is context.
constexpr int SIDE_MARGIN = 8;

// Inner padding from the frame to any text.
constexpr int PADDING = 12;

// Stroke of the frame and of the divider under the headword.
constexpr int FRAME_STROKE = 2;
constexpr int DIVIDER_STROKE = 1;

// Share of the screen height the panel occupies, bottom-anchored just above the button
// hints. Matches the proportions of the reference design.
constexpr int HEIGHT_PERCENT = 66;

// Fallback radius for themes that draw square popups: the panel is always rounded.
constexpr int MIN_RADIUS = 6;

int panelRadius(const ThemeMetrics& metrics) { return std::max(metrics.popupCornerRadius, MIN_RADIUS); }

}  // namespace

DictionaryPanel::Layout DictionaryPanel::compute(const GfxRenderer& renderer) {
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  Layout layout;
  layout.box.x = safe.x + SIDE_MARGIN;
  layout.box.width = std::max(0, safe.width - 2 * SIDE_MARGIN);

  const int bottom = safe.y + safe.height;
  const int wanted = renderer.getScreenHeight() * HEIGHT_PERCENT / 100;
  layout.box.height = std::min(wanted, safe.height);
  layout.box.y = bottom - layout.box.height;

  // Headword line, then the divider, then the body; the dictionary name closes the panel.
  const int headwordHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int footerHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int bodyTop = layout.box.y + PADDING + headwordHeight + PADDING / 2 + DIVIDER_STROKE + PADDING / 2;
  const int bodyBottom = layout.box.y + layout.box.height - PADDING - footerHeight;

  layout.body.x = layout.box.x + PADDING;
  layout.body.width = std::max(0, layout.box.width - 2 * PADDING);
  layout.body.y = bodyTop;
  layout.body.height = std::max(0, bodyBottom - bodyTop);
  return layout;
}

DictionaryPanel::Layout DictionaryPanel::draw(const GfxRenderer& renderer, const char* headword, const char* dictName,
                                              const char* counter) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Layout layout = compute(renderer);
  const int radius = panelRadius(metrics);

  // Opaque fill: the panel floats over the reader's page, which is still in the framebuffer.
  renderer.fillRoundedRect(layout.box.x, layout.box.y, layout.box.width, layout.box.height, radius, Color::White);
  renderer.drawRoundedRect(layout.box.x, layout.box.y, layout.box.width, layout.box.height, FRAME_STROKE, radius, true);

  const int textX = layout.box.x + PADDING;
  const int headwordY = layout.box.y + PADDING;
  if (headword && headword[0] != '\0') {
    renderer.drawText(UI_12_FONT_ID, textX, headwordY, headword, true, EpdFontFamily::BOLD);
  }
  if (counter && counter[0] != '\0') {
    const int counterWidth = renderer.getTextWidth(UI_10_FONT_ID, counter);
    renderer.drawText(UI_10_FONT_ID, layout.box.x + layout.box.width - PADDING - counterWidth, headwordY, counter);
  }

  const int dividerY = headwordY + renderer.getLineHeight(UI_12_FONT_ID) + PADDING / 2;
  renderer.drawLine(textX, dividerY, layout.box.x + layout.box.width - PADDING, dividerY, DIVIDER_STROKE, true);

  if (dictName && dictName[0] != '\0') {
    const int footerY = layout.box.y + layout.box.height - PADDING - renderer.getLineHeight(UI_10_FONT_ID);
    renderer.drawText(UI_10_FONT_ID, textX, footerY, dictName);
  }
  return layout;
}
