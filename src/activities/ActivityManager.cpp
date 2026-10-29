#include "ActivityManager.h"

#include <BlePageTurner.h>
#include <BoardConfig.h>
#include <FontCacheManager.h>
#include <HalDisplay.h>
#include <HalFrontlight.h>
#include <HalPowerManager.h>
#include <Memory.h>
#include <VectorFontSupport.h>

#include <algorithm>

#include "Activity.h"
#include "CrossPointSettings.h"
#include "HapticFeedback.h"
#include "OpdsServerStore.h"
#include "boot_sleep/BootActivity.h"
#include "boot_sleep/SleepActivity.h"
#include "browser/OpdsBookBrowserActivity.h"
#include "components/HeaderBackTapTarget.h"
#include "home/CrashActivity.h"
#include "home/FileBrowserActivity.h"
#include "home/HomeActivity.h"
#include "home/ReadingStatsActivity.h"
#include "library/CoverLibraryActivity.h"
#include "library/LibraryListActivity.h"
#include "network/CrossPointWebServerActivity.h"
#include "network/UsbDriveActivity.h"
#include "plugins/PluginCatalogActivity.h"
#include "reader/ReaderActivity.h"
#include "reader/RemoteTurnGate.h"
#include "settings/OpdsServerListActivity.h"
#include "settings/SettingsListActivity.h"
// Upstream also includes BmpViewerActivity.h here; Matcha's consolidated reader owns BMP
// viewing (ReaderActivity::onGoToBmpViewer), so nothing in this file references it.
#include "util/FrontlightPanelActivity.h"
#include "util/FullScreenMessageActivity.h"

static portMUX_TYPE activityManagerSpinlock = portMUX_INITIALIZER_UNLOCKED;

ActivityManager::ActivityManager(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : renderer(renderer), mappedInput(mappedInput), renderingMutex(xSemaphoreCreateMutex()) {
  assert(renderingMutex != nullptr && "Failed to create rendering mutex");
  stackActivities.reserve(10);
}

ActivityManager::~ActivityManager() { assert(false); /* should never be called */ }

void ActivityManager::begin() {
#if defined(configNUM_CORES) && configNUM_CORES > 1
  constexpr BaseType_t renderTaskCore = 1;
#else
  constexpr BaseType_t renderTaskCore = 0;
#endif
#if CROSSPOINT_VECTOR_FONTS
  // FreeType rasterization runs on this task, and the deepest observed chain
  // is a glyph fault DURING LAYOUT: expat + parser + line-layout frames
  // (~3.5KB on Xtensa) with the scan converter's FT_RENDER_POOL_SIZE (4KB)
  // stack-resident band pool on top — a measured ~8KB peak that trips the
  // canary on an 8KB stack. Vector-font boards all have PSRAM-class RAM.
  constexpr uint32_t renderTaskStackBytes = 16384;
#else
  constexpr uint32_t renderTaskStackBytes = 8192;
#endif
  xTaskCreatePinnedToCore(&renderTaskTrampoline, "ActivityManagerRender",
                          renderTaskStackBytes,  // Stack size
                          this,                  // Parameters
                          1,                     // Priority
                          &renderTaskHandle,     // Task handle
                          renderTaskCore  // Keep long renders/cover decodes off CPU 0's idle watchdog when available
  );
  assert(renderTaskHandle != nullptr && "Failed to create render task");
}

void ActivityManager::renderTaskTrampoline(void* param) {
  auto* self = static_cast<ActivityManager*>(param);
  self->renderTaskLoop();
}

void ActivityManager::renderTaskLoop() {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    // Acquire the lock before reading currentActivity to avoid a TOCTOU race
    // where the main task deletes the activity between the null-check and render().
    RenderLock lock;
    if (currentActivity) {
      HalPowerManager::Lock powerLock;  // Ensure we don't go into low-power mode while rendering
      // Night mode is a global output polarity applied to every activity.
      // The sleep screen forces normal polarity itself (SleepActivity).
      display.setInverted(SETTINGS.screenInverted != 0);
      const uint32_t renderingVisit = screenVisit;
      currentActivity->render(std::move(lock));
      paintedVisit.store(renderingVisit, std::memory_order_release);
    }
    // Notify any task blocked in requestUpdateAndWait() that the render is done.
    TaskHandle_t waiter = nullptr;
    taskENTER_CRITICAL(&activityManagerSpinlock);
    waiter = waitingTaskHandle;
    waitingTaskHandle = nullptr;
    taskEXIT_CRITICAL(&activityManagerSpinlock);
    if (waiter) {
      xTaskNotify(waiter, 1, eIncrement);
    }
  }
}

