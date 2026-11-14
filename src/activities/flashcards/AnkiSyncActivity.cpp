#include "AnkiSyncActivity.h"

#include <AnkiSyncStore.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>
#include <sys/time.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/settings/ClockSyncActivity.h"
#include "activities/settings/SettingsSection.h"
#include "components/HomeTabBar.h"
#include "components/UITheme.h"
#include "flashcards/AnkiClient.h"
#include "flashcards/AnkiSync.h"
#include "fontIds.h"
#include "network/WifiPowerSaveGuard.h"

namespace {
const char* errorText(const AnkiClient::Error& error) {
  using Kind = AnkiClient::Error::Kind;
  switch (error.kind) {
    case Kind::NoMemory:
      return tr(STR_LOW_MEMORY_RETRY);
    case Kind::Network:
    case Kind::Http:
      return tr(STR_ANKI_UNREACHABLE);
    case Kind::Unauthorized:
      return tr(STR_ANKI_KEY_REFUSED);
    case Kind::Forbidden:
      return tr(STR_ANKI_NOT_ALLOWED);
    case Kind::DeckMissing:
      return tr(STR_ANKI_DECK_MISSING);
    case Kind::Cancelled:
      return tr(STR_ANKI_SYNC_CANCELLED);
    default:
      return tr(STR_ANKI_REPLY_ERROR);
  }
}
}  // namespace

AnkiSyncActivity::AnkiSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const Mode mode)
    : Activity("AnkiSync", renderer, mappedInput), mode(mode) {}

void AnkiSyncActivity::setStatus(const State next, std::string text) {
  {
    RenderLock lock(*this);
    state = next;
    message = std::move(text);
  }
  requestUpdate(true);
}

void AnkiSyncActivity::onEnter() {
  Activity::onEnter();
  if (!ANKI_STORE.isConfigured()) {
    state = State::Failed;
    message = tr(STR_ANKI_NO_ADDRESS);
    requestUpdate();
    return;
  }
  wifiUsed = true;
  // Wi-Fi and the reply buffer need the heap the resident UI fonts hold; screens load them again.
  {
    RenderLock lock(*this);
    sdFontSystem.releaseAllResidentFonts(renderer);
  }
  if (WiFi.status() == WL_CONNECTED) {
    onWifi(true);
    return;
  }
  auto wifi = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput);
  if (!wifi) {
    LOG_ERR("ANKI", "OOM: WifiSelectionActivity");
    state = State::Failed;
    message = tr(STR_LOW_MEMORY_RETRY);
    requestUpdate();
    return;
  }
  // The callback only records the outcome: loop() runs the sync, after this screen is drawn.
  startActivityForResult(std::move(wifi), [this](const ActivityResult& result) { onWifi(!result.isCancelled); });
}

void AnkiSyncActivity::onExit() {
  Activity::onExit();
  if (wifiUsed) {
    WiFi.disconnect(false);
    WiFi.mode(WIFI_OFF);
    // Wi-Fi and the replies leave the heap too fragmented for the gray pass and the fonts: a
    // restart clears it and comes back to where the sync started.
    delay(30);
    if (mode == Mode::Sync) {
      silentRestartToHomeTab(static_cast<int>(HomeTab::Flashcards));
    } else {
      silentRestartToSettings(static_cast<int>(SettingsSection::Connectivity));
    }
  }
}

void AnkiSyncActivity::onWifi(const bool connected) {
  if (!connected) {
    state = State::Cancelled;
    return;
  }
  // Modem sleep stalls requests for seconds at a time; the sync is short.
  WiFi.setSleep(false);
  setStatus(State::Working, tr(STR_ANKI_SYNC_NOW));
}

