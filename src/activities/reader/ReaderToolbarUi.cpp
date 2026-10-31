#include "ReaderToolbarUi.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "components/icons/readerDockIcons.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_DISMISS = 1;   // tap anywhere on the page above the sheet
constexpr fui::ActionId ACTION_TOOL = 2;      // value = tool index (Contents, Text, Translate, More)
constexpr fui::ActionId ACTION_PREV = 3;      // scrub row: previous chapter
constexpr fui::ActionId ACTION_NEXT = 4;      // scrub row: next chapter
constexpr fui::ActionId ACTION_SCRUB = 5;     // progress track: dragPermille along the book
constexpr fui::ActionId ACTION_ROW = 6;       // panel list row / Text row, value = row index
constexpr fui::ActionId ACTION_BOOKMARK = 7;  // bookmark toggle beside the page number
constexpr fui::ActionId ACTION_OPTION = 8;    // control row option, value = row * kOptionStride + option

constexpr int16_t kIcon = 24;
// Scrub row: icon-only chapter steps flanking a thin progress track with a round knob.
constexpr int16_t kScrubButton = 44;  // chapter step tap target (square)
constexpr int16_t kScrubKnob = 16;    // round knob on the 2px progress track
constexpr int16_t kScrubGap = 4;      // air between the step targets and the track
constexpr int16_t kBookmarkHit = 44;
// The dock's ink edges, from its band edges: where the chapter-step chevrons' ink starts and ends
// (a 24 px icon centred in a 44 px target, and Tabler's 6 px of air inside the glyph). The
// chapter title, the bookmark and the tools line up on them.
constexpr int16_t kEdgeInk = (kScrubButton - kIcon) / 2 + 6;
// Where the bookmark glyph's ink ends inside its 24 px box.
constexpr int16_t kBookmarkInkRight = 19;
// Tool row: icon over name; the open tool's name is bold and underlined.
constexpr int16_t kToolRowH = 76;
constexpr int16_t kToolUnderline = 3;
// Text panel control rows.
constexpr int16_t kTextRowH = 64;
constexpr int16_t kOptionW = 48;
constexpr int16_t kOptionH = 40;
constexpr int16_t kOptionGap = 4;
constexpr int16_t kStepW = 48;
constexpr int16_t kStepValueW = 48;
// Page panel: one setting per line, its page pictures right-aligned.
constexpr int16_t kThumbW = 44;
constexpr int16_t kThumbH = 58;
constexpr int16_t kThumbGap = 18;
constexpr int16_t kGroupH = 96;
// Air between the grey cursor band and what it marks, on every side.
constexpr int16_t kCursorPad = 6;
// Bottom sheet height for the panels. ListNav fits whole rows in the remaining
// list area; any spare pixels stay between the list and the switcher.
constexpr int kPanelHeightPercent = 62;
// Cap the sheet may grow to when rounding the list area up to a whole row.
constexpr int kPanelHeightMaxPercent = 72;
// The Text panel and More show every row where they fit, up to this share.
constexpr int kTextPanelHeightMaxPercent = 86;
// Landscape has less vertical room; leave a narrow page strip for tap-to-dismiss.
constexpr int kLandscapePanelHeightPercent = 88;

fui::BitmapRef icon24(const uint8_t* bits) { return fui::BitmapRef{bits, kIcon, kIcon, fui::BitmapFormat::Mask1}; }

const fui::Paint& ink() {
  static const fui::Paint paint = fui::Paint::solid(fui::Color::Black);
  return paint;
}

const fui::Paint& paper() {
  static const fui::Paint paint = fui::Paint::solid(fui::Color::White);
  return paint;
}

// A line of `w` x `h` px; pictures are built from these on a 24 px grid, 2 px strokes like Tabler's.
void bar(fui::DrawTarget& t, const fui::Paint& p, const int x, const int y, const int w, const int h) {
  t.fill(fui::Rect{static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<int16_t>(w), static_cast<int16_t>(h)},
         p);
}

void frame(fui::DrawTarget& t, const fui::Paint& p, const int x, const int y, const int w, const int h) {
  t.stroke(
      fui::Rect{static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<int16_t>(w), static_cast<int16_t>(h)}, p,
      2, 3);
}

