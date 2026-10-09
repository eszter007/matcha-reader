#include "FrontlightPanelActivity.h"

#include <FreeInkUIIcon.h>
#include <GfxRenderer.h>
#include <HalFrontlight.h>
#include <HalGPIO.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <iterator>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/icons/customListIcons.h"
#include "components/icons/listIcons.h"
#include "components/icons/panelIcons.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_BRIGHTNESS = 1;
constexpr fui::ActionId ACTION_WARMTH = 2;
constexpr fui::ActionId ACTION_TOGGLE = 3;
constexpr fui::ActionId ACTION_BRIGHTNESS_STEP = 4;
constexpr fui::ActionId ACTION_WARMTH_STEP = 5;

// iOS-style geometry. The panel is a card hanging from the top of the screen:
// a grabber, full-width slider pills, then a 2-column tile grid. The chrome
// itself is fui::sheet / fui::sliderRow / fui::tileGrid; these constants only
// size the bands, and computePanelBottom() mirrors them.
constexpr int16_t kPanelSideMargin = 16;
constexpr int16_t kGrabberHeight = 5;     // fui::SheetProps default, mirrored here
constexpr int16_t kSliderRowHeight = 56;  // the pill itself (finger-sized)
constexpr int16_t kRowIconSize = 32;      // leading icon naming each slider
// Round quick-setting button: finger-sized circle with a caption under it.
constexpr int16_t kQuickCircle = 56;
constexpr int16_t kQuickIcon = 32;
constexpr int16_t kQuickLabelGap = 4;
constexpr int kQuickLabelLines = 2;
// One percent per press, on the -/+ buttons and on the physical Left/Right keys
// alike (both repeat while held), so a level can be set exactly.
constexpr int BRIGHTNESS_STEP = 1;
// The dimmest setting is 1%, not 0: turning the light off is what the lamp
// button next to the slider is for, so a 0% "on" level would only be a second,
// worse way to reach the same place.
constexpr uint8_t MIN_BRIGHTNESS = 1;

uint8_t percentFromPermille(const int16_t permille) {
  int value = (static_cast<int>(permille) * 100 + 500) / 1000;
  if (value < 0) value = 0;
  if (value > 100) value = 100;
  return static_cast<uint8_t>(value);
}
}  // namespace

FrontlightPanelActivity::FrontlightPanelActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("FrontlightPanel", renderer, mappedInput), UiAppHost(renderer) {}

void FrontlightPanelActivity::onEnter() {
  Activity::onEnter();

  // A stored 0% predates the 1% floor (or came from the web settings): show it
  // as the floor rather than a level the slider can no longer produce. onExit
  // persists that, which is the intent — 0 is not a brightness any more.
  brightness = std::max(MIN_BRIGHTNESS, Frontlight.brightness());
  warmth = Frontlight.warmth();
  lightOn = Frontlight.isOn();
  lightOnChanged = false;

  // Seed the touch tile's restore mode from the live setting, so toggling off
  // and back on within this session returns to the mode the user had.
  if (SETTINGS.touchReaderControls != CrossPointSettings::TOUCH_READER_OFF) {
    touchModeRestore = SETTINGS.touchReaderControls;
  }

  resetUi();
  app.on(ACTION_BRIGHTNESS, &FrontlightPanelActivity::onBrightnessEvent, this);
  app.on(ACTION_WARMTH, &FrontlightPanelActivity::onWarmthEvent, this);
  app.on(ACTION_TOGGLE, &FrontlightPanelActivity::onToggleEvent, this);
  app.on(ACTION_BRIGHTNESS_STEP, &FrontlightPanelActivity::onBrightnessStepEvent, this);
  app.on(ACTION_WARMTH_STEP, &FrontlightPanelActivity::onWarmthStepEvent, this);
  app.setScreen(&FrontlightPanelActivity::panelScreen, this);
  requestUpdate();
}