// Reads the buttons every 10 ms, as the main loop would: the debounce needs reads that close.
void AnkiSyncActivity::watchBack(void* arg) {
  auto* self = static_cast<AnkiSyncActivity*>(arg);
  using Button = MappedInputManager::Button;
  while (!self->watchStop) {
    gpio.update();
    if (self->mappedInput.wasPressed(Button::Back) || self->mappedInput.wasReleased(Button::Back)) {
      self->backSeen = true;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  self->watchRunning = false;
  vTaskDelete(nullptr);
}

void AnkiSyncActivity::pollCancel(void* ctx) {
  auto* self = static_cast<AnkiSyncActivity*>(ctx);
  if (self->stopping || !self->backSeen) return;
  self->stopping = true;
  {
    RenderLock lock(*self);
    self->message = tr(STR_ANKI_SYNC_STOPPING);
  }
  self->requestUpdate(true);
}

void AnkiSyncActivity::run() {
  WifiPowerSaveGuard psGuard;
  AnkiClient client;
  if (!client.begin(ANKI_STORE.endpoint(), ANKI_STORE.getKey())) {
    setStatus(State::Failed, errorText(client.error()));
    return;
  }
  if (mode == Mode::Test) {
    std::string version;
    if (ankisync::testConnection(client, version)) {
      char text[96];
      snprintf(text, sizeof(text), tr(STR_ANKI_CONNECTED), version.c_str());
      setStatus(State::Done, text);
    } else {
      setStatus(State::Failed, errorText(client.error()));
    }
    client.end();
    return;
  }
  ankisync::ProgressFn progress;
  progress.ctx = this;
  progress.fn = [](void* ctx, const ankisync::Progress& p) {
    auto* self = static_cast<AnkiSyncActivity*>(ctx);
    char text[96];
    snprintf(text, sizeof(text), tr(STR_ANKI_SYNCING), p.deck.c_str());
    {
      RenderLock lock(*self);
      self->message = text;
      self->detail = std::to_string(p.done) + " / " + std::to_string(p.total);
    }
    self->requestUpdate(true);
  };
  // The sync holds the main loop: Back (a button, or the header's back on touch) is read between
  // cards and while replies arrive, and remembered once seen.
  progress.cancel = [](void* ctx) {
    pollCancel(ctx);
    return static_cast<AnkiSyncActivity*>(ctx)->stopping;
  };
  client.setPoll(pollCancel, this);

  // Reviews are identified by their time in Anki: the clock is set first.
  int64_t nowMs = 0;
  if (ClockSyncActivity::syncAndRecord()) {
    timeval tv;
    gettimeofday(&tv, nullptr);
    nowMs = static_cast<int64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
  } else {
    LOG_ERR("ANKI", "Clock not set; review times kept as they are");
  }
  watchStop = false;
  watchRunning = xTaskCreate(&watchBack, "AnkiBack", 2048, this, 1, nullptr) == pdPASS;
  if (!watchRunning) LOG_ERR("ANKI", "No Back watcher: the sync cannot be cancelled");
  const ankisync::Result result = ankisync::syncAll(client, progress, nowMs);
  watchStop = true;
  while (watchRunning) delay(5);
  // Back still held from the cancel: its release must not close the result straight away.
  gpio.update();
  holdingBack = stopping && mappedInput.isPressed(MappedInputManager::Button::Back);
  client.end();
  detail.clear();
  if (result.ok) {
    char text[128];
    snprintf(text, sizeof(text), tr(STR_ANKI_SYNC_DONE), static_cast<int>(result.cardsAdded),
             static_cast<int>(result.reviewsPulled), static_cast<int>(result.reviewsPushed));
    setStatus(State::Done, text);
  } else {
    LOG_ERR("ANKI", "Sync stopped at %s", result.failedDeck.c_str());
    detail = result.failedDeck;
    setStatus(State::Failed, errorText(result.error));
  }
}

void AnkiSyncActivity::loop() {
  if (state == State::Cancelled) {
    finish();
    return;
  }
  if (state == State::Working && !started) {
    started = true;
    requestUpdateAndWait();
    run();
    return;
  }
  if (state == State::Done || state == State::Failed) {
    if (holdingBack) {
      if (!mappedInput.isPressed(MappedInputManager::Button::Back)) holdingBack = false;
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      finish();
    }
  }
}

void AnkiSyncActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  GUI.drawHeader(renderer, UITheme::getInstance().getHeaderRect(renderer, true, false),
                 mode == Mode::Test ? tr(STR_ANKI_TEST) : tr(STR_ANKI_SYNC_NOW));
  const int top = screen.y + screen.height / 2 - 40;
  if (state == State::Failed && mode == Mode::Sync) {
    UITheme::drawCenteredText(renderer, screen, UI_12_FONT_ID, top - 40, tr(STR_ANKI_SYNC_FAILED), true,
                              EpdFontFamily::BOLD);
  }
  UITheme::drawCenteredWrappedText(renderer, Rect{screen.x + 30, top, screen.width - 60, 120}, UI_10_FONT_ID,
                                   message.c_str(), 4);
  if (!detail.empty()) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top + 110, detail.c_str(), true);
  }
  if (state == State::Done || state == State::Failed) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DONE), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == State::Working && mode == Mode::Sync && !stopping) {
    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer();
}