void ActivityManager::loop() {
  if (mappedInput.consumeSuppressedRelease()) return;

  if (currentActivity && currentActivity->requiresExclusiveStorageLoop()) {
    currentActivity->loop();
    // An exclusive-storage activity must restart rather than navigate away:
    // processing a pending action here could re-enable filesystem users while
    // the USB host still owns the raw SD card.
    if (requestedUpdate.exchange(false) && renderTaskHandle) {
      xTaskNotify(renderTaskHandle, 1, eIncrement);
    }
    return;
  }

  if (currentActivity) {
    // Before anything below waits on the RenderLock: going home pops the reader, which blocks until
    // the render task's background work ends. Buttons alone are seen by the reader itself; the Home
    // key and touch never reach it.
    if (auto* reader = foregroundReader(); reader && (mappedInput.wasAnyPressed() || mappedInput.wasTouchActivity())) {
      reader->interruptBackgroundWork();
    }
    if (!currentActivity->isHomeActivity() && mappedInput.wasHomeGesture()) {
      if (currentActivity->handleHomeGesture()) {
        return;
      }
      goHome();
      return;
    }

    // Tap-first control-center entry: a tap on the status-bar band of the
    // top-level tab screens opens it, mirroring the top-edge swipe (which some
    // panels' etched glass makes unreliable). The reader keeps its clean page
    // (no status bar there to tap). Touch boards only, like the swipe itself.
    bool statusBarTap = false;
    if (mappedInput.hasTouch() && (currentActivity->name == "Home" || currentActivity->name == "FileBrowser" ||
                                   currentActivity->name == "Settings" || currentActivity->name == "SettingsList" ||
                                   currentActivity->name == "NetworkModeSelection")) {
      int tx = 0;
      int ty = 0;
      // The header back button shares this band; its taps stay Back.
      statusBarTap = mappedInput.wasScreenTapped(tx, ty) && ty < 44 && !HeaderBackTapTarget::contains(tx, ty);
    }
    const bool lightPanelRequested = statusBarTap || mappedInput.wasLightPanelGesture();
    if (lightPanelRequested && currentActivity->name == "FrontlightPanel") {
      popActivity();
      return;
    }
    // Only boards whose frontlight was detected get the panel.
    if (lightPanelRequested && Frontlight.present()) {
      haptic_feedback::touchAction();
      pushActivity(std::make_unique<FrontlightPanelActivity>(renderer, mappedInput));
      return;
    }

    // Note: do not hold a lock here, the loop() method must be responsible for acquire one if needed
    currentActivity->loop();
  }

  while (pendingAction != PendingAction::None) {
    // The page turner's radio gives its heap back before the next screen allocates. Its teardown
    // can span loop passes and runs outside the render lock.
    if (pendingAction != PendingAction::ReplaceForSleep &&
        !bleturner::beforeScreenChange(
            0, pendingActivity ? pendingActivity->isReaderActivity() : !pendingReaderPath.empty())) {
      return;
    }
    if (pendingAction == PendingAction::Pop) {
      RenderLock lock;

      if (!currentActivity) {
        // Should never happen in practice
        LOG_ERR("ACT", "Pop set but currentActivity is null; ignoring pop request");
        pendingAction = PendingAction::None;
        continue;
      }

      ++screenVisit;
      ActivityResult pendingResult = std::move(currentActivity->result);

      // Destroy the current activity
      exitActivity(lock);
      pendingAction = PendingAction::None;

      if (stackActivities.empty()) {
        LOG_DBG("ACT", "No more activities on stack, going home");
        lock.unlock();  // goHome may acquire its own lock
        goHome();
        continue;  // Will launch goHome immediately

      } else {
        currentActivity = std::move(stackActivities.back());
        stackActivities.pop_back();
        LOG_DBG("ACT", "Popped from activity stack, new size = %zu", stackActivities.size());
        // Handle result if necessary
        if (currentActivity->resultHandler) {
          LOG_DBG("ACT", "Handling result for popped activity");

          // Move it here to avoid the case where handler calling another startActivityForResult()
          auto handler = std::move(currentActivity->resultHandler);
          currentActivity->resultHandler = nullptr;
          lock.unlock();  // Handler may acquire its own lock
          handler(pendingResult);
        }

        // Request an update to ensure the popped activity gets re-rendered
        if (pendingAction == PendingAction::None) {
          requestUpdate();
        }

        // Handler may request another pending action, we will handle it in the next loop iteration
        continue;
      }

    } else if (pendingActivity || !pendingReaderPath.empty()) {
      // A pushed activity comes from the input loop. Do not stall that loop behind a long render
      // tail (for example SD-backed glyph prewarming); leave the request pending and retry it on
      // the next tick instead.
      if (pendingAction == PendingAction::Push) {
        RenderLock lock{RenderLock::Try{}};
        if (!lock.held()) break;

        stackActivities.push_back(std::move(currentActivity));
        // The parent's header back rect must not route taps on the pushed screen.
        HeaderBackTapTarget::clear();
        LOG_DBG("ACT", "Pushed to activity stack, new size = %zu", stackActivities.size());
        ++screenVisit;
        pendingAction = PendingAction::None;
        currentActivity = std::move(pendingActivity);

        lock.unlock();  // onEnter may acquire its own lock
        currentActivity->onEnter();
        continue;
      }

      RenderLock lock;
      if (pendingAction == PendingAction::Replace || pendingAction == PendingAction::ReplaceForSleep) {
        // Destroy the current activity
        exitActivity(lock);
        // Clear the stack
        while (!stackActivities.empty()) {
          stackActivities.back()->onExit();
          stackActivities.pop_back();
        }
        if (!pendingActivity) {
          pendingActivity =
              ReaderActivity::create(renderer, mappedInput, std::move(pendingReaderPath), pendingReaderFastRefresh);
          pendingReaderPath.clear();
          if (!pendingActivity) {
            LOG_ERR("ACT", "OOM: reader activity");
            pendingActivity = std::make_unique<HomeActivity>(renderer, mappedInput, HomeMenuItem::NONE, false);
          }
        }
      } else if (pendingAction == PendingAction::Push) {
        // Move current activity to stack
        stackActivities.push_back(std::move(currentActivity));
        // The parent's header back rect must not route taps on the pushed
        // screen (which may draw no header of its own).
        HeaderBackTapTarget::clear();
        LOG_DBG("ACT", "Pushed to activity stack, new size = %zu", stackActivities.size());
      }
      ++screenVisit;
      pendingAction = PendingAction::None;
      currentActivity = std::move(pendingActivity);

      lock.unlock();  // onEnter may acquire its own lock
      currentActivity->onEnter();

      // onEnter may request another pending action, we will handle it in the next loop iteration
      continue;
    }
  }

  if (requestedUpdate.exchange(false)) {
    // Using direct notification to signal the render task to update
    // Increment counter so multiple rapid calls won't be lost
    if (renderTaskHandle) {
      xTaskNotify(renderTaskHandle, 1, eIncrement);
    }
  }
}