// The picture for option `k` of a page-setting group: a small page whose text lines show the
// setting, drawn into `r` (kThumbW x kThumbH, or wide for a landscape orientation).
void drawPage(fui::DrawTarget& t, const ReaderToolbarUi::TextRow::Picture picture, const int k, const fui::Rect r) {
  using Picture = ReaderToolbarUi::TextRow::Picture;
  const fui::Paint& p = ink();
  frame(t, p, r.x, r.y, r.width, r.height);
  // The text block inside the page.
  // Few, thin lines, far enough apart that each step reads as different.
  int inset = 8;
  int gap = 8;
  if (picture == Picture::Margins) inset = 5 + k * 5;
  if (picture == Picture::LineSpacing) gap = 5 + k * 3;
  const int left = r.x + inset;
  const int width = r.width - 2 * inset;
  if (picture == Picture::Writing && k == 1) {
    // Columns, read from the right.
    for (int cx = r.x + r.width - inset - 1; cx >= left; cx -= 8) bar(t, p, cx, r.y + 8, 1, r.height - 16);
    return;
  }
  if (picture == Picture::Alignment) {
    // Lines at the usual spacing, the ragged ones clearly short: 0 justify, 1 left, 2 centre, 3 right.
    static constexpr int kPercent[4] = {100, 55, 80, 65};
    int i = 0;
    for (int y = r.y + 8; y <= r.y + r.height - 9; y += 8, ++i) {
      const int w = k == 0 ? width : width * kPercent[i % 4] / 100;
      const int x = k == 1 ? left : (k == 2 ? left + (width - w) / 2 : (k == 3 ? left + width - w : left));
      bar(t, p, x, y, w, 1);
    }
    return;
  }
  // Orientation: a tall or wide page (the caller sizes `r`); 2 and 3 are the same pages turned
  // round, which puts the paragraph's short last line at the top right.
  const bool turned = picture == Picture::Orientation && k >= 2;
  if (picture == Picture::Orientation) {
    // A header bar marks the page's top edge: at the bottom once the page is turned round.
    bar(t, p, r.x + 4, turned ? r.y + r.height - 7 : r.y + 4, r.width - 8, 3);
  }
  const int top = r.y + (picture == Picture::Orientation && !turned ? 12 : 8);
  const int bottom = r.y + r.height - (picture == Picture::Orientation && turned ? 13 : 9);
  int lines = 0;
  for (int y = top; y <= bottom; y += gap) ++lines;
  int line = 0;
  for (int y = top; y <= bottom; y += gap, ++line) {
    int w = width;
    int x = left;
    if (picture == Picture::Orientation && line == (turned ? 0 : lines - 1)) {
      w = width * 3 / 5;
      x = turned ? left + width - w : left;
    }
    bar(t, p, x, y, w, 1);
  }
}

// The button cursor: the light grey band the lists use for theirs, drawn under the content.
void cursor(fui::DrawTarget& t, const fui::Rect r) {
  constexpr uint8_t kCursorRadius = 8;  // one radius for every highlight, whatever the theme's
  t.fill(r, fui::Paint::solid(fui::Color::LightGray), kCursorRadius);
}

// Text vertically centred in `rect`.
void centredText(fui::DrawTarget& t, const fui::Rect rect, const char* s, const fui::TextStyle& style) {
  if (!s) return;
  const int16_t lh = t.lineHeight(style.font);
  t.text(fui::Rect{rect.x, static_cast<int16_t>(rect.y + (rect.height - lh) / 2), rect.width, lh}, s, style);
}
}  // namespace

ReaderToolbarUi::ReaderToolbarUi(GfxRenderer& renderer) : UiAppHost(renderer) {}

void ReaderToolbarUi::begin() {
  resetUi();
  pending_ = Routed{};
  nav_.reset();
  for (fui::ActionId id = ACTION_DISMISS; id <= ACTION_OPTION; ++id) {
    app.on(id, &ReaderToolbarUi::onAction, this);
  }
  app.setScreen(&ReaderToolbarUi::screenFn, this);
}

void ReaderToolbarUi::render() {
  renderUi();
  // Wrapped rows can fit fewer than the fixed-height estimate; the nav then
  // advances the viewport after layout and asks for a rebuild. Converges (top
  // only moves forward toward the selection); the bound is a backstop.
  for (int pass = 0; pass < 3 && nav_.consumeRebuildNeeded(); ++pass) renderUi();
}

ReaderToolbarUi::Routed ReaderToolbarUi::route(const MappedInputManager& input) {
  pending_ = Routed{};
  // routeHeld: the scrub track is a drag target, so held frames must reach it.
  const auto touch = routeTouch(input, false, /*routeHeld=*/true);
  pending_.routed = touch.routed;
  pending_.x = touch.snap.touchX;
  pending_.y = touch.snap.touchY;
  // Only the release commits a scrub: every held frame dispatches too
  // (dragPermille set), and re-paginating a chapter per frame would be seconds
  // of work per swipe.
  if (pending_.event == Event::Scrub && !touch.snap.touchReleased) pending_.event = Event::None;
  return pending_;
}

