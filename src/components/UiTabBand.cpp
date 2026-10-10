#include "UiTabBand.h"

#include <GfxRenderer.h>

#include <algorithm>

#include "components/HomeTabBar.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {
constexpr int16_t PILL_PAD_H = 20;
constexpr int16_t PILL_PAD_H_MIN = 8;
constexpr int16_t PILL_INSET_V = 4;
constexpr int16_t PILL_GAP = 10;
constexpr int16_t PILL_LEADING = 4;
// Lyra's tab: its label plus this much on each side, the pill width its equal slots used to cap at.
constexpr int16_t LYRA_TAB_PAD = 16;
constexpr int16_t LYRA_TAB_MIN_PAD = 4;

// Cover Grid's tab band: content-width pills packed from the leading edge. Idle pills are a grey
// outline, the selected one is filled solid black with white text -- no underline and no rule
// under the band, so the pill alone carries the state. With the cursor elsewhere on the screen
// the fill drops to a grey dither, which is the only cue left that the band is not where the
// next key press lands.
void buildPills(UiAppHost::UiScreen& screen, const GfxRenderer& renderer, const fui::TabItem* tabs, const int count,
                const UiTabBand::Options& opt) {
  const bool tabsFocused = opt.focused;
  const auto& metrics = UITheme::getInstance().getMetrics();

  fui::TabBarProps props;
  props.tabs = tabs;
  props.count = static_cast<uint8_t>(count);
  props.action = opt.action;
  props.inputMask = fui::InputTouch;
  props.text = screen.theme().smallText;
  props.text.align = fui::TextAlign::Center;
  // ContentWidth, not the default EqualWidth: pills sized to their own label, so a short one
  // ("Tags") stays an oval instead of spreading into a circle across an equal slot.
  props.layout = fui::TabBarLayout::ContentWidth;
  props.tabInset = fui::Insets{PILL_INSET_V, 0, PILL_INSET_V, 0};
  props.gap = PILL_GAP;
  props.leadingInset = PILL_LEADING;
  props.divider = false;

  const int16_t lineHeight = screen.target().lineHeight(props.text.font);
  // Pill height is the band minus the tab insets, so the band carries the label plus its 8px
  // vertical padding plus those insets.
  const auto wanted = static_cast<int16_t>(lineHeight + 16 + 2 * PILL_INSET_V);
  const auto preferred = static_cast<int16_t>(tabBandHeight(metrics, opt.hasTouch));
  const int16_t band = preferred > wanted ? preferred : wanted;
  const int pillHeight = band - 2 * PILL_INSET_V;
  // A stadium needs a radius of at least half the pill height.
  const auto radius = static_cast<uint8_t>(pillHeight > 2 ? std::min(pillHeight / 2, 255) : 1);

  const auto side = static_cast<int16_t>(metrics.contentSidePadding);
  const int16_t slotsWidth = static_cast<int16_t>(screen.frame().screen().width - 2 * side);
  const auto pillWidth = [&](const int i, const int pad) {
    const int16_t labelW = screen.target().measureText(props.text.font, tabs[i].label, props.text).width;
    return std::max<int>(labelW + 2 * pad, pillHeight);
  };
  const auto rowWidth = [&](const int first, const int n, const int pad) {
    int total = PILL_LEADING + PILL_GAP * (n - 1);
    for (int i = first; i < first + n; ++i) total += pillWidth(i, pad);
    return total;
  };
  // More tabs than the row holds (Insights, one per language): show the run that ends on the
  // selected one, then as many after it as still fit. tabBar() itself does not scroll.
  if (count > 1 && rowWidth(0, count, PILL_PAD_H_MIN) > slotsWidth) {
    int selected = 0;
    for (int i = 0; i < count; ++i) {
      if (tabs[i].selected) selected = i;
    }
    // At the full padding, not the squeezed one: a windowed row has no reason to look cramped.
    int first = 0;
    while (first < selected && rowWidth(first, selected - first + 1, PILL_PAD_H) > slotsWidth) first++;
    int shown = selected - first + 1;
    while (first + shown < count && rowWidth(first, shown + 1, PILL_PAD_H) <= slotsWidth) shown++;
    buildPills(screen, renderer, tabs + first, shown, opt);
    return;
  }
  // Widest horizontal padding the row still fits at. Past that tabBar() abandons ContentWidth for
  // equal slots, which would leave the outlines this function paints at the wrong x.
  int16_t pad = PILL_PAD_H;
  while (pad > PILL_PAD_H_MIN && rowWidth(0, count, pad) > slotsWidth) pad = static_cast<int16_t>(pad - 2);
  props.contentInset = fui::Insets{8, pad, 8, pad};

  // explicitlySet is required: without it StyleSet::unset() is true and tabBar() substitutes
  // its own defaults for everything below.
  fui::StyleSet pills{};
  pills.explicitlySet = true;
  pills.normal.background = fui::Paint::none();
  pills.normal.foreground = fui::Paint::solid(fui::Color::Black);
  // No border here: FreeInkUIGfxRenderer::stroke honours a dithered paint only at radius 0 and
  // falls back to solid black on a rounded one, which is the heavy outline this replaces. The
  // grey outline is painted below instead.
  pills.normal.border = fui::Paint::none();
  pills.normal.radius = radius;
  pills.selected = pills.normal;
  pills.selected.background =
      tabsFocused ? fui::Paint::solid(fui::Color::Black) : fui::Paint::dither(fui::Color::DarkGray);
  pills.selected.foreground = fui::Paint::solid(fui::Color::White);
  pills.focused = pills.normal;
  pills.active = pills.selected;
  pills.disabled = pills.normal;
  props.tabStyles = pills;

  const fui::Rect contentTabRect = screen.takeTop(band);
  const fui::Rect frameRect = screen.frame().screen();
  const fui::Rect tabRect{frameRect.x, contentTabRect.y, frameRect.width, contentTabRect.height};
  const fui::Rect slotsRect{static_cast<int16_t>(tabRect.x + side), tabRect.y, slotsWidth, tabRect.height};

  // Grey outlines first, under the labels tabBar() draws: a dithered pill with a white one punched
  // out of it. Slot geometry mirrors tabBar()'s ContentWidth pass, which with zero horizontal
  // tabInset makes each slot exactly its pill.
  int16_t x = static_cast<int16_t>(slotsRect.x + PILL_LEADING);
  for (int i = 0; i < count; ++i) {
    const auto pillW = static_cast<int16_t>(pillWidth(i, pad));
    if (!tabs[i].selected) {
      const int16_t y = static_cast<int16_t>(slotsRect.y + PILL_INSET_V);
      renderer.fillRoundedRect(x, y, pillW, pillHeight, radius, Color::DarkGray);
      renderer.fillRoundedRect(x + 2, y + 2, pillW - 4, pillHeight - 4, std::max(radius - 2, 1), Color::White);
    }
    x = static_cast<int16_t>(x + pillW + PILL_GAP);
  }

  fui::tabBar(screen.frame(), slotsRect, props);
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
}

}  // namespace