void ActivityManager::exitActivity(const RenderLock& lock) {
  // Note: lock must be held by the caller
  if (currentActivity) {
    currentActivity->onExit();
    currentActivity.reset();
  }
  // The outgoing screen's header back button must not eat taps on the next
  // screen; the next header draw re-records it.
  HeaderBackTapTarget::clear();
}

void ActivityManager::replaceActivity(std::unique_ptr<Activity>&& newActivity) {
  mappedInput.resetHomeButtonInput();
  pendingReaderPath.clear();
  // Note: no lock here, this is usually called by loop() and we may run into deadlock
  if (currentActivity) {
    // Defer launch if we're currently in an activity, to avoid deleting the current activity
    // leading to the "delete this" problem
    pendingActivity = std::move(newActivity);
    pendingAction = PendingAction::Replace;
  } else {
    // No current activity, safe to launch immediately
    currentActivity = std::move(newActivity);
    currentActivity->onEnter();
  }
}

void ActivityManager::goToFileTransfer() {
  auto activity = makeUniqueNoThrow<CrossPointWebServerActivity>(renderer, mappedInput);
  if (!activity) {
    LOG_ERR("ACT", "Not enough memory to open File Transfer");
    return;
  }
  replaceActivity(std::move(activity));
}

void ActivityManager::goToReadingStats() {
  replaceActivity(std::make_unique<ReadingStatsActivity>(renderer, mappedInput));
}

