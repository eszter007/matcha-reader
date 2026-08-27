#pragma once

#include <CrossPointSettings.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalGPIO.h>
#include <HalTiltSensor.h>
#include <Logging.h>
#include <components/bars/tap-zones.h>

#include <ctime>
#include <string>

#include "BookStats.h"
#include "MappedInputManager.h"
#include "ReadingStatsStore.h"
#include "activities/ActivityManager.h"

namespace ReaderUtils {

constexpr unsigned long GO_HOME_MS = 1000;
constexpr unsigned long GO_BACK_OR_HOME_MS = GO_HOME_MS;
constexpr unsigned long SKIP_HOLD_MS = 700;
constexpr unsigned long BOOKMARK_HOLD_MS = 400;
constexpr unsigned long BOOKMARK_MESSAGE_DURATION_MS = 2500;

// Reading-stats heartbeat interval: one load/add/save round per flush, so this also
// throttles SD writes (see the settings-write throttling rule).
constexpr unsigned long READING_STATS_FLUSH_MS = 5UL * 60UL * 1000UL;

// Flush whole elapsed minutes of the current reading session into READING_STATS_STORE.
//
// Stats used to be written only in the readers' onExit(), so any exit path that never
// runs it -- a hang or watchdog reset on the sleep transition, a battery pull, a crash
// -- silently lost the entire session: days stopped registering and the streak broke
// while page progress (saved on page turns) kept working. Call this periodically from
// the reader's loop() (it self-throttles to one SD write per READING_STATS_FLUSH_MS)
// and with force=true from onExit() to record the sub-interval tail.
//
// The sub-minute remainder is carried forward in sessionStartMs so repeated flushes
// never drop seconds.
//
// bookPath/language attribute the same minutes to one book, and to that day in that language,
// as well as to the day overall (issue #38). Both are const char* rather than std::string:
// this runs on every loop() tick and returns early most of the time, so string parameters
// would mean a heap allocation per tick for arguments almost always thrown away.
// Both are optional: an empty bookPath records only the per-day total, and an empty language is
// the normal case for books that declare none (TXT/XTC, manga converted before meta.bin carried
// a language tag) -- it is filled in on a later flush if the book ever starts declaring one.
inline void flushReadingStats(unsigned long& sessionStartMs, const bool force = false, const char* bookPath = nullptr,
                              const char* language = nullptr) {
  if (sessionStartMs == 0) return;
  const unsigned long elapsed = millis() - sessionStartMs;
  if (!force && elapsed < READING_STATS_FLUSH_MS) return;
  const uint16_t minutes = static_cast<uint16_t>(elapsed / 60000UL);
  if (minutes == 0) return;
  // Local-midnight day boundary: shift by the user's display UTC offset so an evening
  // session doesn't get logged against "tomorrow" (UTC midnight is 9am in Japan).
  // gmtime_r into a stack tm: gmtime()'s shared static buffer is not safe with the
  // render task also converting time (matches HalClock's usage).
  const time_t now = HalClock::localEpoch(SETTINGS.clockUtcOffsetQ);
  struct tm t = {};
  gmtime_r(&now, &t);
  const bool loadOk = READING_STATS_STORE.loadFromFile();
  READING_STATS_STORE.addMinutes(static_cast<uint16_t>(t.tm_year + 1900), static_cast<uint8_t>(t.tm_mon + 1),
                                 static_cast<uint8_t>(t.tm_mday), minutes);
  READING_STATS_STORE.addBookMinutes(bookPath, language, minutes, static_cast<uint16_t>(t.tm_year + 1900),
                                     static_cast<uint8_t>(t.tm_mon + 1), static_cast<uint8_t>(t.tm_mday));
  READING_STATS_STORE.addLanguageMinutes(language, minutes, static_cast<uint16_t>(t.tm_year + 1900),
                                         static_cast<uint8_t>(t.tm_mon + 1), static_cast<uint8_t>(t.tm_mday));
  if (!READING_STATS_STORE.saveToFile()) {
    LOG_ERR("STATS", "saveToFile failed (load=%d, %u min lost from file)", loadOk, minutes);
  }
  // Per-book day history (see BookStats). Loaded and dropped here: nothing stays in DRAM.
  if (bookPath && *bookPath) {
    BookStats bookStats;
    if (!bookStats.load(bookPath)) {
      LOG_ERR("STATS", "book stats load failed (%u min not attributed)", minutes);
    } else {
      bookStats.recordMinutes(static_cast<uint16_t>(t.tm_year + 1900), static_cast<uint8_t>(t.tm_mon + 1),
                              static_cast<uint8_t>(t.tm_mday), minutes);
      if (!bookStats.save()) LOG_ERR("STATS", "book stats save failed (%u min not attributed)", minutes);
    }
  }
  sessionStartMs += static_cast<unsigned long>(minutes) * 60000UL;
}
enum ReaderTouchAction : freeink::ui::ActionId {
  READER_TOUCH_PREV = 1,
  READER_TOUCH_NEXT = 3,
};

inline void applyOrientation(GfxRenderer& renderer, const uint8_t orientation) {
  switch (orientation) {
    case CrossPointSettings::ORIENTATION::PORTRAIT:
      renderer.setOrientation(GfxRenderer::Orientation::Portrait);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);
      break;
    case CrossPointSettings::ORIENTATION::INVERTED:
      renderer.setOrientation(GfxRenderer::Orientation::PortraitInverted);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CCW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise);
      break;
    default:
      break;
  }
}

