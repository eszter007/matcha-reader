#include "DictionaryPanel.h"

#include <GfxRenderer.h>

#include <algorithm>
#include <cstring>

#include "components/UITheme.h"
#include "fontIds.h"

namespace {

// Gap between the panel and the edges of the usable area. The bottom margin measures to the top
// of the button hints, not to the physical edge, so the panel never crowds them.
constexpr int SIDE_MARGIN = 20;
constexpr int TOP_MARGIN = 60;
constexpr int BOTTOM_MARGIN = 40;

// Extra air between the headword and the divider under it, on top of the usual half-padding.
constexpr int HEADWORD_GAP = 4;

// Inner padding from the frame to any text. Deliberately tight: the panel is small and every
// pixel spent on padding is a line of definition the reader does not get.
constexpr int PADDING = 8;

// Stroke of the frame and of the divider under the headword.
constexpr int FRAME_STROKE = 2;
constexpr int DIVIDER_STROKE = 1;

// Fallback radius for themes that draw square popups: the panel is always rounded.
constexpr int MIN_RADIUS = 6;

// The headword is part of the entry, not part of the chrome, so it is set in the same serif the
// definition uses. A CJK headword is routed to a CJK face by the renderer's own font resolution.
constexpr int HEADWORD_FONT_ID = NOTOSERIF_12_FONT_ID;

int panelRadius(const ThemeMetrics& metrics) { return std::max(metrics.popupCornerRadius, MIN_RADIUS); }

// Copy `text` into `out`, trimmed to maxWidth with a trailing ellipsis when it does not fit.
// Cuts on UTF-8 codepoint boundaries so a multi-byte character is never split.
void ellipsize(const GfxRenderer& renderer, const char* text, const int maxWidth, char* out, const size_t outSize) {
  const size_t len = strlen(text);
  if (len < outSize && renderer.getTextWidth(SMALL_FONT_ID, text) <= maxWidth) {
    memcpy(out, text, len + 1);
    return;
  }
  static constexpr char ELLIPSIS[] = "\xe2\x80\xa6";  // U+2026
  const int ellipsisWidth = renderer.getTextWidth(SMALL_FONT_ID, ELLIPSIS);
  size_t cut = std::min(len, outSize - sizeof(ELLIPSIS));
  while (cut > 0) {
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) cut--;  // codepoint boundary
    memcpy(out, text, cut);
    out[cut] = '\0';
    if (renderer.getTextWidth(SMALL_FONT_ID, out) + ellipsisWidth <= maxWidth) break;
    cut--;
  }
  memcpy(out + cut, ELLIPSIS, sizeof(ELLIPSIS));
}

}  // namespace

DictionaryPanel::Layout DictionaryPanel::compute(const GfxRenderer& renderer) {
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  Layout layout;
  layout.box.x = safe.x + SIDE_MARGIN;
  layout.box.width = std::max(0, safe.width - 2 * SIDE_MARGIN);

  // safe.height already stops at the button hints, so insetting it bottoms the panel above them.
  layout.box.y = safe.y + TOP_MARGIN;
  layout.box.height = std::max(0, safe.height - TOP_MARGIN - BOTTOM_MARGIN);

  // Headword line, then the divider, then the body; a second divider and the dictionary name
  // close the panel.
  const int headwordHeight = renderer.getLineHeight(HEADWORD_FONT_ID);
  const int footerHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int bodyTop =
      layout.box.y + PADDING + headwordHeight + HEADWORD_GAP + PADDING / 2 + DIVIDER_STROKE + PADDING / 2;
  const int bodyBottom = layout.box.y + layout.box.height - PADDING - footerHeight - PADDING / 2 - DIVIDER_STROKE;

  layout.body.x = layout.box.x + PADDING;
  layout.body.width = std::max(0, layout.box.width - 2 * PADDING);
  layout.body.y = bodyTop;
  layout.body.height = std::max(0, bodyBottom - bodyTop);
  return layout;
}

void DictionaryPanel::clearButtonHints(const GfxRenderer& renderer) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Inverted portrait puts the front-button hints along the top edge instead of the bottom.
  const bool isInverted = renderer.getOrientation() == GfxRenderer::Orientation::PortraitInverted;
  const int bandY = isInverted ? 0 : renderer.getScreenHeight() - metrics.buttonHintsHeight;
  renderer.fillRect(0, bandY, renderer.getScreenWidth(), metrics.buttonHintsHeight, false);
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
    renderer.drawText(HEADWORD_FONT_ID, textX, headwordY, headword, true, EpdFontFamily::BOLD);
  }
  const int rightEdge = layout.box.x + layout.box.width - PADDING;
  if (counter && counter[0] != '\0') {
    // Baseline-aligned with the headword but a size down: the counter is a reference, not a label.
    const int counterWidth = renderer.getTextWidth(SMALL_FONT_ID, counter);
    const int counterY = headwordY + renderer.getLineHeight(HEADWORD_FONT_ID) - renderer.getLineHeight(SMALL_FONT_ID);
    renderer.drawText(SMALL_FONT_ID, rightEdge - counterWidth, counterY, counter);
  }

  const int dividerY = headwordY + renderer.getLineHeight(HEADWORD_FONT_ID) + HEADWORD_GAP + PADDING / 2;
  renderer.drawLine(textX, dividerY, rightEdge, dividerY, DIVIDER_STROKE, true);

  const int footerY = layout.box.y + layout.box.height - PADDING - renderer.getLineHeight(SMALL_FONT_ID);
  renderer.drawLine(textX, footerY - PADDING / 2, rightEdge, footerY - PADDING / 2, DIVIDER_STROKE, true);
  if (dictName && dictName[0] != '\0') {
    // Dictionary titles run long ("English-Deutsch FreeDict+WikDict dictionary (en-de)"); clip to
    // the panel with an ellipsis rather than letting the text run under the frame.
    char buf[96];
    ellipsize(renderer, dictName, layout.box.width - 2 * PADDING, buf, sizeof(buf));
    renderer.drawText(SMALL_FONT_ID, textX, footerY, buf);
  }
  return layout;
}
