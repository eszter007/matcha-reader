#include "UiTabListActivity.h"

#include <GfxRenderer.h>

#include <algorithm>
#include <cassert>

#include "MappedInputManager.h"
#include "components/HomeTabBar.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

UiTabListActivity::UiTabListActivity(const char* name, GfxRenderer& renderer, MappedInputManager& mappedInput,
                                     const bool wantsTouchLongPress)
    : UiListActivity(name, renderer, mappedInput, wantsTouchLongPress) {}

void UiTabListActivity::onEnter() {
  // Size the per-tab state before the base resets activeNav() (which indexes
  // into it).
  tabNavs.assign(static_cast<size_t>(tabCount()), fui::ListNav{});
  UiListActivity::onEnter();
  app.on(ACTION_TAB, &UiTabListActivity::tabActionTrampoline, this);
}

fui::ListNav& UiTabListActivity::activeNav() {
  if (tabNavs.empty()) return nav;  // pre-onEnter fallback
  // Invariant: subclasses keep activeTab() inside [0, tabCount()), and
  // tabCount() does not change after onEnter() sized tabNavs.
  assert(activeTab() >= 0 && static_cast<size_t>(activeTab()) < tabNavs.size());
  return tabNavs[static_cast<size_t>(activeTab())];
}

int UiTabListActivity::ringPos() const {
  if (tabNavs.empty()) return 0;
  assert(activeTab() >= 0 && static_cast<size_t>(activeTab()) < tabNavs.size());
  return tabNavs[static_cast<size_t>(activeTab())].selected;
}

void UiTabListActivity::tabActionTrampoline(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<UiTabListActivity*>(user);
  if (event.value < 0 || event.value >= self->tabCount()) return;
  self->onTabAction(event.value);
}

void UiTabListActivity::onRowAction(const fui::ActionEvent& event) {
  activeNav().selected = event.value + 1;  // ring position, not row index
  if (event.longPress) {
    onRowLongPress(event.value);
    return;
  }
  activateIndex(event.value);
}

void UiTabListActivity::moveRingTo(const int ringIndex) {
  activeNav().requestSelection(ringIndex);
  requestUpdate();
}

TabRing::Focus UiTabListActivity::ringFocus() const {
  if (tabFocus >= 0) return TabRing::Focus::BottomBar;
  return ringPos() == 0 ? TabRing::Focus::TopTabs : TabRing::Focus::Content;
}

void UiTabListActivity::ringSetFocus(const TabRing::Focus focus, const bool atEnd) {
  tabFocus = focus == TabRing::Focus::BottomBar ? static_cast<int>(tabBarTab()) : -1;
  if (focus == TabRing::Focus::TopTabs) moveRingTo(0);
  if (focus == TabRing::Focus::Content) moveRingTo(atEnd ? listCount() : 1);
}

void UiTabListActivity::restoreHoldStart() {
  if (!holdStart_.pending) return;
  holdStart_.pending = false;
  activeNav().selected = holdStart_.selected;
  tabFocus = holdStart_.tabFocus;
}

void UiTabListActivity::navigateButtons() {
  if (mappedInput.wasPressed(MappedInputManager::Button::NavNext) ||
      mappedInput.wasPressed(MappedInputManager::Button::NavPrevious)) {
    navigationStartedOnTabs = ringPos() == 0 && tabFocus < 0;
    holdStart_ = {activeNav().selected, tabFocus, true};
  }
  // A press walks the ring at once (TabRing decides every move between the parts); a hold steps
  // the tab instead, see holdStart_.
  buttonNavigator.onNextPress([this] {
    if (ringFocus() != TabRing::Focus::Content) return TabRing::step(*this, 1);
    if (ringPos() >= listCount()) return TabRing::leaveContent(*this, 1);
    moveRingTo(ringPos() + 1);
  });
  buttonNavigator.onPreviousPress([this] {
    if (ringFocus() != TabRing::Focus::Content) return TabRing::step(*this, -1);
    if (ringPos() <= 1) return TabRing::leaveContent(*this, -1);
    moveRingTo(ringPos() - 1);
  });
  buttonNavigator.onNextContinuous([this] {
    restoreHoldStart();
    stepTab(1);
  });
  buttonNavigator.onPreviousContinuous([this] {
    restoreHoldStart();
    stepTab(-1);
  });
}

void UiTabListActivity::syncTabListViewport(UiScreen& screen, fui::ListProps& props) {
  syncListViewport(screen, props, 1);
}