bool UiTabBand::drawsTopRule() {
  // Only where headers carry a rule (Lyra): Classic has none, so neither does its band.
  const auto& metrics = UITheme::getInstance().getMetrics();
  return !HomeTabBar::enabled() && !metrics.tabPillFullSlot && metrics.headerUnderlineSize > 0;
}

void UiTabBand::build(UiAppHost::UiScreen& screen, const GfxRenderer& renderer, const fui::TabItem* tabs,
                      const int count, const Options& opt) {
  if (HomeTabBar::enabled()) {
    buildPills(screen, renderer, tabs, count, opt);
    return;
  }
  const auto& metrics = UITheme::getInstance().getMetrics();
  const bool tabsFocused = opt.focused;
  fui::TabBarProps tabProps;
  tabProps.tabs = tabs;
  tabProps.count = static_cast<uint16_t>(count);
  tabProps.action = opt.action;
  tabProps.inputMask = fui::InputTouch;
  // Pill shape and label size are theme-driven. Lyra packs tabs at their label width from the
  // leading edge, as its own tab row did; tabBar() falls back to equal slots when a row is too
  // wide for that. Full-slot (RoundedRaff): the pill fills its slot like the legacy layout
  // (slot minus a 4px frame, 8px clearance above the divider) with
  // body-size labels; zero horizontal contentInset disables the tabBar's
  // label-width shrink.
  if (metrics.tabPillFullSlot) {
    tabProps.text = screen.theme().bodyText;
    tabProps.tabInset = fui::Insets{4, 4, 7, 4};
    tabProps.contentInset = fui::Insets{2, 0, 2, 0};
  } else {
    tabProps.text = screen.theme().smallText;
    tabProps.gap = static_cast<int16_t>(metrics.tabSpacing);
    // Unfocused state: no bottom inset, so the pill (and the 2px selected
    // underline drawn along its bottom edge) reaches the band's 1px divider —
    // legacy Lyra drew the underline sitting on that rule, not floating above.
    tabProps.tabInset = tabsFocused ? fui::Insets{2, 0, 4, 0} : fui::Insets{2, 0, 0, 0};
    tabProps.layout = fui::TabBarLayout::ContentWidth;
  }
  // Equal slots narrower than the widest label would clip it: show the window of tabs that fits,
  // ending on the selected one.
  {
    int widest = 1;
    int selected = 0;
    for (int i = 0; i < count; ++i) {
      widest =
          std::max<int>(widest, screen.target().measureText(tabProps.text.font, tabs[i].label, tabProps.text).width);
      if (tabs[i].selected) selected = i;
    }
    const int available = screen.frame().screen().width - 2 * metrics.contentSidePadding;
    const int fits = std::max(1, available / (widest + 16));
    if (count > fits) {
      const int first = std::clamp(selected - fits + 1, 0, count - fits);
      tabProps.tabs = tabs + first;
      tabProps.count = static_cast<uint16_t>(fits);
    }
  }
  if (tabProps.layout == fui::TabBarLayout::ContentWidth) {
    // The widest padding the row still fits at, as Cover Grid's pills choose theirs. Past the
    // narrowest, tabBar() falls back to equal slots, which cut long labels short.
    const int available = screen.frame().screen().width - 2 * metrics.contentSidePadding;
    int16_t pad = LYRA_TAB_PAD;
    for (; pad > LYRA_TAB_MIN_PAD; pad = static_cast<int16_t>(pad - 4)) {
      int row = tabProps.gap * (tabProps.count - 1);
      for (int i = 0; i < tabProps.count; ++i) {
        row += screen.target().measureText(tabProps.text.font, tabProps.tabs[i].label, tabProps.text).width + 2 * pad;
      }
      if (row <= available) break;
    }
    tabProps.contentInset = fui::Insets{2, pad, 2, pad};
  }
  const int16_t tabLineHeight = screen.target().lineHeight(tabProps.text.font);
  const auto preferredTabHeight = static_cast<int16_t>(tabBandHeight(metrics, opt.hasTouch));
  const int16_t tabBand = preferredTabHeight > tabLineHeight + 10 ? preferredTabHeight : tabLineHeight + 10;

  if (opt.pillMaxPad > 0 && metrics.tabPillFullSlot) {
    // Cap each pill at its label plus this padding: the equal-width slots (and
    // so the tab positions) stay exactly where they were, only the pill stops
    // stretching across the whole slot. The SDK shrinks the pill to content
    // width and centers it in its slot when the horizontal contentInset is
    // nonzero.
    tabProps.contentInset.left = opt.pillMaxPad;
    tabProps.contentInset.right = opt.pillMaxPad;
  }

  // Legacy Lyra two-state treatment: with the selection on the tab band, the
  // band fills gray and the active tab is a solid pill; with the selection
  // down in the list, the band is plain and the active tab keeps a gray box
  // with an underline. The 1px rule under the band is always there, drawn
  // full-width below (not by tabBar, whose rect is inset for side padding).
  fui::StyleSet tabStyles;
  tabStyles.explicitlySet = true;
  tabStyles.normal.foreground = fui::Paint::solid(fui::Color::Black);
  if (tabsFocused) {
    tabStyles.selected.background = fui::Paint::solid(fui::Color::Black);
    tabStyles.selected.foreground = fui::Paint::solid(fui::Color::White);
    tabStyles.selected.radius = screen.theme().listRowRadius;
  } else if (metrics.tabPillFullSlot) {
    // Legacy RoundedRaff unfocused treatment: same pill, dimmed to dark gray,
    // text stays inverted; no underline.
    tabStyles.selected.background = fui::Paint::dither(fui::Color::DarkGray);
    tabStyles.selected.foreground = fui::Paint::solid(fui::Color::White);
    tabStyles.selected.radius = screen.theme().listRowRadius;
  } else {
    tabStyles.selected.background = fui::Paint::dither(fui::Color::LightGray);
    tabStyles.selected.foreground = fui::Paint::solid(fui::Color::Black);
    tabProps.selectedUnderline = 2;
  }
  // Focus/flash states keep the pill instead of falling back to an unset
  // (blank) style.
  tabStyles.focused = tabStyles.selected;
  tabStyles.active = tabStyles.selected;
  tabProps.tabStyles = tabStyles;
  const fui::Rect contentTabRect = screen.takeTop(tabBand);
  const fui::Rect frameRect = screen.frame().screen();
  // Tab chrome is a full-width screen band like the legacy GUI tab bar. The
  // remaining list content still stays inside the device safe area.
  const fui::Rect tabRect{frameRect.x, contentTabRect.y, frameRect.width, contentTabRect.height};
  // Focused band wash is the Lyra treatment; legacy RoundedRaff keeps the
  // band plain in both states.
  if (tabsFocused && !metrics.tabPillFullSlot) {
    screen.target().fill(tabRect, fui::Paint::dither(fui::Color::LightGray));
  }
  // The band chrome (wash, divider) spans the full screen width, but the tab
  // slots keep the content side padding so the outer pills never touch the
  // bezel. The divider is drawn here rather than by tabBar(), which would
  // inset it along with the slots; the slot band is shortened by the same 1px
  // so pill geometry is unchanged.
  const auto side = static_cast<int16_t>(metrics.contentSidePadding);
  const fui::Rect slotsRect{static_cast<int16_t>(tabRect.x + side), tabRect.y,
                            static_cast<int16_t>(tabRect.width - 2 * side), static_cast<int16_t>(tabRect.height - 1)};
  tabProps.divider = false;
  fui::tabBar(screen.frame(), slotsRect, tabProps);
  screen.target().fill(fui::Rect{tabRect.x, static_cast<int16_t>(tabRect.bottom() - 1), tabRect.width, 1},
                       fui::Paint::solid(fui::Color::Black));
  if (drawsTopRule()) {
    // The theme's header rule, as thick as under any other header (Plugins, say).
    const auto rule = static_cast<int16_t>(std::max(1, metrics.headerUnderlineSize));
    screen.target().fill(fui::Rect{tabRect.x, tabRect.y, tabRect.width, rule}, fui::Paint::solid(fui::Color::Black));
  }
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
}