void FrontlightPanelActivity::persistLightSettings() {
  // brightness/warmth are always restored unconditionally on boot (see
  // main.cpp), so they never diverge from SETTINGS at onEnter() — comparing
  // against SETTINGS here only fires on a genuine user change. lightOn has
  // no such guarantee (see lightOnChanged's declaration), so it's gated on
  // the user actually having touched it this session instead.
  const bool changed = SETTINGS.frontlightBrightness != brightness || SETTINGS.frontlightWarmth != warmth ||
                       (lightOnChanged && SETTINGS.frontlightOn != (lightOn ? 1 : 0));
  if (changed) {
    SETTINGS.frontlightBrightness = brightness;
    SETTINGS.frontlightWarmth = warmth;
    if (lightOnChanged) SETTINGS.frontlightOn = lightOn ? 1 : 0;
    SETTINGS.saveToFile();
  }
}

void FrontlightPanelActivity::onExit() {
  persistLightSettings();
  Activity::onExit();
}

void FrontlightPanelActivity::onBrightnessEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<FrontlightPanelActivity*>(user);
  if (event.dragPermille < 0) return;
  self->brightness = std::max(MIN_BRIGHTNESS, percentFromPermille(event.dragPermille));
  Frontlight.setBrightness(self->brightness);
  if (!self->lightOn) {
    self->lightOn = true;
    self->lightOnChanged = true;
    Frontlight.setOn(true);
  }
}

void FrontlightPanelActivity::onWarmthEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<FrontlightPanelActivity*>(user);
  if (event.dragPermille < 0) return;
  self->warmth = percentFromPermille(event.dragPermille);
  Frontlight.setWarmth(self->warmth);
}

void FrontlightPanelActivity::onToggleEvent(const fui::ActionEvent&, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->toggleLight();
}

void FrontlightPanelActivity::onBrightnessStepEvent(const fui::ActionEvent& event, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->adjustBrightness(event.value);
}

void FrontlightPanelActivity::onWarmthStepEvent(const fui::ActionEvent& event, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->adjustWarmth(event.value);
}

void FrontlightPanelActivity::runTile(const int idx) {
  switch (idx) {
    case 0:  // Night mode (inverted output polarity, applied to the whole UI)
      SETTINGS.screenInverted = SETTINGS.screenInverted ? 0 : 1;
      SETTINGS.saveToFile();
      // Inversion rewrites every pixel; take the clean waveform so the panel
      // does not keep a ghost of the old polarity.
      cleanRefreshPending = true;
      requestUpdate();
      break;
    case 1:  // Ghost-cleanup refresh of the whole frame
      // Refreshing with the panel still up would clean a frame the user is
      // about to dismiss anyway: drop the panel first and let the repaint of
      // the screen underneath carry the clean waveform instead.
      renderer.promoteNextRefresh(HalDisplay::FULL_REFRESH);
      close();
      break;
    case 2: {  // Cycle the reading orientation
      // Ignore a repeat of the same gesture. Every step here changes the reader's viewport, and
      // the reader reflows on any change -- a full chapter repagination, seconds to tens of
      // seconds on a long chapter. So a tile that fires three times for one hold does not just
      // overshoot the orientation, it rebuilds the chapter three times, which is what #209
      // reports as "flip, indexing, flip, indexing, flip, indexing" for a single shortcut.
      const uint32_t now = millis();
      if (lastOrientationTileMs != 0 && now - lastOrientationTileMs < kOrientationTileDebounceMs) {
        LOG_INF("FLP", "Ignoring repeated orientation tile within %ums", now - lastOrientationTileMs);
        break;
      }
      lastOrientationTileMs = now;
      SETTINGS.orientation = static_cast<uint8_t>((SETTINGS.orientation + 1) % 4);
      SETTINGS.saveToFile();
      // Only the setting changes: turning the renderer cropped the portrait-only
      // screens the panel opens over. The reader reflows on its next loop().
      requestUpdate();
      break;
    }
    case 3:  // Touch reader controls (for reading with the palm on the glass)
      // Toggles the existing Settings -> Controls option, nothing lower-level:
      // that setting only governs the reader's tap/swipe handling, so the
      // panel's own gestures (including the swipe that reopens it) keep
      // working while it is off. Off remembers the mode (Tap/Swipe/Inverted
      // Tap) so toggling back does not stomp the user's choice.
      if (SETTINGS.touchReaderControls != CrossPointSettings::TOUCH_READER_OFF) {
        touchModeRestore = SETTINGS.touchReaderControls;
        SETTINGS.touchReaderControls = CrossPointSettings::TOUCH_READER_OFF;
      } else {
        SETTINGS.touchReaderControls = touchModeRestore;
      }
      SETTINGS.saveToFile();
      requestUpdate();
      break;
    default:
      break;
  }
}

