#pragma once

#include <cstdint>

#include "components/UiAppHost.h"

class GfxRenderer;
class MappedInputManager;

// The one tab band every screen with top tabs draws: FreeInkUI's tabBar with the active theme's
// treatment (Cover Grid pills, Lyra's band wash and underline, RoundedRaff's full-slot pills).
// It takes the band off the top of the screen's remaining content and registers the tabs as
// touch targets under `action`, so drawing and hit-testing cannot drift apart.
namespace UiTabBand {

struct Options {
  // Dispatched with the tab's value when a tab is tapped.
  freeink::ui::ActionId action = 0;
  // The button cursor is on the band.
  bool focused = false;
  bool hasTouch = false;
  // When > 0, each pill is capped at its label width plus this padding per side, centred in its
  // equal-width slot (themes other than Cover Grid).
  int16_t pillMaxPad = 0;
};

void build(UiAppHost::UiScreen& screen, const GfxRenderer& renderer, const freeink::ui::TabItem* tabs, int count,
           const Options& options);

// The band for a screen that draws itself by hand (the cover grid, Insights): a FreeInkUI app
// whose whole screen is the band, so those screens get the same pills and the same touch targets
// as the FreeInkUI list screens without becoming one.
class Host : private UiAppHost {
 public:
  explicit Host(const GfxRenderer& renderer);
  // Call from onEnter().
  void begin();
  // Paint the band with its top edge at `top`. Call from render(), after the header.
  void render(const freeink::ui::TabItem* tabs, int count, const Options& options, int top);
  // The tab tapped this pass, or -1. Call from loop() before the screen's own touch handling.
  int tappedTab(const MappedInputManager& input);

 private:
  static constexpr freeink::ui::ActionId ACTION_TAB = 1;
  static void screenFn(UiScreen& screen, void* user);
  static void tabFn(const freeink::ui::ActionEvent& event, void* user);

  const GfxRenderer& renderer_;
  // Valid only inside render(): the app builds its screen synchronously from there.
  const freeink::ui::TabItem* tabs_ = nullptr;
  int count_ = 0;
  Options options_{};
  int top_ = 0;
  int tapped_ = -1;
};

}  // namespace UiTabBand
