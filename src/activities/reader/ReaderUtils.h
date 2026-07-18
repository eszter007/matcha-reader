#pragma once

#include <CrossPointSettings.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <HalTiltSensor.h>
#include <Logging.h>
#include <Memory.h>

#include <ctime>

#include "MappedInputManager.h"
#include "ReadingStatsStore.h"

namespace ReaderUtils {

constexpr unsigned long GO_HOME_MS = 1000;
constexpr unsigned long SKIP_HOLD_MS = 700;
constexpr unsigned long BOOKMARK_HOLD_MS = 400;
constexpr unsigned long BOOKMARK_MESSAGE_DURATION_MS = 2500;

// Reading-stats heartbeat interval: one load/add/save round per flush, so this also
// throttles SD writes (see the settings-write throttling rule).
constexpr unsigned long READING_STATS_FLUSH_MS = 5UL * 60UL * 1000UL;

// Flush whole elapsed minutes of the current reading session into READING_STATS.
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
// TEMPORARY DIAGNOSTIC (remove before merging): append one line per stats event to
// /system/stats_trace.txt so a device without serial access can report why days stop
// registering (reported regression: no day recorded since Jul 13). HalStorage has no
// append mode (openFileForWrite is O_TRUNC), so read-modify-rewrite with a transient
// 2KB heap buffer; when the file approaches the cap it restarts from empty. Bounded to
// one write per flush attempt by the 5-minute throttle.
inline void statsTrace(const char* tag, const unsigned long sessionStartMs, const unsigned long elapsed,
                       const uint16_t minutes, const int loadOk, const int saveOk) {
  constexpr size_t CAP = 2048;
  constexpr size_t TRACE_LINE_MAX = 96;  // LINE_MAX is taken by <limits.h>
  auto buf = makeUniqueNoThrow<char[]>(CAP);
  if (!buf) return;  // diagnostics must never take down the reader
  char* base = buf.get();
  size_t len = Storage.readFileToBuffer("/system/stats_trace.txt", base, CAP);
  if (len > CAP - TRACE_LINE_MAX) len = 0;  // cap reached: restart the trace
  const int n = snprintf(base + len, TRACE_LINE_MAX, "%lu %s start=%lu el=%lu min=%u load=%d save=%d\n", millis(), tag,
                         sessionStartMs, elapsed, minutes, loadOk, saveOk);
  if (n <= 0) return;
  HalFile f;
  if (!Storage.openFileForWrite("STATS", "/system/stats_trace.txt", f)) return;
  f.write(reinterpret_cast<const uint8_t*>(base), len + static_cast<size_t>(n));
}

inline void flushReadingStats(unsigned long& sessionStartMs, const bool force = false) {
  if (sessionStartMs == 0) {
    if (force) statsTrace("exit-nostart", 0, 0, 0, -1, -1);
    return;
  }
  const unsigned long elapsed = millis() - sessionStartMs;
  if (!force && elapsed < READING_STATS_FLUSH_MS) return;
  const uint16_t minutes = static_cast<uint16_t>(elapsed / 60000UL);
  if (minutes == 0) {
    if (force) statsTrace("exit-zero", sessionStartMs, elapsed, 0, -1, -1);
    return;
  }
  // Local-midnight day boundary: shift by the user's display UTC offset so an evening
  // session doesn't get logged against "tomorrow" (UTC midnight is 9am in Japan).
  const time_t now = HalClock::localEpoch(SETTINGS.clockUtcOffsetQ);
  const struct tm* t = gmtime(&now);
  const bool loadOk = READING_STATS.loadFromFile();
  READING_STATS.addMinutes(static_cast<uint16_t>(t->tm_year + 1900), static_cast<uint8_t>(t->tm_mon + 1),
                           static_cast<uint8_t>(t->tm_mday), minutes);
  const bool saveOk = READING_STATS.saveToFile();
  if (!saveOk) {
    LOG_ERR("STATS", "saveToFile failed (load=%d, %u min lost from file)", loadOk, minutes);
  }
  statsTrace(force ? "exit-flush" : "beat-flush", sessionStartMs, elapsed, minutes, loadOk, saveOk);
  sessionStartMs += static_cast<unsigned long>(minutes) * 60000UL;
}

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
  const bool prev =
      tiltPrev ||
      (usePress ? (input.wasPressed(MappedInputManager::Button::PageBack) || input.wasPressed(prevButton))
                : (input.wasReleased(MappedInputManager::Button::PageBack) || input.wasReleased(prevButton)));
  const bool powerTurn = SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN &&
                         input.wasReleased(MappedInputManager::Button::Power);
  const bool next = tiltNext || (usePress ? (input.wasPressed(MappedInputManager::Button::PageForward) || powerTurn ||
                                             input.wasPressed(nextButton))
                                          : (input.wasReleased(MappedInputManager::Button::PageForward) || powerTurn ||
                                             input.wasReleased(nextButton)));
  return {prev, next, tiltPrev || tiltNext};
}

inline void displayWithRefreshCycle(const GfxRenderer& renderer, int& pagesUntilFullRefresh) {
  if (pagesUntilFullRefresh <= 1) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    renderer.displayBuffer();
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

}  // namespace ReaderUtils