void FrontlightPanelActivity::adjustBrightness(const int delta) {
  int next = static_cast<int>(brightness) + delta;
  if (next < MIN_BRIGHTNESS) next = MIN_BRIGHTNESS;
  if (next > 100) next = 100;
  if (next == brightness) return;
  brightness = static_cast<uint8_t>(next);
  Frontlight.setBrightness(brightness);
  if (!lightOn) {
    lightOn = true;
    lightOnChanged = true;
    Frontlight.setOn(true);
  }
  requestUpdate();
}

void FrontlightPanelActivity::adjustWarmth(const int delta) {
  int next = static_cast<int>(warmth) + delta;
  if (next < 0) next = 0;
  if (next > 100) next = 100;
  if (next == warmth) return;
  warmth = static_cast<uint8_t>(next);
  Frontlight.setWarmth(warmth);
  requestUpdate();
}

void FrontlightPanelActivity::toggleLight() {
  lightOn = !lightOn;
  lightOnChanged = true;
  Frontlight.setOn(lightOn);
  requestUpdate();
}

void FrontlightPanelActivity::close() { finish(); }

bool FrontlightPanelActivity::handleHomeGesture() {
  close();
  return true;
}

void FrontlightPanelActivity::loop() {
  const auto touch = routeTouch(mappedInput, false, /*routeHeld=*/true);
  if (touch.routed) {
    if (app.invalidated()) requestUpdate();
    if (touch) {
      if (touch.event.dragPermille >= 0) draggingSlider = true;
      return;
    }
    // Swipe up dismisses the sheet, the way it was pulled down from the top
    // edge. draggingSlider keeps a fast slider flick from closing it.
    if (!draggingSlider && mappedInput.wasSwipe() == MappedInputManager::SwipeDir::Up) {
      close();
      return;
    }
    // panelBottom > 0 guards the frame the sheet opens in: the release that
    // opened it (a status-bar tap) is still in the input snapshot when the panel
    // runs its first loop(), and panelBottom is only known once render() has
    // measured the layout — so at 0 that release read as "tapped below the
    // sheet" and closed it again before it was ever drawn.
    if (touch.snap.touchReleased && !draggingSlider) {
      const int button = quickButtonAt(touch.snap.touchX, touch.snap.touchY);
      if (button >= 0) {
        runQuickButton(button);
        return;
      }
    }
    if (touch.snap.touchReleased && !draggingSlider && panelBottom > 0 && touch.snap.touchY >= panelBottom) {
      close();
      return;
    }
  }
  if (draggingSlider) {
    if (!touch.snap.touchHeld) {
      draggingSlider = false;
      cleanRefreshPending = true;
      requestUpdate();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasLightPanelGesture()) {
    close();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    toggleLight();
    return;
  }

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::ScreenLeft},
                                       [this] { adjustBrightness(-BRIGHTNESS_STEP); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::ScreenRight},
                                       [this] { adjustBrightness(BRIGHTNESS_STEP); });

  const int upDelta = gpio.hasEdgeSideButtons() ? -BRIGHTNESS_STEP : BRIGHTNESS_STEP;
  const int downDelta = gpio.hasEdgeSideButtons() ? BRIGHTNESS_STEP : -BRIGHTNESS_STEP;
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::ScreenUp, MappedInputManager::Button::PageBack},
                                       [this, upDelta] { adjustBrightness(upDelta); });
  buttonNavigator.onPressAndContinuous(
      {MappedInputManager::Button::ScreenDown, MappedInputManager::Button::PageForward},
      [this, downDelta] { adjustBrightness(downDelta); });
}

