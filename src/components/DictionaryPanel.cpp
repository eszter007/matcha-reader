#include "DictionaryPanel.h"

#include <GfxRenderer.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "components/UITheme.h"
#include "components/icons/dictionaryIcons.h"
#include "fontIds.h"

namespace {

// Gap between the panel and the side edges of the usable area.
constexpr int SIDE_MARGIN = 20;

// Share of the screen height the panel occupies, bottom-anchored above the button hints.
constexpr int HEIGHT_PERCENT = 66;

// Floor under the gap above and below the panel, so it never reaches an edge on a short screen.
constexpr int MIN_TOP_MARGIN = 20;

// Extra air between the headword and the divider under it, on top of the usual half-padding.
constexpr int HEADWORD_GAP = 4;

// Inner padding from the frame to any text. The headword and the entry share it, so the two
// line up down the left edge and clear the frame by the same amount on the right.
constexpr int PADDING = 14;

// Stroke of the frame and of the divider under the headword.
constexpr int FRAME_STROKE = 2;
constexpr int DIVIDER_STROKE = 1;

// Fallback radius for themes that draw square popups: the panel is always rounded.
constexpr int MIN_RADIUS = 6;

// The headword is part of the entry, not part of the chrome, so it is set in the same serif the
// definition uses. A CJK headword is routed to a CJK face by the renderer's own font resolution.
constexpr int HEADWORD_FONT_ID = NOTOSERIF_12_FONT_ID;

// The save-sentence button: the icon's size, and the extra reach around it a finger gets.
constexpr int ADD_ICON_SIZE = 24;
constexpr int ADD_TAP_SLOP = 12;
// Space kept between the headword and the button, and between the footer label and the counter.
constexpr int TITLE_GAP = 8;
constexpr int FOOTER_GAP = 16;

int panelRadius(const ThemeMetrics& metrics) { return std::max(metrics.popupCornerRadius, MIN_RADIUS); }

// Copy `text` into `out`, trimmed to maxWidth with a trailing ellipsis when it does not fit.
// Cuts on UTF-8 codepoint boundaries so a multi-byte character is never split.
void ellipsize(const GfxRenderer& renderer, const int fontId, const char* text, const int maxWidth, char* out,
               const size_t outSize, const EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  const size_t len = strlen(text);
  if (len < outSize && renderer.getTextWidth(fontId, text, style) <= maxWidth) {
    memcpy(out, text, len + 1);
    return;
  }
  static constexpr char ELLIPSIS[] = "\xe2\x80\xa6";  // U+2026
  const int ellipsisWidth = renderer.getTextWidth(fontId, ELLIPSIS, style);
  size_t cut = std::min(len, outSize - sizeof(ELLIPSIS));
  while (cut > 0) {
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) cut--;  // codepoint boundary
    memcpy(out, text, cut);
    out[cut] = '\0';
    if (renderer.getTextWidth(fontId, out, style) + ellipsisWidth <= maxWidth) break;
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

  // Centred in the usable area. safe.height already stops at the button hints, so the panel is
  // centred over the page rather than over the whole screen -- which is what reads as centred,
  // since the hint band is chrome, not page.
  const int wanted = renderer.getScreenHeight() * HEIGHT_PERCENT / 100;
  layout.box.height = std::min(wanted, std::max(0, safe.height - 2 * MIN_TOP_MARGIN));
  layout.box.y = safe.y + (safe.height - layout.box.height) / 2;

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

  // Top-right corner, centred on the headword line. The tap target reaches past the icon so a
  // finger does not have to land on 24 pixels, but stays inside the frame.
  const int iconX = layout.box.x + layout.box.width - PADDING - ADD_ICON_SIZE;
  const int iconY = layout.box.y + PADDING + (headwordHeight - ADD_ICON_SIZE) / 2;
  const int tapLeft = iconX - ADD_TAP_SLOP;
  const int tapTop = std::max(layout.box.y, iconY - ADD_TAP_SLOP);
  const int tapRight = std::min(layout.box.x + layout.box.width, iconX + ADD_ICON_SIZE + ADD_TAP_SLOP);
  const int tapBottom = iconY + ADD_ICON_SIZE + ADD_TAP_SLOP;
  layout.addButton = Rect{tapLeft, tapTop, tapRight - tapLeft, tapBottom - tapTop};
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
                                              const char* counter, const char* kind, const bool addButton) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Layout layout = compute(renderer);
  const int radius = panelRadius(metrics);

  // Opaque fill: the panel floats over the reader's page, which is still in the framebuffer.
  renderer.fillRoundedRect(layout.box.x, layout.box.y, layout.box.width, layout.box.height, radius, Color::White);
  renderer.drawRoundedRect(layout.box.x, layout.box.y, layout.box.width, layout.box.height, FRAME_STROKE, radius, true);

  const int textX = layout.box.x + PADDING;
  const int headwordY = layout.box.y + PADDING;
  const int rightEdge = layout.box.x + layout.box.width - PADDING;
  // The headword stops short of the save button rather than running under it.
  const int headwordRight = addButton ? rightEdge - ADD_ICON_SIZE - TITLE_GAP : rightEdge;
  if (headword && headword[0] != '\0') {
    char buf[128];
    ellipsize(renderer, HEADWORD_FONT_ID, headword, headwordRight - textX, buf, sizeof(buf), EpdFontFamily::BOLD);
    renderer.drawText(HEADWORD_FONT_ID, textX, headwordY, buf, true, EpdFontFamily::BOLD);
  }
  if (addButton) {
    const int iconX = rightEdge - ADD_ICON_SIZE;
    const int iconY = headwordY + (renderer.getLineHeight(HEADWORD_FONT_ID) - ADD_ICON_SIZE) / 2;
    renderer.drawIcon(DictAddCardIcon, iconX, iconY, ADD_ICON_SIZE);
  }

  const int dividerY = headwordY + renderer.getLineHeight(HEADWORD_FONT_ID) + HEADWORD_GAP + PADDING / 2;
  renderer.drawLine(textX, dividerY, rightEdge, dividerY, DIVIDER_STROKE, true);

  const int footerY = layout.box.y + layout.box.height - PADDING - renderer.getLineHeight(SMALL_FONT_ID);
  renderer.drawLine(textX, footerY - PADDING / 2, rightEdge, footerY - PADDING / 2, DIVIDER_STROKE, true);
  int labelWidth = rightEdge - textX;
  if (counter && counter[0] != '\0') {
    const int counterWidth = renderer.getTextWidth(SMALL_FONT_ID, counter);
    renderer.drawText(SMALL_FONT_ID, rightEdge - counterWidth, footerY, counter);
    labelWidth -= counterWidth + FOOTER_GAP;  // the label clips before it can reach the counter
  }
  // "Vocab | JMdict | Tatoeba [1][2]": which index answered, then the dictionary. Dictionary
  // titles run long ("English-Deutsch FreeDict+WikDict dictionary (en-de)"), so the whole label
  // is clipped with an ellipsis rather than running into the counter.
  char label[160];
  label[0] = '\0';
  const bool hasKind = kind && kind[0] != '\0';
  const bool hasName = dictName && dictName[0] != '\0';
  snprintf(label, sizeof(label), "%s%s%s", hasKind ? kind : "", hasKind && hasName ? " | " : "",
           hasName ? dictName : "");
  if (label[0] != '\0' && labelWidth > 0) {
    char buf[160];
    ellipsize(renderer, SMALL_FONT_ID, label, labelWidth, buf, sizeof(buf));
    renderer.drawText(SMALL_FONT_ID, textX, footerY, buf);
  }
  return layout;
}