void ActivityManager::goToJoinNetwork() {
  // Post heap-defrag reboot: enter the web-server activity straight in Join
  // Network mode (skips mode selection, does not reboot again).
  auto activity = makeUniqueNoThrow<CrossPointWebServerActivity>(renderer, mappedInput, /*startInJoinNetwork=*/true);
  if (!activity) {
    LOG_ERR("ACT", "OOM: file transfer activity");
    return;
  }
  replaceActivity(std::move(activity));
}

void ActivityManager::goToUsbDrive() {
#if FREEINK_CAP_USB_MSC
  auto activity = makeUniqueNoThrow<UsbDriveActivity>(renderer, mappedInput);
  if (!activity) {
    LOG_ERR("ACT", "OOM: USB Drive activity");
    return;
  }
  replaceActivity(std::move(activity));
#else
  LOG_ERR("ACT", "USB Drive requested in a build without USB Drive capability");
#endif
}

void ActivityManager::goToSettings(const SettingsSection openSection) {
  replaceActivity(std::make_unique<SettingsListActivity>(renderer, mappedInput, openSection));
}

void ActivityManager::goToFileBrowser(std::string path) {
  replaceActivity(std::make_unique<FileBrowserActivity>(renderer, mappedInput, std::move(path)));
}

void ActivityManager::goToLibrary(const int initialTab) {
  // Two screens behind one entry (SETTINGS.libraryView): this fork's cover grid, or upstream's
  // indexed list. Built through separate makeUniqueNoThrow calls rather than one unique_ptr<Activity>
  // so each keeps its own concrete type at the allocation, and a null is reported before
  // replaceActivity() sees it.
  if (SETTINGS.libraryView == CrossPointSettings::LIBRARY_VIEW_LIST) {
    (void)initialTab;  // the indexed list has its own tab set
    auto activity = makeUniqueNoThrow<LibraryListActivity>(renderer, mappedInput);
    if (!activity) {
      LOG_ERR("ACT", "OOM: library list activity");
      return;
    }
    replaceActivity(std::move(activity));
    return;
  }
  auto activity = makeUniqueNoThrow<CoverLibraryActivity>(renderer, mappedInput, initialTab);
  if (!activity) {
    LOG_ERR("ACT", "OOM: cover library activity");
    return;
  }
  replaceActivity(std::move(activity));
}

void ActivityManager::goToBrowser() {
  const auto& servers = OPDS_STORE.getServers();
  // Skip the server picker when there's only one server configured
  if (servers.size() == 1) {
    replaceActivity(std::make_unique<OpdsBookBrowserActivity>(renderer, mappedInput, servers[0]));
  } else {
    replaceActivity(std::make_unique<OpdsServerListActivity>(renderer, mappedInput, true));
  }
}