int FrontlightPanelActivity::computePanelBottom() const {
  const auto tokens = uiThemeTokens(uiTarget);
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int16_t lineHeight = uiTarget.lineHeight(tokens.smallText.font);
  // Slim battery band + the air around it (mirrors buildPanelScreen).
  const int y0 = std::max<int>(metrics.batteryHeight, lineHeight);
  int y = tokens.spaceMd + y0 + tokens.spaceMd;
  if (Frontlight.present()) {
    // addSliderRow() cancels the caption line, so a row is the control band plus the takeTop gap
    // and the spaceMd of air it adds after itself.
    y += kSliderRowHeight + 2 * tokens.spaceMd;  // brightness
    if (Frontlight.hasColorTemperature()) {
      y += kSliderRowHeight + 2 * tokens.spaceMd;  // warmth
    }
    y += tokens.spaceSm;
  }
  // The quick buttons are touch targets, so a buttons-only board gets none and the sheet is
  // exactly the frontlight controls. Mirrors the band buildPanelScreen() reserves.
  if (mappedInput.hasTouch()) {
    y += tokens.spaceSm + kQuickCircle + kQuickLabelGap + kQuickLabelLines * uiTarget.lineHeight(tokens.smallText.font);
  }
  // The sheet's grabber band: content margin + grabber + air to the edge.
  // buildPanelScreen() feeds the same theme spacings into SheetProps.
  y += tokens.spaceLg + kGrabberHeight + tokens.spaceLg + tokens.spaceMd;
  return y;
}

void FrontlightPanelActivity::panelScreen(UiScreen& screen, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->buildPanelScreen(screen);
}

void FrontlightPanelActivity::addSliderRow(UiScreen& screen, const uint8_t* icon, const uint8_t value,
                                           const fui::ActionId sliderAction, const fui::ActionId stepAction,
                                           const bool showToggle) {
  const auto& theme = screen.theme();

  // rowProps is a member (fui::SliderRowProps embeds a 324-byte StyleSet, well past the 256-byte
  // budget a local gets -- AGENTS.md). Every field that varies between the two rows is
  // reassigned here; the rest keep their constructed defaults.
  //
  // No caption line: the leading icon says which slider this is, so a "Brightness 100%" heading
  // above it is a second label for the same control. Screen::sliderRow() would overwrite
  // captionGap with the theme's spacing, so the row is themed and placed here and handed to the
  // component directly -- which also leaves room at the left for the icon.
  rowProps.label = nullptr;
  rowProps.value = nullptr;
  rowProps.sliderValue = value;
  rowProps.sliderAction = sliderAction;
  rowProps.decrement = stepAction;
  rowProps.increment = stepAction;
  rowProps.decrementValue = -BRIGHTNESS_STEP;
  rowProps.incrementValue = BRIGHTNESS_STEP;
  rowProps.labelText = theme.smallText;
  rowProps.buttonText = theme.titleText;
  rowProps.buttonText.bold = true;
  rowProps.gap = theme.spaceMd;
  // Round step buttons and a full stadium capsule, whatever the theme's corner radius: at this
  // height Lyra's 6 and Classic's 0 turn both into squared-off bars. Half the band height,
  // because 255 is RADIUS_INHERIT -- asking for it hands the shape back to the theme.
  rowProps.buttonRadius = static_cast<uint8_t>(kSliderRowHeight / 2);
  rowProps.capsuleRadius = static_cast<uint8_t>(kSliderRowHeight / 2);
  // Cancels the caption line sliderRow() reserves whether or not there is a label to put in it.
  rowProps.captionGap = static_cast<int16_t>(-screen.target().lineHeight(rowProps.labelText.font));
  if (showToggle) {
    // Lamp on/off after the +: the sliders set the level, this kills the light
    // outright. Filled glyph = on, outline = off.
    rowProps.toggleAction = ACTION_TOGGLE;
    rowProps.toggleIcon = fui::bitmapFromIcon(lightOn ? icon_sun_filled_32 : icon_sun_32);
  } else {
    rowProps.toggleAction = fui::NO_ACTION;
    rowProps.toggleIcon = fui::BitmapRef{};
  }

  fui::Rect band = screen.takeTop(fui::sliderRowHeight(screen.target(), rowProps, kSliderRowHeight), theme.spaceMd);
  renderer.drawIcon(icon, band.x, band.y + (band.height - kRowIconSize) / 2, kRowIconSize);
  const int16_t leading = static_cast<int16_t>(kRowIconSize + theme.spaceMd);
  band.x = static_cast<int16_t>(band.x + leading);
  band.width = static_cast<int16_t>(band.width - leading);
  fui::sliderRow(screen.frame(), band, rowProps);
  // The wrapper's own trailing gap is one spaceMd; double it so the rows
  // breathe -- a control band this tall reads cramped at the list cadence.
  screen.spacer(screen.theme().spaceMd);
}