void ReaderToolbarUi::onAction(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<ReaderToolbarUi*>(user);
  Routed& out = self->pending_;
  out.value = event.value;
  out.permille = event.dragPermille;
  if (event.action >= ACTION_DISMISS && event.action <= ACTION_OPTION) out.event = static_cast<Event>(event.action);
  if (out.event == Event::Scrub && event.dragPermille < 0) out.event = Event::None;
  // A handled action repaints through the reader's own fast path, not through
  // app.invalidate(): the page underneath is the reader's to draw.
  self->app.clearTapFlash();
}

void ReaderToolbarUi::screenFn(UiScreen& screen, void* user) {
  auto* self = static_cast<ReaderToolbarUi*>(user);
  if (self->model_.panel) {
    self->buildPanel(screen);
  } else {
    self->buildToolbar(screen);
  }
}

// Contents / Text / Page / Translate / More: five equal slots, each an icon over its name. The open
// panel's name is bold and underlined; on the dock a button cursor outlines its slot instead.
void ReaderToolbarUi::buildToolRow(UiScreen& screen, const fui::LayoutAnchor anchor, const int16_t sideInset) {
  const auto& tokens = screen.theme();
  static const uint8_t* const kIcons[kToolCount] = {DockContentsIcon, DockTextIcon, DockPageIcon, DockTranslateIcon,
                                                    DockMoreIcon};
  static constexpr StrId kNames[kToolCount] = {StrId::STR_TOOL_CONTENTS, StrId::STR_TOOL_TEXT, StrId::STR_TOOL_PAGE,
                                               StrId::STR_TRANSLATE, StrId::STR_TOOL_MORE};
  // sideInset absorbs the difference between the two hosts' content bands (the toolbar's is
  // inset, the panel's is full width): the tools must land on the same x either way, or they jump
  // when a tap swaps the toolbar for a panel.
  const fui::Rect row = screen.take(anchor, kToolRowH).inset(fui::Insets{0, sideInset, 0, sideInset});
  auto& t = screen.target();
  fui::TextStyle nameStyle = tokens.smallText;
  nameStyle.align = fui::TextAlign::Center;
  const int16_t nameH = t.lineHeight(nameStyle.font);
  const int16_t blockH = static_cast<int16_t>(kIcon + tokens.spaceSm + nameH + kToolUnderline + 2);
  // The first tool's ink starts and the last one's ends on the dock's ink edges, where the chapter
  // steps, the chapter title and the bookmark sit.
  int16_t blockW[kToolCount];
  for (int i = 0; i < kToolCount; ++i) {
    blockW[i] = std::max<int16_t>(kIcon, t.measureText(nameStyle.font, I18N.get(kNames[i]), nameStyle).width);
  }
  const int16_t inkLeft = static_cast<int16_t>(row.x + kEdgeInk);
  const int16_t inkRight = static_cast<int16_t>(row.right() - kEdgeInk);
  // The end tools sit on the edges; the centres between are evenly spaced, so the icons fall on a
  // regular beat whatever the names' widths.
  int16_t centres[kToolCount];
  centres[0] = static_cast<int16_t>(inkLeft + blockW[0] / 2);
  centres[kToolCount - 1] = static_cast<int16_t>(inkRight - blockW[kToolCount - 1] / 2);
  for (int i = 1; i < kToolCount - 1; ++i) {
    centres[i] = static_cast<int16_t>(centres[0] + (centres[kToolCount - 1] - centres[0]) * i / (kToolCount - 1));
  }
  for (int i = 0; i < kToolCount; ++i) {
    // Tap target: halfway to each neighbour, the row's ends at the edges.
    const int16_t from = i == 0 ? row.x : static_cast<int16_t>((centres[i - 1] + centres[i]) / 2);
    const int16_t to = i == kToolCount - 1 ? row.right() : static_cast<int16_t>((centres[i] + centres[i + 1]) / 2);
    const fui::Rect slot{from, row.y, static_cast<int16_t>(to - from), row.height};
    const int16_t blockX = static_cast<int16_t>(centres[i] - blockW[i] / 2);
    if (!model_.panel && model_.toolbarFocus == kFocusFirstTool + i) {
      cursor(t, fui::Rect{static_cast<int16_t>(blockX - 8), static_cast<int16_t>(row.y + 2),
                          static_cast<int16_t>(blockW[i] + 16), static_cast<int16_t>(row.height - 4)});
    }
    const int16_t top = static_cast<int16_t>(row.y + (row.height - blockH) / 2);
    t.bitmap(fui::Rect{static_cast<int16_t>(centres[i] - kIcon / 2), top, kIcon, kIcon}, icon24(kIcons[i]),
             fui::BitmapMode::Center);
    const bool open = i == model_.activeTool;
    nameStyle.bold = open;
    const char* name = I18N.get(kNames[i]);
    // Centred on the tool and as wide as its slot allows, so the bold open name has room to grow.
    const int16_t half = static_cast<int16_t>(std::min(centres[i] - slot.x, slot.right() - centres[i]));
    const fui::Rect nameRect{static_cast<int16_t>(centres[i] - half),
                             static_cast<int16_t>(top + kIcon + tokens.spaceSm), static_cast<int16_t>(2 * half), nameH};
    t.text(nameRect, name, nameStyle);
    if (open) {
      const int16_t w = t.measureText(nameStyle.font, name, nameStyle).width;
      t.fill(fui::Rect{static_cast<int16_t>(centres[i] - w / 2), static_cast<int16_t>(nameRect.y + nameH + 2), w,
                       kToolUnderline},
             ink());
    }
    screen.frame().hit(slot, ACTION_TOOL, static_cast<int16_t>(i), fui::InputTouch);
  }
}