// Cover Grid's tab band: content-width pills packed from the leading edge. Idle pills are a grey
// outline, the selected one is filled solid black with white text -- no underline and no rule
// under the band, so the pill alone carries the state. With the cursor elsewhere on the screen
// the fill drops to a grey dither, which is the only cue left that the band is not where the
// next key press lands.
void UiTabListActivity::buildPillTabBar(UiScreen& screen, const fui::TabItem* tabs, const int count,
                                        const bool tabsFocused) {
  const auto& metrics = UITheme::getInstance().getMetrics();

  fui::TabBarProps props;
  props.tabs = tabs;
  props.count = static_cast<uint8_t>(count);
  props.action = ACTION_TAB;
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
  const auto preferred = static_cast<int16_t>(tabBandHeight(metrics, mappedInput.hasTouch()));
  const int16_t band = preferred > wanted ? preferred : wanted;
  const int pillHeight = band - 2 * PILL_INSET_V;
  // A stadium needs a radius of at least half the pill height.
  const auto radius = static_cast<uint8_t>(pillHeight > 2 ? std::min(pillHeight / 2, 255) : 1);

  const auto side = static_cast<int16_t>(metrics.contentSidePadding);
  const int16_t slotsWidth = static_cast<int16_t>(screen.frame().screen().width - 2 * side);
  // Widest horizontal padding the row still fits at. Past that tabBar() abandons ContentWidth for
  // equal slots, which would leave the outlines this function paints at the wrong x.
  int16_t pad = PILL_PAD_H;
  for (;; pad = static_cast<int16_t>(pad - 2)) {
    int total = PILL_LEADING + PILL_GAP * (count - 1);
    for (int i = 0; i < count; ++i) {
      const int16_t labelW = screen.target().measureText(props.text.font, tabs[i].label, props.text).width;
      total += std::max<int>(labelW + 2 * pad, pillHeight);
    }
    if (total <= slotsWidth || pad <= PILL_PAD_H_MIN) break;
  }
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
    const int16_t labelW = screen.target().measureText(props.text.font, tabs[i].label, props.text).width;
    const auto pillW = static_cast<int16_t>(std::max<int>(labelW + 2 * pad, pillHeight));
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

void UiTabListActivity::buildTabBar(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();

  // Tabs. The selected pill dims to a dither when the selection is down in
  // the list (the legacy focused/unfocused tab distinction).
  // Stack array, not a heap vector: this runs on every render and the tab
  // count is small and fixed.
  constexpr int MAX_TABS = 8;
  const int count = tabCount() < MAX_TABS ? tabCount() : MAX_TABS;
  fui::TabItem tabs[MAX_TABS];
  for (int i = 0; i < count; i++) {
    tabs[i].label = tabLabel(i);
    tabs[i].value = static_cast<int16_t>(i);
    tabs[i].selected = activeTab() == i;
    tabs[i].indicator = tabIndicator(i);
  }
  const bool tabsFocused = ringFocus() == TabRing::Focus::TopTabs;
  if (HomeTabBar::enabled()) {
    buildPillTabBar(screen, tabs, count, tabsFocused);
    return;
  }
  fui::TabBarProps tabProps;
  tabProps.tabs = tabs;
  tabProps.count = static_cast<uint16_t>(count);
  tabProps.action = ACTION_TAB;
  tabProps.inputMask = fui::InputTouch;
  // Pill shape and label size are theme-driven. Lyra uses equal-width slots
  // with small labels so wide text (e.g. "Controls") still fits at large UI scales.
  // Full-slot (RoundedRaff): the pill fills its slot like the legacy layout
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
    tabProps.tabInset = tabsFocused ? fui::Insets{2, 4, 4, 4} : fui::Insets{2, 4, 0, 4};
    tabProps.contentInset = fui::Insets{2, 0, 2, 0};
  }
  const int16_t tabLineHeight = screen.target().lineHeight(tabProps.text.font);
  const auto preferredTabHeight = static_cast<int16_t>(tabBandHeight(metrics, mappedInput.hasTouch()));
  const int16_t tabBand = preferredTabHeight > tabLineHeight + 10 ? preferredTabHeight : tabLineHeight + 10;

  if (tabPillMaxPad > 0) {
    // Cap each pill at its label plus this padding: the equal-width slots (and
    // so the tab positions) stay exactly where they were, only the pill stops
    // stretching across the whole slot. The SDK shrinks the pill to content
    // width and centers it in its slot when the horizontal contentInset is
    // nonzero.
    tabProps.contentInset.left = tabPillMaxPad;
    tabProps.contentInset.right = tabPillMaxPad;
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
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
}