void FrontlightPanelActivity::buildPanelScreen(UiScreen& screen) {
  const auto& theme = screen.theme();

  // Sheet chrome first: the card body, the 2px rule along its bottom edge, and
  // the grabber on the edge the sheet is dragged from. Screen::sheet() also
  // clamps the content area to the sheet, so every band below lays out inside
  // it. (No header: the panel is a floating card, and its own grabber says
  // what it is.)
  fui::SheetProps sheetProps;
  // A roomy band above the bottom rule: the grabber gets a full spaceLg of
  // air on both sides so the last row of content never crowds the sheet edge.
  sheetProps.grabberMargin = theme.spaceLg;
  sheetProps.grabberInset = static_cast<int16_t>(theme.spaceLg + theme.spaceMd);
  screen.sheet(sheetProps, static_cast<int16_t>(panelBottom));
  screen.insetContent(fui::Insets{0, kPanelSideMargin, 0, kPanelSideMargin});

  // Reuse the exact battery renderer and header rectangle used by Home. Call
  // the base implementation directly because RoundedRaff suppresses its
  // untitled Home header.
  {
    const auto& metrics = UITheme::getInstance().getMetrics();
    screen.spacer(theme.spaceMd);
    const int16_t bandH = std::max<int16_t>(static_cast<int16_t>(metrics.batteryHeight),
                                            screen.target().lineHeight(theme.smallText.font));
    screen.takeTop(bandH, theme.spaceMd);
    UITheme::getInstance().getTheme().BaseTheme::drawHeader(
        renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.homeTopPadding - metrics.topPadding},
        nullptr);
  }

  if (Frontlight.present()) {
    // No lamp button on the row: the light switch is one of the quick buttons below, so the
    // panel has a single place that turns it on and off.
    addSliderRow(screen, PanelBrightnessIcon, brightness, ACTION_BRIGHTNESS, ACTION_BRIGHTNESS_STEP,
                 /*showToggle=*/!mappedInput.hasTouch());
    if (Frontlight.hasColorTemperature()) {
      addSliderRow(screen, PanelWarmthIcon, warmth, ACTION_WARMTH, ACTION_WARMTH_STEP, /*showToggle=*/false);
    }
    screen.spacer(theme.spaceSm);
  }

  // Quick-setting buttons. Reserve the band here; the circles and captions draw as chrome in
  // render(), where the renderer's icon and circle primitives are to hand. Touch boards only --
  // they are touch targets.
  quickRowRect = fui::Rect{};
  if (mappedInput.hasTouch()) {
    const int16_t rowHeight = static_cast<int16_t>(kQuickCircle + kQuickLabelGap +
                                                   kQuickLabelLines * screen.target().lineHeight(theme.smallText.font));
    quickRowRect = screen.takeTop(rowHeight, theme.spaceSm);
  }
}