void ActivityManager::goToOpdsServers() {
  auto activity = makeUniqueNoThrow<OpdsServerListActivity>(renderer, mappedInput, true);
  if (!activity) {
    LOG_ERR("ACT", "OOM: OPDS server list activity");
    return;
  }
  replaceActivity(std::move(activity));
}

void ActivityManager::goToPlugins(bool showOpds) {
  auto activity = makeUniqueNoThrow<PluginCatalogActivity>(renderer, mappedInput, showOpds, /*rootMode=*/true);
  if (!activity) {
    LOG_ERR("ACT", "OOM: plugin catalog activity");
    return;
  }
  replaceActivity(std::move(activity));
}

void ActivityManager::goToReader(std::string path, const bool allowFastInitialRefresh) {
  if (path.empty()) {
    goToFileBrowser("/");
    return;
  }

  if (currentActivity) {
    // Built in loop() after the current screen is gone; see pendingReaderPath.
    mappedInput.resetHomeButtonInput();
    pendingActivity.reset();
    pendingReaderPath = std::move(path);
    pendingReaderFastRefresh = allowFastInitialRefresh;
    pendingAction = PendingAction::Replace;
    return;
  }
  auto activity = ReaderActivity::create(renderer, mappedInput, std::move(path), allowFastInitialRefresh);
  if (!activity) {
    LOG_ERR("ACT", "OOM: reader activity");
    return;
  }
  replaceActivity(std::move(activity));
}

void ActivityManager::goToSleep(bool fromTimeout) {
  replaceActivity(std::make_unique<SleepActivity>(renderer, mappedInput, fromTimeout));
  if (pendingAction == PendingAction::Replace) pendingAction = PendingAction::ReplaceForSleep;
  loop();  // Important: sleep screen must be rendered immediately, the caller will go to sleep right after this returns
}

void ActivityManager::goToBoot() { replaceActivity(std::make_unique<BootActivity>(renderer, mappedInput)); }

void ActivityManager::goToFullScreenMessage(std::string message, EpdFontFamily::Style style) {
  replaceActivity(std::make_unique<FullScreenMessageActivity>(renderer, mappedInput, std::move(message), style));
}

void ActivityManager::goHome(HomeMenuItem initialMenuItem, bool cleanInitialRefresh) {
  // Home's first paint is FAST, which cannot clear what a reader leaves behind: dense text and
  // grayscale image planes deposit charge (see GfxRenderer::panelResidue_), and it showed as the
  // previous page ghosting through Home's cover tile. Scrub on the way out of a reader -- and
  // ONLY then. The light list screens (FileBrowser, Settings, ReadingStats) leave nothing to
  // clear, and scrubbing on every return made the most-visited screen the slowest one: 1690ms
  // plus repaints, against 502ms everywhere else.
  const bool leftReaderFrame = currentActivity && currentActivity->isReaderActivity();

  if (initialMenuItem == HomeMenuItem::NONE && currentActivity) {
    const auto& activityName = currentActivity->name;
    if (activityName == "FileBrowser") {
      initialMenuItem = HomeMenuItem::FILE_BROWSER;
    } else if (activityName == "Library") {
      initialMenuItem = HomeMenuItem::LIBRARY;
    } else if (activityName == "OpdsBookBrowser" || activityName == "PluginCatalog") {
      initialMenuItem = HomeMenuItem::OPDS_BROWSER;
    } else if (activityName == "CrossPointWebServer") {
      initialMenuItem = HomeMenuItem::FILE_TRANSFER;
    } else if (activityName == "Settings" || activityName == "SettingsList") {
      initialMenuItem = HomeMenuItem::SETTINGS_MENU;
    }
  }
  replaceActivity(
      std::make_unique<HomeActivity>(renderer, mappedInput, initialMenuItem, cleanInitialRefresh || leftReaderFrame));
}
void ActivityManager::goToCrashReport() { replaceActivity(std::make_unique<CrashActivity>(renderer, mappedInput)); }