void ReaderToolbarUi::buildToolbar(UiScreen& screen) {
  const auto& tokens = screen.theme();

  // Sheet height from its content: scrub row, meta line, tool row.
  const int16_t metaH = kBookmarkHit;
  const int16_t contentH =
      static_cast<int16_t>(tokens.spaceSm + kScrubButton + metaH + tokens.spaceSm + kToolRowH + tokens.spaceSm);
  fui::SheetProps sheetProps;
  sheetProps.anchor = fui::SheetEdge::Bottom;
  sheetProps.dismissAction = ACTION_DISMISS;
  // No grabber: it suggests a drag the sheet does not have. Tap the page above to close.
  sheetProps.grabberHeight = 0;
  sheetProps.grabberMargin = tokens.spaceSm;
  sheetProps.grabberInset = 0;
  const int16_t grabberBand =
      static_cast<int16_t>(sheetProps.grabberMargin + sheetProps.grabberHeight + sheetProps.grabberInset);
  screen.sheet(sheetProps, static_cast<int16_t>(contentH + grabberBand));
  screen.insetContent(fui::Insets{0, tokens.spaceMd, 0, tokens.spaceMd});
  screen.spacer(tokens.spaceSm);

  // Scrub row: |< [progress track + knob: tap/drag to jump] >|
  {
    const fui::Rect band = screen.takeTop(kScrubButton, 0);
    const fui::Rect previousRect{band.x, band.y, kScrubButton, kScrubButton};
    const fui::Rect nextRect{static_cast<int16_t>(band.right() - kScrubButton), band.y, kScrubButton, kScrubButton};
    const auto iconIn = [](const fui::Rect r) {
      return fui::Rect{static_cast<int16_t>(r.x + (r.width - kIcon) / 2),
                       static_cast<int16_t>(r.y + (r.height - kIcon) / 2), kIcon, kIcon};
    };
    if (model_.toolbarFocus == kFocusPrevChapter) cursor(screen.target(), previousRect);
    if (model_.toolbarFocus == kFocusNextChapter) cursor(screen.target(), nextRect);
    screen.target().bitmap(iconIn(previousRect), icon24(DockPrevChapterIcon), fui::BitmapMode::Center);
    screen.target().bitmap(iconIn(nextRect), icon24(DockNextChapterIcon), fui::BitmapMode::Center);
    screen.frame().hit(previousRect, ACTION_PREV, 0, fui::InputTouch);
    screen.frame().hit(nextRect, ACTION_NEXT, 0, fui::InputTouch);

    // Progress: a 2px track with a solid round knob at the book position, the
    // scrub hit rect spanning the whole band height so a finger a little off the
    // line still scrubs. Tap-to-jump / drag arrive as dragPermille along this rect.
    const int16_t trackX = static_cast<int16_t>(band.x + kScrubButton + kScrubGap);
    const int16_t trackW = static_cast<int16_t>(band.width - 2 * (kScrubButton + kScrubGap));
    const int16_t trackY = static_cast<int16_t>(band.y + band.height / 2);
    const int permille = std::clamp(model_.progressPermille, 0, 1000);
    const int16_t knobX = static_cast<int16_t>(trackX + (static_cast<int32_t>(trackW - 1) * permille) / 1000);
    screen.target().fill(fui::Rect{trackX, static_cast<int16_t>(trackY - 1), trackW, 2}, ink());
    screen.target().fill(fui::Rect{trackX, static_cast<int16_t>(trackY - 2), static_cast<int16_t>(knobX - trackX), 4},
                         ink());
    screen.target().fill(fui::Rect{static_cast<int16_t>(knobX - kScrubKnob / 2),
                                   static_cast<int16_t>(trackY - kScrubKnob / 2), kScrubKnob, kScrubKnob},
                         ink(), static_cast<uint8_t>(kScrubKnob / 2));
    screen.frame().hit(fui::Rect{trackX, band.y, trackW, band.height}, ACTION_SCRUB, 0,
                       fui::InputTouch | fui::InputDrag);
  }

  // Meta line: chapter title (left), page / percent and the bookmark toggle (right).
  {
    const fui::Rect line = screen.takeTop(metaH, tokens.spaceSm);
    const int16_t inkLeft = static_cast<int16_t>(line.x + kEdgeInk);
    const int16_t inkRight = static_cast<int16_t>(line.right() - kEdgeInk);
    const int16_t markX = static_cast<int16_t>(inkRight - kBookmarkInkRight);
    const fui::Rect markRect{static_cast<int16_t>(markX - (kBookmarkHit - kIcon) / 2), line.y, kBookmarkHit,
                             kBookmarkHit};
    fui::TextStyle titleStyle = tokens.bodyText;
    fui::TextStyle infoStyle = tokens.smallText;
    infoStyle.align = fui::TextAlign::Right;
    const int16_t infoW =
        model_.pageInfo ? screen.target().measureText(infoStyle.font, model_.pageInfo, infoStyle).width : 0;
    const fui::Rect infoRect{static_cast<int16_t>(markX + 4 - infoW - tokens.spaceMd), line.y, infoW, line.height};
    const fui::Rect titleRect{inkLeft, line.y, static_cast<int16_t>(infoRect.x - inkLeft - tokens.spaceMd),
                              line.height};
    if (model_.toolbarFocus == kFocusBookmark) cursor(screen.target(), markRect);
    centredText(screen.target(), titleRect, model_.chapterTitle, titleStyle);
    centredText(screen.target(), infoRect, model_.pageInfo, infoStyle);
    screen.target().bitmap(fui::Rect{markX, static_cast<int16_t>(line.y + (kBookmarkHit - kIcon) / 2), kIcon, kIcon},
                           icon24(model_.bookmarked ? DockBookmarkFilledIcon : DockBookmarkIcon),
                           fui::BitmapMode::Center);
    screen.frame().hit(markRect, ACTION_BOOKMARK, 0, fui::InputTouch);
  }

  screen.spacer(tokens.spaceSm);
  buildToolRow(screen, fui::LayoutAnchor::Top, 0);  // content band already inset
}