struct PageTurnResult {
  bool prev;
  bool next;
  bool fromTilt;
};

inline PageTurnResult detectPageTurn(const MappedInputManager& input) {
  const bool usePress = SETTINGS.longPressButtonBehavior == SETTINGS.OFF;
  const bool tiltNext = SETTINGS.tiltPageTurn && halTiltSensor.wasTiltedForward();
  const bool tiltPrev = SETTINGS.tiltPageTurn && halTiltSensor.wasTiltedBack();
  const bool swapFront = input.isNavDirectionSwapped();
  const auto prevButton = swapFront ? MappedInputManager::Button::Right : MappedInputManager::Button::Left;
  const auto nextButton = swapFront ? MappedInputManager::Button::Left : MappedInputManager::Button::Right;
  const auto pageButtonTriggered = [&](const MappedInputManager::Button button) {
    if (usePress) return input.wasPressed(button);
    return input.wasLongPressed(button, SKIP_HOLD_MS) || input.wasReleased(button);
  };
  const bool prev =
      tiltPrev || (pageButtonTriggered(MappedInputManager::Button::PageBack) || pageButtonTriggered(prevButton));
  const bool powerTurn = SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN &&
                         input.wasReleased(MappedInputManager::Button::Power);
  const bool next = tiltNext || pageButtonTriggered(MappedInputManager::Button::PageForward) || powerTurn ||
                    pageButtonTriggered(nextButton);
  return {prev, next, tiltPrev || tiltNext};
}

// A short power-button click closes the dictionary / word-lookup screens, but only when that same
// click is what opens them (SHORT_PWRBTN::WORD_LOOKUP). The button sits under the holding hand's
// index finger, so entering AND leaving with it is what makes the shortcut one-handed -- opening
// with the power button and having to reach across for Back defeats the point.
//
// Left alone under every other shortPwrBtn value: Sleep, Page Turn, Force Refresh and Footnotes
// each own the click elsewhere, and a lookup screen must not swallow it from them.
//
// The Down term mirrors the open paths (EpubReaderActivity, MangaReaderActivity) exactly: a power
// release that arrives in the SAME input update as a Down release is the Power+Down screenshot
// combo being let go, not a request to leave, so the screen survives being screenshotted. Down held
// on its own is not consulted -- only the two releases coinciding.
// A short power click inside the word-lookup panel, when the user has bound the short click to
// Word Lookup. What it DOES depends on the view: on the page it selects the highlighted word;
// in the definition it closes the dictionary outright.
inline bool wordLookupPowerClick(const MappedInputManager& input) {
  return SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::WORD_LOOKUP &&
         input.wasReleased(MappedInputManager::Button::Power) && !gpio.wasReleased(HalGPIO::BTN_DOWN);
}

struct TouchPageTurn {
  bool prev;
  bool next;
  unsigned long heldMs;
};