void ActivityManager::pushActivity(std::unique_ptr<Activity>&& activity) {
  mappedInput.resetHomeButtonInput();
  pendingReaderPath.clear();
  if (pendingActivity) {
    // Should never happen in practice
    LOG_ERR("ACT", "pendingActivity while pushActivity is not expected");
    pendingActivity.reset();
  }
  pendingActivity = std::move(activity);
  pendingAction = PendingAction::Push;
}

void ActivityManager::popActivity() {
  mappedInput.resetHomeButtonInput();
  pendingReaderPath.clear();
  if (pendingActivity) {
    // Should never happen in practice
    LOG_ERR("ACT", "pendingActivity while popActivity is not expected");
    pendingActivity.reset();
  }
  pendingAction = PendingAction::Pop;
}

bool ActivityManager::preventAutoSleep() const { return currentActivity && currentActivity->preventAutoSleep(); }

bool ActivityManager::requiresExclusiveStorageLoop() const {
  return currentActivity && currentActivity->requiresExclusiveStorageLoop();
}

bool ActivityManager::isReaderActivity() const {
  return std::any_of(stackActivities.begin(), stackActivities.end(),
                     [](const auto& activity) { return activity->isReaderActivity(); }) ||
         (currentActivity && currentActivity->isReaderActivity());
}

bool ActivityManager::isForegroundReader() const {
  return pendingAction == PendingAction::None && currentActivity && currentActivity->isReaderActivity();
}

ReaderActivity* ActivityManager::foregroundReader() const {
  return pendingAction == PendingAction::None && currentActivity ? currentActivity->asReaderActivity() : nullptr;
}

bool ActivityManager::isRemoteReaderContext() const {
  return pendingAction == PendingAction::None && currentActivity && currentActivity->acceptsRemoteInput();
}

bool ActivityManager::remoteClick(const RemoteButton button) {
  if (!isRemoteReaderContext()) return false;
  if (RenderLock::peek() || requestedUpdate.load()) {
    // Kept for when the drawing ends, like a page turn: the latest click wins.
    queuedRemoteClick = button;
    noteQueuedRemoteInput();
    return true;
  }
  return mappedInput.injectRemoteClick(button);
}

bool ActivityManager::isForegroundHomeShown() const {
  return pendingAction == PendingAction::None && currentActivity && currentActivity->name == "Home" &&
         paintedVisit.load(std::memory_order_acquire) == screenVisit;
}

bool ActivityManager::remoteShortcut(const bool readerMenu) {
  auto* reader = foregroundReader();
  if (!reader) return false;
  const bool shown = reader->pageRendered.load(std::memory_order_acquire);
  if (!remoteTurnAccepted(shown, RenderLock::peek(), reader->inputOverPage())) {
    if (!shown || reader->inputOverPage()) return false;
    queuedRemoteShortcut = readerMenu ? QueuedShortcut::ReaderMenu : QueuedShortcut::SaveQuote;
    noteQueuedRemoteInput();
    return true;
  }
  return reader->remoteShortcut(readerMenu);
}

namespace {
constexpr int MAX_QUEUED_REMOTE_TURNS = 3;
constexpr unsigned long QUEUED_REMOTE_TURN_MAX_AGE_MS = 4000;
}  // namespace

bool ActivityManager::remoteTurn(const bool forward, const bool chapter) {
  if (!isRemoteReaderContext() || mappedInput.hasRemoteClick()) return false;
  auto* reader = foregroundReader();
  if (!reader || reader->inputOverPage()) {
    return !chapter && remoteClick(forward ? RemoteButton::Next : RemoteButton::Previous);
  }
  const bool shown = reader->pageRendered.load(std::memory_order_acquire);
  if (!remoteTurnAccepted(shown, RenderLock::peek(), reader->inputOverPage())) {
    // A press during a page draw or a background slice is kept, not lost: a remote has no held
    // state to read back later, so a dropped press is simply gone.
    if (!shown || chapter) return false;
    if (queuedRemoteTurnVisit != screenVisit) queuedRemoteTurns = 0;
    queuedRemoteTurns = static_cast<int8_t>(
        std::clamp(queuedRemoteTurns + (forward ? 1 : -1), -MAX_QUEUED_REMOTE_TURNS, MAX_QUEUED_REMOTE_TURNS));
    noteQueuedRemoteInput();
    return true;
  }
  const bool turned = chapter ? reader->skipPages(forward ? 1 : -1) : reader->pageTurn(forward);
  if (turned) requestUpdate();
  return turned;
}