// The Text / Page panel: control rows, a label left and its stepper, chooser, switch or page
// pictures right.
void ReaderToolbarUi::buildTextRows(UiScreen& screen, const fui::Rect area) {
  const auto& tokens = screen.theme();
  auto& t = screen.target();
  const int count = model_.textRowCount;
  // The dock's ink edges, so labels and controls line up with the tools below.
  const int16_t left = static_cast<int16_t>(area.x + tokens.spaceMd + kEdgeInk);
  const int16_t right = static_cast<int16_t>(area.right() - tokens.spaceMd - kEdgeInk);
  // Ink ends at the right edge: a cell centres a 24 px glyph, so it reaches past by its padding.
  const int16_t inkRight = static_cast<int16_t>(right + (kOptionW - kIcon) / 2);
  fui::TextStyle labelStyle = tokens.bodyText;
  fui::TextStyle valueStyle = tokens.bodyText;
  valueStyle.align = fui::TextAlign::Right;
  int16_t y = area.y;

  for (int i = 0; i < count; ++i) {
    const TextRow& r = model_.textRows[i];
    const int16_t value = static_cast<int16_t>(i * kOptionStride);
    const bool focused = i == model_.selectedIndex;

    if (model_.pageGrid && r.kind == TextRow::Kind::Options) {
      // Label left, page pictures right; the chosen picture gets a second frame around it.
      int16_t width = 0;
      for (int k = 0; k < r.optionCount; ++k) {
        const bool landscape = r.picture == TextRow::Picture::Orientation && k % 2 == 1;
        width = static_cast<int16_t>(width + (landscape ? thumbH_ : thumbW_) + (k > 0 ? kThumbGap : 0));
      }
      // Frames reach 4 px past a picture: keep the outermost frame's ink on the right edge.
      int16_t x = static_cast<int16_t>(right - 4 - width);
      if (focused) {
        cursor(t, fui::Rect{static_cast<int16_t>(left - kCursorPad), static_cast<int16_t>(y + 10 - kCursorPad),
                            static_cast<int16_t>(right - left + 2 * kCursorPad),
                            static_cast<int16_t>(groupH_ - 20 + 2 * kCursorPad)});
      }
      centredText(t, fui::Rect{left, y, static_cast<int16_t>(x - left - tokens.spaceMd), groupH_}, r.label, labelStyle);
      for (int k = 0; k < r.optionCount; ++k) {
        const bool landscape = r.picture == TextRow::Picture::Orientation && k % 2 == 1;
        const int16_t w = landscape ? thumbH_ : thumbW_;
        const int16_t h = landscape ? thumbW_ : thumbH_;
        const fui::Rect thumb{x, static_cast<int16_t>(y + (groupH_ - h) / 2), w, h};
        drawPage(t, r.picture, k, thumb);
        if (k == r.selected) t.stroke(thumb.inset(fui::Insets{-4, -4, -4, -4}), ink(), 2, 4);
        screen.frame().hit(thumb.inset(fui::Insets{-8, -8, -8, -8}), ACTION_OPTION, static_cast<int16_t>(value + k),
                           fui::InputTouch);
        x = static_cast<int16_t>(x + w + kThumbGap);
      }
      y = static_cast<int16_t>(y + groupH_);
      continue;
    }

    const fui::Rect rowRect{area.x, y, area.width, kTextRowH};
    if (focused) {
      cursor(t, fui::Rect{static_cast<int16_t>(left - kCursorPad), static_cast<int16_t>(y + 8 - kCursorPad),
                          static_cast<int16_t>(right - left + 2 * kCursorPad),
                          static_cast<int16_t>(kTextRowH - 16 + 2 * kCursorPad)});
    }
    const int16_t cellY = static_cast<int16_t>(rowRect.y + (kTextRowH - kOptionH) / 2);
    int16_t controlX = right;
    switch (r.kind) {
      case TextRow::Kind::Stepper: {
        // [a] value [A]: Tabler's small letter steps down, the capital steps up.
        const fui::Rect plus{static_cast<int16_t>(inkRight - kStepW), cellY, kStepW, kOptionH};
        const fui::Rect number{static_cast<int16_t>(plus.x - kOptionGap - kStepValueW), cellY, kStepValueW, kOptionH};
        const fui::Rect minus{static_cast<int16_t>(number.x - kOptionGap - kStepW), cellY, kStepW, kOptionH};
        const auto iconIn = [](const fui::Rect c) {
          return fui::Rect{static_cast<int16_t>(c.x + (c.width - kIcon) / 2),
                           static_cast<int16_t>(c.y + (c.height - kIcon) / 2), kIcon, kIcon};
        };
        fui::TextStyle num = tokens.bodyText;
        num.align = fui::TextAlign::Center;
        t.bitmap(iconIn(minus), icon24(DockSmallerIcon), fui::BitmapMode::Center);
        centredText(t, number, r.value, num);
        t.bitmap(iconIn(plus), icon24(DockLargerIcon), fui::BitmapMode::Center);
        screen.frame().hit(minus, ACTION_OPTION, value, fui::InputTouch);
        screen.frame().hit(plus, ACTION_OPTION, static_cast<int16_t>(value + 1), fui::InputTouch);
        controlX = minus.x;
        break;
      }
      case TextRow::Kind::Toggle: {
        // A switch: outlined capsule, filled with the knob right when on.
        const fui::Rect sw{static_cast<int16_t>(right - 44), static_cast<int16_t>(rowRect.y + (kTextRowH - 24) / 2), 44,
                           24};
        if (r.selected > 0) t.fill(sw, ink(), 12);
        t.stroke(sw, ink(), 2, 12);
        const int16_t knobX = static_cast<int16_t>(r.selected > 0 ? sw.right() - 20 : sw.x + 4);
        t.fill(fui::Rect{knobX, static_cast<int16_t>(sw.y + 4), 16, 16}, r.selected > 0 ? paper() : ink(), 8);
        screen.frame().hit(rowRect, ACTION_ROW, static_cast<int16_t>(i), fui::InputTouch);
        controlX = sw.x;
        break;
      }
      default: {
        // Chooser: value and chevron at the right edge; a row with no value (it opens a screen)
        // keeps the whole width for its label.
        const int16_t valueX = static_cast<int16_t>(r.value ? left + (right - left) / 3 : right - 24);
        const fui::Rect valueRect{valueX, rowRect.y, static_cast<int16_t>(right - valueX), kTextRowH};
        char shown[64];
        snprintf(shown, sizeof(shown), "%s%s\xE2\x80\xBA", r.value ? r.value : "", r.value ? "  " : "");
        centredText(t, valueRect, shown, valueStyle);
        screen.frame().hit(rowRect, ACTION_ROW, static_cast<int16_t>(i), fui::InputTouch);
        controlX = valueRect.x;
        break;
      }
    }
    centredText(t, fui::Rect{left, rowRect.y, static_cast<int16_t>(controlX - left - tokens.spaceSm), kTextRowH},
                r.label, labelStyle);
    y = static_cast<int16_t>(y + kTextRowH);
  }
}