inline TouchPageTurn detectTouchPageTurn(GfxRenderer& renderer, const MappedInputManager& input) {
  TouchPageTurn result{false, false, 0};
  if (!SETTINGS.touchReaderControls || !input.hasTouch()) {
    return result;
  }

  const bool swipeMode = SETTINGS.touchReaderControls == CrossPointSettings::TOUCH_READER_SWIPE ||
                         SETTINGS.touchReaderControls == CrossPointSettings::TOUCH_READER_INVERTED_SWIPE;
  if (swipeMode) {
    // Horizontal swipes turn pages; taps remain free for the centered reader-menu
    // zone. A slow swipe never becomes a long-press chapter skip.
    // Inverted reverses which way advances, the same choice INVERTED_TAP offers for taps:
    // vertical Japanese text reads right-to-left, so the advancing gesture runs the other way.
    const bool invertedSwipe = SETTINGS.touchReaderControls == CrossPointSettings::TOUCH_READER_INVERTED_SWIPE;
    const auto dir = input.wasSwipe();
    if (dir == MappedInputManager::SwipeDir::Left) {
      result.next = !invertedSwipe;
      result.prev = invertedSwipe;
    } else if (dir == MappedInputManager::SwipeDir::Right) {
      result.prev = !invertedSwipe;
      result.next = invertedSwipe;
    }
    return result;
  }

  int x = 0;
  int y = 0;
  if (!input.wasScreenTapped(x, y)) {
    return result;
  }

  const int16_t width = static_cast<int16_t>(renderer.getScreenWidth());
  const int16_t height = static_cast<int16_t>(renderer.getScreenHeight());
  // Outer thirds only: the center column contains the reader-menu tap target
  // (isTouchMenuTap below), so it must not double as a page turn.
  const int16_t zoneWidth = width / 3;
  const bool inverted = SETTINGS.touchReaderControls == CrossPointSettings::TOUCH_READER_INVERTED_TAP;
  const freeink::ui::TapZone zones[] = {
      {freeink::ui::Rect{0, 0, zoneWidth, height}, inverted ? READER_TOUCH_NEXT : READER_TOUCH_PREV},
      {freeink::ui::Rect{static_cast<int16_t>(width - zoneWidth), 0, zoneWidth, height},
       inverted ? READER_TOUCH_PREV : READER_TOUCH_NEXT},
  };

  for (const auto& zone : zones) {
    if (!zone.enabled || !zone.rect.contains(static_cast<int16_t>(x), static_cast<int16_t>(y))) continue;
    result.prev = zone.action == READER_TOUCH_PREV;
    result.next = zone.action == READER_TOUCH_NEXT;
    break;
  }
  result.heldMs = gpio.lastTouchHeldMs();
  return result;
}

// Tap in the center third of the screen: the tap path into the reader menu on
// every touch board. The page-turn tap zones are the outer horizontal thirds,
// so the centered rectangle remains free in tap mode. The Off/Swipe Up
// alternatives are only surfaced on home-key boards (SettingsList), where the
// menu stays reachable through the key's long-press function.
inline bool isTouchMenuTap(const GfxRenderer& renderer, const MappedInputManager& input) {
  if (!input.hasTouch()) return false;
  if (SETTINGS.showReaderMenu != CrossPointSettings::READER_MENU_TAP) return false;
  int x = 0;
  int y = 0;
  if (!input.wasScreenTapped(x, y)) return false;
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  const int zoneWidth = width / 3;
  const int zoneHeight = height / 3;
  return x >= zoneWidth && x < width - zoneWidth && y >= zoneHeight && y < height - zoneHeight;
}

// Reader menu opens on the menu edge-swipe or a center-third tap. On home-key
// boards a long press of the capacitive key runs the user-selected long-press
// function instead (SETTINGS.longPressMenuFunction), not the menu.
// With touch reader controls Off the reading surface ignores touch entirely,
// menu included, so a stray brush of the screen can't open it; the menu stays
// reachable via the Confirm button.
inline bool isTouchMenuGesture(const GfxRenderer& renderer, const MappedInputManager& input) {
  if (!SETTINGS.touchReaderControls) return false;
  if (!input.hasTouch()) return false;
  if (input.wasMenuGesture()) return true;
  // Bottom-edge up-swipe variant: only selectable on home-key boards, where
  // Home is the capacitive key and the bottom edge is otherwise unused.
  if (SETTINGS.showReaderMenu == CrossPointSettings::READER_MENU_SWIPE_UP && input.wasReaderMenuSwipeUp()) {
    return true;
  }
  return isTouchMenuTap(renderer, input);
}