void ActivityManager::noteQueuedRemoteInput() {
  if (queuedRemoteTurnVisit != screenVisit) {
    queuedRemoteTurns = 0;
    queuedRemoteClick = RemoteButton::None;
    queuedRemoteShortcut = QueuedShortcut::None;
  }
  queuedRemoteTurnVisit = screenVisit;
  queuedRemoteTurnAtMs = millis();
  if (auto* reader = foregroundReader()) reader->interruptBackgroundWork();
}

bool ActivityManager::applyQueuedRemoteInput() {
  if (queuedRemoteTurns == 0 && queuedRemoteClick == RemoteButton::None && queuedRemoteShortcut == QueuedShortcut::None)
    return false;
  // Another screen, or a wait so long the reader would no longer expect it.
  if (queuedRemoteTurnVisit != screenVisit || millis() - queuedRemoteTurnAtMs > QUEUED_REMOTE_TURN_MAX_AGE_MS) {
    queuedRemoteTurns = 0;
    queuedRemoteClick = RemoteButton::None;
    queuedRemoteShortcut = QueuedShortcut::None;
    return false;
  }
  if (!isRemoteReaderContext() || RenderLock::peek() || requestedUpdate.load()) return false;
  if (queuedRemoteClick != RemoteButton::None) {
    // Refused while a device key is in this frame; tried again next loop.
    if (!mappedInput.injectRemoteClick(queuedRemoteClick)) return false;
    queuedRemoteClick = RemoteButton::None;
    return true;
  }
  auto* reader = foregroundReader();
  if (!reader || reader->inputOverPage()) {
    queuedRemoteTurns = 0;
    queuedRemoteShortcut = QueuedShortcut::None;
    return false;
  }
  if (queuedRemoteShortcut != QueuedShortcut::None) {
    const bool readerMenu = queuedRemoteShortcut == QueuedShortcut::ReaderMenu;
    queuedRemoteShortcut = QueuedShortcut::None;
    queuedRemoteTurns = 0;  // the shortcut opens something over the page
    return reader->remoteShortcut(readerMenu);
  }
  const bool forward = queuedRemoteTurns > 0;
  queuedRemoteTurns = static_cast<int8_t>(queuedRemoteTurns + (forward ? -1 : 1));
  queuedRemoteTurnAtMs = millis();
  const bool turned = reader->pageTurn(forward);
  if (turned) requestUpdate();
  return turned;
}

bool ActivityManager::handleForcedRefresh() { return currentActivity && currentActivity->handleForcedRefresh(); }

bool ActivityManager::skipLoopDelay() const { return currentActivity && currentActivity->skipLoopDelay(); }

ScreenshotInfo ActivityManager::getScreenshotInfo() const {
  if (currentActivity) {
    return currentActivity->getScreenshotInfo();
  }
  return {};
}

void ActivityManager::prepareForSleep() {
  RenderLock lock;
  for (const auto& activity : stackActivities) activity->prepareForSleep();
  if (currentActivity) currentActivity->prepareForSleep();
}

