"""Run with python3 test/test_remote_button_input.py (requires a host C++ compiler)."""

from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "src/MappedInputManager.cpp").read_text()
# Exercise production frame/dispatch methods with physical input under test control.
methods = []
for signature, following in (
    ("void MappedInputManager::update(", "bool MappedInputManager::isNavDirectionSwapped("),
    ("bool MappedInputManager::injectRemoteClick(", "bool MappedInputManager::wasLongPressed("),
    ("bool MappedInputManager::isPressed(", "MappedInputManager::Labels MappedInputManager::mapLabels("),
):
    methods.append(signature + source.split(signature, 1)[1].split(following, 1)[0])

stub = r"""
#pragma once
#include <cstdint>
struct HalGPIO {
  static constexpr uint8_t BTN_BACK = 0, BTN_CONFIRM = 1, BTN_LEFT = 2, BTN_RIGHT = 3, BTN_UP = 4, BTN_DOWN = 5,
                           BTN_POWER = 6;
  bool pressed = false, released = false, held = false;
  bool anyButtonDownRaw() const { return held; }
  uint8_t button = 0;
  void update() {}
  bool wasAnyPressed() const { return pressed; }
  bool wasAnyReleased() const { return released; }
  bool wasPressed(uint8_t b) const { return b == button && pressed; }
  bool wasReleased(uint8_t b) const { return b == button && released; }
  bool isPressed(uint8_t b) const { return b == button && held; }
  unsigned long getHeldTime() const { return 900; }
  bool wasCapacitivePagePressed() const { return false; }
  bool hasHomeKey() const { return false; }
  bool wasHomeKeyTapped() const { return false; }
  bool wasHomeKeyLongPressed() const { return false; }
  bool wasHomeKeyPressed() const { return false; }
  bool wasTouchActivity() const { return false; }
};
"""
harness = r"""
#include <cassert>
#include "MappedInputManager.h"
#include "util/SideButtonActions.h"
class GfxRenderer {};
unsigned long millis() { return 1000; }
constexpr unsigned long TOUCH_HELD_OVERRIDE_WINDOW_MS = 250;
struct CrossPointSettings {
  enum { VIBRATION_OFF, VIBRATION_TOUCH_PAGE };
  enum { IGNORE = 0, PWR_BOOKMARK = 7, PWR_SYNC = 8, PWR_READER_MENU = 9 };
  int shortPwrBtn = IGNORE;
  int vibration = VIBRATION_OFF, hapticIntensity = 0;
  int homeButtonTapAction = 0, homeButtonDoubleTapAction = 0, homeButtonLongPressAction = 0;
} SETTINGS;
struct HalHaptics {
  static void feedback(bool, bool, int) {}
  static void longPress(bool, int) {}
};
bool MappedInputManager::mapButton(Button b, bool (HalGPIO::*fn)(uint8_t) const) const {
  return (gpio.*fn)(static_cast<uint8_t>(b));
}
bool MappedInputManager::wasBackGesture() const { return false; }
MappedInputManager::SwipeDir MappedInputManager::wasSwipe(int*, int*) const { return SwipeDir::None; }
""" + "\n".join(methods) + r"""
int main() {
  HalGPIO gpio;
  GfxRenderer renderer;
  MappedInputManager input(gpio, renderer);
  using Button = MappedInputManager::Button;
  struct Mapping { RemoteButton remote; Button button; Button other; };
  for (const auto mapping : {
      Mapping{RemoteButton::Confirm, Button::Confirm, Button::Back},
      Mapping{RemoteButton::Back, Button::Back, Button::Confirm},
      Mapping{RemoteButton::Next, Button::NavNext, Button::NavPrevious},
      Mapping{RemoteButton::Next, Button::Right, Button::Left},
      Mapping{RemoteButton::Previous, Button::NavPrevious, Button::NavNext},
      Mapping{RemoteButton::Previous, Button::Left, Button::Right}}) {
    const auto remote = mapping.remote;
    const auto button = mapping.button;
    const auto other = mapping.other;
    input.update();
    assert(input.injectRemoteClick(remote));
    assert(input.hasRemoteClick());
    assert(input.wasPressed(button) && input.wasReleased(button));
    assert(!input.wasPressed(other) && !input.wasReleased(other));
    assert(!input.isPressed(button) && input.getHeldTime() == 0);
    assert(!input.injectRemoteClick(remote));
    input.update();
    assert(!input.hasRemoteClick());
    assert(!input.wasPressed(button) && !input.wasReleased(button));
    assert(input.getHeldTime() == 900);
  }
  // Physical edges and held Confirm/Back must win over a simultaneous remote click.
  gpio.pressed = true;
  assert(!input.injectRemoteClick(RemoteButton::Confirm));
  gpio.pressed = false;
  gpio.released = true;
  assert(!input.injectRemoteClick(RemoteButton::Back));
  gpio.released = false;
  gpio.held = true;
  for (Button button : {Button::Confirm, Button::Back}) {
    gpio.button = static_cast<uint8_t>(button);
    assert(!input.injectRemoteClick(RemoteButton::Confirm));
    assert(!input.injectRemoteClick(RemoteButton::Back));
  }
  gpio.held = false;
  assert(input.injectRemoteClick(RemoteButton::Back));
}
"""
manager_source = (ROOT / "src/activities/ActivityManager.cpp").read_text()
remote_turn = "bool ActivityManager::remoteTurn(" + manager_source.split(
    "bool ActivityManager::remoteTurn(", 1
)[1].split("bool ActivityManager::handleForcedRefresh()", 1)[0]
harness = harness.replace("int main() {", r"""
#include <algorithm>
#include <atomic>
#include <memory>
#include "activities/reader/RemoteTurnGate.h"
struct RenderLock {
  static inline bool busy = false;
  static bool peek() { return busy; }
};
constexpr int MAX_QUEUED_REMOTE_TURNS = 3;
constexpr unsigned long QUEUED_REMOTE_TURN_MAX_AGE_MS = 4000;
struct ReaderActivity {
  std::atomic<bool> pageRendered{true};
  bool menu = false;
  int pages = 0, chapters = 0, interrupts = 0;
  bool inputOverPage() const { return menu; }
  void interruptBackgroundWork() { interrupts++; }
  int menus = 0;
  bool remoteShortcut(bool readerMenu) { menus += readerMenu ? 1 : 0; return true; }
  bool pageTurn(bool forward) { pages += forward ? 1 : -1; return true; }
  bool skipPages(int step) { chapters += step; return true; }
};
struct ActivityManager {
  MappedInputManager& mappedInput;
  std::unique_ptr<ReaderActivity> currentActivity = std::make_unique<ReaderActivity>();
  bool context = true, foregroundReaderShown = true;
  std::atomic<bool> requestedUpdate{false};
  uint32_t screenVisit = 1;
  enum class QueuedShortcut : uint8_t { None, ReaderMenu, SaveQuote };
  int8_t queuedRemoteTurns = 0;
  RemoteButton queuedRemoteClick = RemoteButton::None;
  QueuedShortcut queuedRemoteShortcut = QueuedShortcut::None;
  uint32_t queuedRemoteTurnVisit = 0;
  unsigned long queuedRemoteTurnAtMs = 0;
  void noteQueuedRemoteInput();
  bool isRemoteReaderContext() const { return context; }
  bool isForegroundReader() const { return foregroundReaderShown; }
  ReaderActivity* foregroundReader() const { return foregroundReaderShown ? currentActivity.get() : nullptr; }
  bool remoteClick(RemoteButton button) { return mappedInput.injectRemoteClick(button); }
  void requestUpdate() {}
  bool remoteTurn(bool forward, bool chapter);
  bool applyQueuedRemoteInput();
};
""" + remote_turn + r"""
void checkRouting(MappedInputManager& input) {
  ActivityManager manager{input};
  input.update();
  assert(manager.remoteTurn(true, false));
  assert(manager.currentActivity->pages == 1 && !input.hasRemoteClick());
  for (bool reader : {true, false}) {
    manager.foregroundReaderShown = reader;
    manager.currentActivity->menu = true;
    for (bool forward : {true, false}) {
      input.update();
      assert(manager.remoteTurn(forward, false));
      assert(input.wasPressed(forward ? MappedInputManager::Button::NavNext
                                     : MappedInputManager::Button::NavPrevious));
      assert(manager.currentActivity->pages == 1);
      assert(!manager.remoteTurn(forward, false));
    }
    input.update();
    assert(!manager.remoteTurn(true, true));
    assert(manager.currentActivity->chapters == 0 && !input.hasRemoteClick());
  }
  manager.context = false;
  assert(!manager.remoteTurn(true, false));
}
void checkQueue(MappedInputManager& input) {
  ActivityManager manager{input};
  input.update();
  // Presses during a draw are kept, capped at three, and interrupt the render task's background work.
  RenderLock::busy = true;
  for (int i = 0; i < 5; i++) assert(manager.remoteTurn(true, false));
  assert(manager.currentActivity->pages == 0 && manager.currentActivity->interrupts == 5);
  assert(!manager.applyQueuedRemoteInput());
  assert(!manager.remoteTurn(true, true));  // chapter skips are not queued
  RenderLock::busy = false;
  for (int i = 0; i < 3; i++) assert(manager.applyQueuedRemoteInput());
  assert(!manager.applyQueuedRemoteInput());
  assert(manager.currentActivity->pages == 3);
  // Opposite turns cancel; a changed screen drops what is left.
  RenderLock::busy = true;
  assert(manager.remoteTurn(true, false) && manager.remoteTurn(false, false) && manager.remoteTurn(false, false));
  RenderLock::busy = false;
  manager.screenVisit++;
  assert(!manager.applyQueuedRemoteInput() && manager.currentActivity->pages == 3);
  // A held shortcut replaces the held turns: it opens something over the page.
  manager.queuedRemoteTurnVisit = manager.screenVisit;
  manager.queuedRemoteTurnAtMs = millis();
  manager.queuedRemoteTurns = 2;
  manager.queuedRemoteShortcut = ActivityManager::QueuedShortcut::ReaderMenu;
  assert(manager.applyQueuedRemoteInput() && manager.currentActivity->menus == 1);
  assert(!manager.applyQueuedRemoteInput() && manager.currentActivity->pages == 3);
}
int main() {
""")
harness = harness.replace("  using Button =", "  checkRouting(input);\n  checkQueue(input);\n  using Button =")
with tempfile.TemporaryDirectory() as directory:
    directory = Path(directory)
    (directory / "HalGPIO.h").write_text(stub)
    cpp = directory / "remote_input.cpp"
    executable = directory / "remote_input"
    cpp.write_text("#include <initializer_list>\n" + harness)
    subprocess.run([
        "c++", "-std=c++20", "-DFREEINK_CAP_TOUCH=0", "-I", str(directory),
        "-I", str(ROOT / "src"), str(cpp), "-o", str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True)
print("Remote button input regression checks passed")