void FrontlightPanelActivity::drawQuickRow() {
  if (quickRowRect.width <= 0) return;
  const bool touchOn = SETTINGS.touchReaderControls != CrossPointSettings::TOUCH_READER_OFF;
  // The orientation button shows and names the mode it would GIVE you, not the one you are in:
  // four modes cycle, so a readout sitting among four buttons that all promise an action is the
  // one thing in the row that would have to be read differently.
  const int nextOrientation = (SETTINGS.orientation + 1) % 4;
  const bool nextIsLandscape = nextOrientation % 2 == 1;
  // Every glyph names the action, not the state: tap the moon to go dark, the sun to come back.
  const uint8_t* icons[kQuickCount] = {SETTINGS.screenInverted ? PanelSunIcon : PanelMoonIcon, PanelRefreshIcon,
                                       nextIsLandscape ? PanelLandscapeIcon : PanelPortraitIcon,
                                       touchOn ? PanelTouchOffIcon : PanelTouchOnIcon,
                                       lightOn ? PanelBulbOffIcon : PanelBulbIcon};
  static constexpr StrId kOrientNames[4] = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW,
                                            StrId::STR_ORIENTATION_INVERTED, StrId::STR_LANDSCAPE_CCW};
  // Every caption names what the tap DOES, like the glyph above it, and all five say it the same
  // way -- mixing an action ("Dark Mode") with a readout ("Portrait") in one row makes the reader
  // work out which is which. "Touch Off" was the worst of it: with touch currently on it reads
  // just as easily as the status "touch: off", the exact opposite of the truth, so those two
  // carry a verb instead.
  char touchLabel[64];
  snprintf(touchLabel, sizeof(touchLabel), "%s %s", I18N.get(touchOn ? StrId::STR_TURN_OFF : StrId::STR_TURN_ON),
           tr(STR_TOUCH_TOGGLE));
  char lightLabel[64];
  snprintf(lightLabel, sizeof(lightLabel), "%s %s", I18N.get(lightOn ? StrId::STR_TURN_OFF : StrId::STR_TURN_ON),
           tr(STR_LIGHT));
  const char* labels[kQuickCount] = {I18N.get(SETTINGS.screenInverted ? StrId::STR_LIGHT_MODE : StrId::STR_DARK_MODE),
                                     tr(STR_FORCE_REFRESH), I18N.get(kOrientNames[nextOrientation]), touchLabel,
                                     lightLabel};

  const int slot = quickRowRect.width / kQuickCount;
  const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  for (int i = 0; i < kQuickCount; ++i) {
    const int cx = quickRowRect.x + i * slot + slot / 2;
    const int circleX = cx - kQuickCircle / 2;
    renderer.drawRoundedRect(circleX, quickRowRect.y, kQuickCircle, kQuickCircle, /*lineWidth=*/2, kQuickCircle / 2,
                             true);
    renderer.drawIcon(icons[i], cx - kQuickIcon / 2, quickRowRect.y + (kQuickCircle - kQuickIcon) / 2, kQuickIcon);
    // Two lines: "Night Mode" and "Refresh Screen" do not fit a fifth of the panel on one, and
    // truncating them to "Night M..." says less than the icon above already does.
    const auto lines = renderer.wrappedText(SMALL_FONT_ID, labels[i], slot - 4, kQuickLabelLines);
    int labelY = quickRowRect.y + kQuickCircle + kQuickLabelGap;
    for (const auto& line : lines) {
      const int labelWidth = renderer.getTextWidth(SMALL_FONT_ID, line.c_str());
      renderer.drawText(SMALL_FONT_ID, cx - labelWidth / 2, labelY, line.c_str(), true);
      labelY += lineHeight;
    }
  }
}

int FrontlightPanelActivity::quickButtonAt(const int x, const int y) const {
  if (quickRowRect.width <= 0) return -1;
  if (y < quickRowRect.y || y >= quickRowRect.y + quickRowRect.height) return -1;
  const int slot = quickRowRect.width / kQuickCount;
  if (slot <= 0 || x < quickRowRect.x) return -1;
  const int idx = (x - quickRowRect.x) / slot;
  return idx >= 0 && idx < kQuickCount ? idx : -1;
}

void FrontlightPanelActivity::runQuickButton(const int idx) {
  if (idx == kQuickCount - 1) {
    toggleLight();
    return;
  }
  runTile(idx);
}

void FrontlightPanelActivity::render(RenderLock&&) {
  panelBottom = computePanelBottom();

  // fui::sheet draws the card body, its bottom rule, and the grabber during
  // renderUi(); the battery band at the card's top is part of the screen build.
  renderUi();
  drawQuickRow();

  // A tile that rewrote the whole frame (night mode) re-drives every pixel
  // once; ordinary repaints stay on the fast path. HALF: strong enough to
  // flip the whole frame's polarity without the FULL waveform's blackout
  // flash. Any faint residue clears with the panel's dedicated refresh tile
  // or the next scheduled clean refresh.
  renderer.displayBuffer(cleanRefreshPending ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  cleanRefreshPending = false;
}