void ActivityManager::requestUpdate(bool immediate) {
  updateRequests.fetch_add(1, std::memory_order_relaxed);
  if (immediate) {
    if (renderTaskHandle) {
      xTaskNotify(renderTaskHandle, 1, eIncrement);
    }
  } else {
    // Deferring the update until current loop is finished
    // This is to avoid multiple updates being requested in the same loop
    requestedUpdate = true;
  }
}
void ActivityManager::requestUpdateAndWait() {
  if (!renderTaskHandle) {
    return;
  }
  updateRequests.fetch_add(1, std::memory_order_relaxed);

  // Atomic section to perform checks
  taskENTER_CRITICAL(&activityManagerSpinlock);
  auto currTaskHandler = xTaskGetCurrentTaskHandle();
  auto mutexHolder = xSemaphoreGetMutexHolder(renderingMutex);
  bool isRenderTask = (currTaskHandler == renderTaskHandle);
  bool alreadyWaiting = (waitingTaskHandle != nullptr);
  bool holdingRenderLock = (mutexHolder == currTaskHandler);
  if (!alreadyWaiting && !isRenderTask && !holdingRenderLock) {
    waitingTaskHandle = currTaskHandler;
  }
  taskEXIT_CRITICAL(&activityManagerSpinlock);

  // Render task cannot call requestUpdateAndWait() or it will cause a deadlock
  assert(!isRenderTask && "Render task cannot call requestUpdateAndWait()");

  // There should never be the case where 2 tasks are waiting for a render at the same time
  assert(!alreadyWaiting && "Already waiting for a render to complete");

  // Cannot call while holding RenderLock or it will cause a deadlock
  assert(!holdingRenderLock && "Cannot call requestUpdateAndWait() while holding RenderLock");

  xTaskNotify(renderTaskHandle, 1, eIncrement);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

// RenderLock

// renderingMutex is a plain (non-recursive) mutex, so a task that already holds it and
// constructs another RenderLock would block on ITSELF, forever. That is never what a nested
// acquire wants -- the state the inner lock is protecting is already protected by the outer one
// -- and the paths that nest are not locally visible: openWordLookupPanel() has to hold the lock
// across the panel's construction (the section's page slot must not be re-faulted while the scan
// copies out of it), and the constructor's reclaimFontHeap() takes the lock in turn. So a nested
// acquire is a no-op that owns nothing; the OUTERMOST RenderLock still does the single release.
bool RenderLock::heldByCurrentTask() {
  return xSemaphoreGetMutexHolder(activityManager.renderingMutex) == xTaskGetCurrentTaskHandle();
}

RenderLock::RenderLock() {
  if (heldByCurrentTask()) {
    nested = true;
    return;
  }
  xSemaphoreTake(activityManager.renderingMutex, portMAX_DELAY);
  isLocked = true;
}

RenderLock::RenderLock([[maybe_unused]] Activity&) {
  if (heldByCurrentTask()) {
    nested = true;
    return;
  }
  xSemaphoreTake(activityManager.renderingMutex, portMAX_DELAY);
  isLocked = true;
}

// held() is true when this task is already the holder: the caller's guarantee (nobody else is
// rendering) holds, and a zero-timeout take by the holder would otherwise fail spuriously.
RenderLock::RenderLock(Try) {
  if (heldByCurrentTask()) {
    nested = true;
    return;
  }
  isLocked = (xSemaphoreTake(activityManager.renderingMutex, 0) == pdTRUE);
}

RenderLock::RenderLock(const TryFor wait) {
  if (heldByCurrentTask()) {
    nested = true;
    return;
  }
  isLocked = (xSemaphoreTake(activityManager.renderingMutex, pdMS_TO_TICKS(wait.ms)) == pdTRUE);
}

RenderLock::~RenderLock() { unlock(); }

// A nested lock owns nothing, so it only drops its claim -- releasing here would hand the mutex
// away while the outer scope still relies on it.
void RenderLock::unlock() {
  if (isLocked) {
    xSemaphoreGive(activityManager.renderingMutex);
    isLocked = false;
  }
  nested = false;
}

/**
 *
 * Checks if renderingMutex is busy.
 *
 * @return true if renderingMutex is busy, otherwise false.
 *
 */
bool RenderLock::peek() { return xQueuePeek(activityManager.renderingMutex, NULL, 0) != pdTRUE; };