UiTabBand::Host::Host(const GfxRenderer& renderer) : UiAppHost(renderer), renderer_(renderer) {}

void UiTabBand::Host::begin() {
  resetUi();
  app.on(ACTION_TAB, &Host::tabFn, this);
  app.setScreen(&Host::screenFn, this);
}

void UiTabBand::Host::screenFn(UiScreen& screen, void* user) {
  auto* self = static_cast<Host*>(user);
  if (!self->tabs_ || self->count_ <= 0) return;
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(self->top_), 0, 0, 0});
  build(screen, self->renderer_, self->tabs_, self->count_, self->options_);
}

void UiTabBand::Host::tabFn(const fui::ActionEvent& event, void* user) {
  static_cast<Host*>(user)->tapped_ = event.value;
}

void UiTabBand::Host::render(const fui::TabItem* tabs, const int count, const Options& options, const int top) {
  tabs_ = tabs;
  count_ = count;
  options_ = options;
  options_.action = ACTION_TAB;
  top_ = top;
  renderUi();
  tabs_ = nullptr;
}

int UiTabBand::Host::tappedTab(const MappedInputManager& input) {
  tapped_ = -1;
  if (routeTouch(input)) app.clearTapFlash();
  const int tapped = tapped_;
  tapped_ = -1;
  return tapped;
}