// One helper, blocking or deferred: the async form starts the refresh and
// returns so the caller can overlap CPU work with the panel's refresh time.
// Async callers must not touch the framebuffer until
// renderer.waitRefreshComplete() and must rebuild the differential baseline
// before the next page turn (the tiled grayscale cleanup does).
inline void displayWithRefreshCycle(const GfxRenderer& renderer, int& pagesUntilFullRefresh, bool async = false) {
  const auto mode = (pagesUntilFullRefresh <= 1) ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH;
  if (async) {
    renderer.displayBufferAsync(mode);
  } else {
    renderer.displayBuffer(mode);
  }
  if (pagesUntilFullRefresh <= 1) {
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    pagesUntilFullRefresh--;
  }
}

// Display the B/W base of a page whose grayscale pass follows. Panels that
// combine the base (Paper Mono) defer the activation so base + gray planes go
// out as one waveform — displaying the base separately makes the gray pass
// re-drive the whole text body (a visible flash). Other panels display
// normally. Same refresh-cadence bookkeeping as displayWithRefreshCycle.
inline void displayBaseWithRefreshCycle(const GfxRenderer& renderer, int& pagesUntilFullRefresh) {
  if (!renderer.combinesGrayscaleBase()) {
    displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
    return;
  }
  const auto mode = (pagesUntilFullRefresh <= 1) ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH;
  renderer.displayGrayscaleBase(mode);
  if (pagesUntilFullRefresh <= 1) {
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    pagesUntilFullRefresh--;
  }
}

// Grayscale anti-aliasing pass. Renders content twice (LSB + MSB) to build
// the grayscale buffer. Only the content callback is re-rendered — status bars
// and other overlays should be drawn before calling this.
// Kept as a template to avoid std::function overhead; instantiated once per reader type.
template <typename RenderFn>
void renderAntiAliased(GfxRenderer& renderer, RenderFn&& renderFn) {
  if (!renderer.storeBwBuffer()) {
    LOG_ERR("READER", "Failed to store BW buffer for anti-aliasing");
    // A combined-base panel may still hold a deferred B/W activation; flush it
    // so the page reaches the panel even without its grays.
    if (renderer.combinesGrayscaleBase()) renderer.cleanupGrayscaleWithFrameBuffer();
    return;
  }

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  renderFn();
  renderer.copyGrayscaleLsbBuffers();

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  renderFn();
  renderer.copyGrayscaleMsbBuffers();

  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);

  renderer.restoreBwBuffer();
}

struct BackNavCallback {
  void* ctx;
  void (*fn)(void*);
};

// Returns true if the back button was consumed (caller should return).
// Long press (>= GO_BACK_OR_HOME_MS):
// - default: go to file browser
// - with backShortToFileBrowser: go home
// Short press (< GO_BACK_OR_HOME_MS):
// - default: go home
// - with backShortToFileBrowser: go to file browser.
inline bool handleBackNavigation(const MappedInputManager& mappedInput, ActivityManager& activityManager,
                                 const char* filePath, BackNavCallback goHome) {
  // The reading surface deliberately has no left-edge swipe-to-exit path: in
  // swipe page-turn mode a right swipe must page back instead. Home remains
  // available through the board's dedicated Home gesture/key. Back swipes stay
  // available in menus and other activities; only this reader-surface handler
  // ignores them. Physical Back buttons are unaffected: isPressed() is
  // button-only, and this guard skips just the gesture's own release frame.
  if (mappedInput.wasBackGesture()) {
    return false;
  }

  const bool backTriggered = mappedInput.wasLongPressed(MappedInputManager::Button::Back, GO_BACK_OR_HOME_MS) ||
                             mappedInput.wasReleased(MappedInputManager::Button::Back);
  if (!backTriggered) return false;

  const bool longPress = mappedInput.getHeldTime() >= GO_BACK_OR_HOME_MS;
  if (longPress != SETTINGS.backShortToFileBrowser) {
    activityManager.goToFileBrowser(filePath);
  } else {
    goHome.fn(goHome.ctx);
  }
  return true;
}

}  // namespace ReaderUtils