// Height the Text / Page panel's content needs.
int ReaderToolbarUi::textContentHeight() const { return model_.textRowCount * (model_.pageGrid ? groupH_ : kTextRowH); }

void ReaderToolbarUi::buildPanel(UiScreen& screen) {
  const auto& tokens = screen.theme();
  const fui::Rect safe = screen.frame().safeRect();

  fui::SheetProps sheetProps;
  sheetProps.anchor = fui::SheetEdge::Bottom;
  sheetProps.dismissAction = ACTION_DISMISS;  // tap the page above the sheet = back to the toolbar
  // No grabber: it suggests a drag the sheet does not have. Tap the page above to close.
  sheetProps.grabberHeight = 0;
  sheetProps.grabberMargin = tokens.spaceSm;
  sheetProps.grabberInset = 0;

  // Size the sheet to an exact number of rows instead of a fixed screen share:
  // a percentage leaves up to a row's height of dead space above the switcher,
  // and a short list leaves empty row slots. Chrome = everything in the sheet
  // that is not the rows.
  const bool textPanel = model_.textRows != nullptr;
  // The Text controls and a short list (More) grow to show every row without paging.
  const bool tall = textPanel || model_.fitAllRows;
  const int16_t rowH = textPanel          ? kTextRowH
                       : model_.denseRows ? static_cast<int16_t>(UITheme::getInstance().getMetrics().listRowHeight)
                                          : tokens.rowHeight;
  const int16_t rowGap = textPanel ? 0
                         : (model_.denseRows && !model_.fitAllRows)
                             ? tokens.listRowGap
                             : std::max(tokens.listRowGap, tokens.listTouchRowGap);
  const int16_t rowStride = static_cast<int16_t>(rowH + rowGap);
  const int16_t grabberBand =
      static_cast<int16_t>(sheetProps.grabberMargin + sheetProps.grabberHeight + sheetProps.grabberInset);
  const int16_t chrome =
      static_cast<int16_t>(grabberBand + tokens.spaceSm + tokens.spaceSm + kToolRowH + tokens.spaceSm);
  const bool landscape = safe.width > safe.height;
  const int targetPercent =
      landscape ? kLandscapePanelHeightPercent : (tall ? kTextPanelHeightMaxPercent : kPanelHeightPercent);
  const int capPercent =
      landscape ? kLandscapePanelHeightPercent : (tall ? kTextPanelHeightMaxPercent : kPanelHeightMaxPercent);
  const int16_t target = static_cast<int16_t>((safe.height * targetPercent) / 100);
  const int16_t cap = static_cast<int16_t>((safe.height * capPercent) / 100);
  if (textPanel) {
    // Shown whole, never paged. Page rows shrink, pictures and all, where the full size would not
    // fit the screen (landscape).
    groupH_ = kGroupH;
    thumbW_ = kThumbW;
    thumbH_ = kThumbH;
    const int16_t maxContent = static_cast<int16_t>(cap - chrome);
    if (model_.pageGrid && model_.textRowCount > 0 && textContentHeight() > maxContent) {
      groupH_ = static_cast<int16_t>(std::max(48, maxContent / model_.textRowCount));
      thumbH_ = static_cast<int16_t>(std::max(24, groupH_ - 20));
      thumbW_ = static_cast<int16_t>(thumbH_ * kThumbW / kThumbH);
    }
    screen.sheet(sheetProps, static_cast<int16_t>(chrome + std::min<int>(textContentHeight(), maxContent)));
    screen.spacer(tokens.spaceSm);
    screen.spacer(tokens.spaceSm, fui::LayoutAnchor::Bottom);
    buildToolRow(screen, fui::LayoutAnchor::Bottom, tokens.spaceMd);
    const fui::Rect rule = screen.take(fui::LayoutAnchor::Bottom, tokens.spaceSm);
    screen.target().fill(fui::Rect{rule.x, static_cast<int16_t>(rule.bottom() - 1), rule.width, 1}, ink());
    buildTextRows(screen, screen.body());
    return;
  }
  const int itemCount = model_.itemCount;
  int sheetRows = (target - chrome + rowGap) / rowStride;
  if (static_cast<int16_t>(chrome + (sheetRows + 1) * rowStride - rowGap) <= cap) ++sheetRows;
  if (itemCount > 0 && sheetRows > itemCount) sheetRows = itemCount;
  if (sheetRows < 1) sheetRows = 1;
  screen.sheet(sheetProps, static_cast<int16_t>(chrome + sheetRows * rowStride - rowGap));
  // No blanket side inset: Screen::list() draws in the content band, and the
  // scroll track must reach the sheet's edge like a full-screen list's does.
  // The title insets itself; the rows inset via rowInset below.

  // No title: the underlined tool below names the open panel.
  screen.spacer(tokens.spaceSm);

  // Switcher row along the sheet's bottom edge, under a hairline; the rows take what is left.
  screen.spacer(tokens.spaceSm, fui::LayoutAnchor::Bottom);
  buildToolRow(screen, fui::LayoutAnchor::Bottom, tokens.spaceMd);  // the dock's inset, so tools stay put
  const fui::Rect rule = screen.take(fui::LayoutAnchor::Bottom, tokens.spaceSm);
  screen.target().fill(fui::Rect{rule.x, static_cast<int16_t>(rule.bottom() - 1), rule.width, 1}, ink());

  listProps_.count = static_cast<uint16_t>(std::max(0, model_.itemCount));
  listProps_.action = ACTION_ROW;
  listProps_.inputMask = fui::InputTouch;  // physical buttons stay with the reader
  listProps_.rowHeight = rowH;
  listProps_.rowGap = rowGap;
  // The label column starts flush with the panel title (no list-side padding
  // on top of the sheet's own inset). Body-size text: small reads condensed
  // and the taller row doubles as the tap target.
  listProps_.labelText = tokens.bodyText;
  listProps_.sidePadding = 0;
  // The list band spans the sheet's full width -- the scroll track hugs the
  // panel edge (theme bezel inset included) exactly like a full-screen list.
  // rowInset pulls the rows back to the title's spaceLg alignment.
  listProps_.rowInset = tokens.spaceLg;
  const fui::Rect listRect = screen.body();
  const int count = std::max(0, model_.itemCount);
  // The nav owns selection + viewport (same fui::ListNav idiom as the list
  // menu screens). A shown cursor re-follows into view on every build; a
  // hidden one (-1, touch) leaves the viewport where scrolling put it.
  nav_.selected = std::clamp(model_.selectedIndex, -1, count - 1);
  nav_.followOnBuild = nav_.selected >= 0;
  nav_.followPending = false;
  nav_.syncToProps(listRect, listProps_.rowHeight, rowGap, count, listProps_);

  // Materialise only the visible window of rows.
  const int windowCount = std::min({nav_.visibleRows, count - nav_.top, kMaxWindow});
  for (int i = 0; i < windowCount; ++i) {
    const int index = nav_.top + i;
    const int toggleState = model_.rowToggleState ? model_.rowToggleState(index) : -1;
    windowLabels_[i] = model_.rowText ? model_.rowText(index) : std::string();
    windowValues_[i] = toggleState < 0 && model_.rowValue ? model_.rowValue(index) : std::string();
    fui::ListItem item;
    item.label = windowLabels_[i].c_str();
    item.value = windowValues_[i].empty() ? nullptr : windowValues_[i].c_str();
    item.toggle = toggleState >= 0;
    item.toggleChecked = toggleState > 0;
    item.actionValue = static_cast<int16_t>(index);
    windowItems_[i] = item;
  }
  listProps_.items = windowItems_;
  listProps_.itemsWindowFirst = static_cast<uint16_t>(nav_.top);
  listProps_.itemsWindowCount = static_cast<uint16_t>(std::max(0, windowCount));
  listProps_.valueText = tokens.bodyText;
  applySwitchStyle(listProps_);
  if (count > 0) {
    screen.list(listProps_);
  }
}
