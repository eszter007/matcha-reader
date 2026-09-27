#pragma once

#include "CrossPointSettings.h"
#include "activities/Activity.h"
#include "components/UiAppHost.h"
#include "util/ButtonNavigator.h"

// Top-anchored control center opened by a top-edge down-swipe, a status-bar tap,
// or a button bound to "Control Center" (iOS Control Center style): a grabber,
// the frontlight brightness/warmth sliders (on boards with a light), and a grid
// of quick-setting tiles — night mode, ghost-cleanup refresh, reading
// orientation, and reader touch controls on/off. The
// frontlight controls are always there: they are what the panel is for. Pure
// 1-bit: no dithered fills, selection reads as a filled tile. The grabber sits
// along the panel's bottom edge, the edge the sheet is dragged from.
class FrontlightPanelActivity final : public Activity, private UiAppHost {
  ButtonNavigator buttonNavigator;

  uint8_t brightness = 60;
  uint8_t warmth = 50;
  bool lightOn = false;
  // lightOn is seeded from the live hardware state (Frontlight.isOn()), which
  // legitimately diverges from the saved SETTINGS.frontlightOn preference —
  // e.g. after a wake with frontlightRestoreOnWake off, the light stays off
  // live while the saved "was on" preference is deliberately kept (see
  // main.cpp's restoreLightOn). brightness/warmth have no such divergence
  // (always restored unconditionally on boot), so only lightOn needs a
  // touched-by-the-user flag: onExit() must not persist a mirror that never
  // reflected user intent in the first place.
  bool lightOnChanged = false;
  bool draggingSlider = false;
  // The touch tile toggles SETTINGS.touchReaderControls between off and this
  // remembered mode, so a Swipe or Inverted Tap user gets their mode back
  // rather than the Tap default. Seeded from the setting in onEnter().
  uint8_t touchModeRestore = CrossPointSettings::TOUCH_READER_ON;
  // When the orientation tile last fired. Each activation costs the reader a full chapter
  // repagination, so a repeated event must not be taken as a repeated intent -- see runTile().
  uint32_t lastOrientationTileMs = 0;
  // A deliberate second rotation is seconds apart; a repeat from one gesture is tens of
  // milliseconds. Well clear of both.
  static constexpr uint32_t kOrientationTileDebounceMs = 750;
  int panelBottom = 0;

  // Quick-setting buttons, left to right: night mode, refresh, orientation, touch, light. Round
  // icon buttons with a caption under each; every icon shows what tapping it does, so the night,
  // touch and light buttons swap glyph with their state. Touch boards only — they are touch
  // targets. The light button lives here rather than beside the brightness slider so all five
  // switches read as one row.
  static constexpr int kQuickCount = 5;

  // fui::SliderRowProps and fui::TileGridProps embed a 324-byte fui::StyleSet,
  // so the props the render path fills in live here instead of on the stack
  // (AGENTS.md: locals stay under 256 bytes). The components take them by
  // const reference and draw immediately, so one instance per call site is
  // enough — every field either is reassigned on each use or keeps its
  // constructed default.
  freeink::ui::SliderRowProps rowProps;
  // Band the quick-button row was laid out into, in screen coordinates: the row draws as chrome
  // after the app renders, and loop() hit-tests taps against it.
  freeink::ui::Rect quickRowRect{};

  static void panelScreen(UiScreen& screen, void* user);
  static void onBrightnessEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onWarmthEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onToggleEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onBrightnessStepEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onWarmthStepEvent(const freeink::ui::ActionEvent& event, void* user);

  void buildPanelScreen(UiScreen& screen);
  // One slider row: a leading icon naming the control, then [-] [draggable capsule] [+], plus a
  // lamp on/off button after the + when showToggle is set (button boards, which have no quick
  // buttons to put the light switch in).
  void addSliderRow(UiScreen& screen, const uint8_t* icon, uint8_t value, freeink::ui::ActionId sliderAction,
                    freeink::ui::ActionId stepAction, bool showToggle);
  int computePanelBottom() const;
  void adjustBrightness(int delta);
  void adjustWarmth(int delta);
  void toggleLight();
  // What tapping quick button `idx` does. Kept as its own step so the night/refresh/orientation/
  // touch actions read in one place.
  void runTile(int idx);
  // Paint the round buttons and their captions into quickRowRect.
  void drawQuickRow();
  // Button index under a touch, or -1.
  int quickButtonAt(int x, int y) const;
  void runQuickButton(int idx);
  // Copy the panel's live brightness/warmth/lightOn into SETTINGS and save if
  // anything actually changed. onExit() runs it on every way out.
  void persistLightSettings();
  void close();

  // One-shot: a tile that rewrote the whole frame (night mode) re-drives it
  // with the ghost-cleanup waveform on the next render. The "refresh" tile does
  // not use this — it closes the panel and promotes the repaint underneath
  // instead (GfxRenderer::promoteNextRefresh).
  bool cleanRefreshPending = false;

 public:
  explicit FrontlightPanelActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool handleHomeGesture() override;
};
