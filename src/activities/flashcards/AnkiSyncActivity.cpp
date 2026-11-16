#include "AnkiSyncActivity.h"

#include <AnkiSyncStore.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <SdSystemDir.h>
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
// Between a service's name and its test result in the message.
constexpr char RESULT_SEPARATOR = '\x1F';

// Adds "Anki: 1 new card, 2 reviews received, 0 sent". Not inlined, so its buffers are not part
// of the caller's stack frame during the sync itself.
__attribute__((noinline)) void appendSummary(std::string& summary, const char* name, const ankisync::Result& result) {
  char added[48], pulled[64], text[128];  // room for longer translations than "1234 new cards"
  I18N.formatCount(added, sizeof(added), StrId::STR_SYNC_NEW_CARDS, static_cast<int>(result.cardsAdded));
  I18N.formatCount(pulled, sizeof(pulled), StrId::STR_SYNC_REVIEWS_RECEIVED, static_cast<int>(result.reviewsPulled));
  snprintf(text, sizeof(text), I18n::getInstance().get(StrId::STR_SYNC_SERVICE_DONE), name, added, pulled,
           static_cast<int>(result.reviewsPushed));
  if (!summary.empty()) summary += "; ";
  summary += text;
}

// The outcome carried across the restart between services: three lines.
std::string stashPath() { return sdsystem::path("flashcard_sync_pending.txt"); }

const char* errorText(const AnkiClient::Error& error, const ankisync::Service& service) {
  using Kind = AnkiClient::Error::Kind;
  const auto text = [](const StrId id) { return I18n::getInstance().get(id); };
  switch (error.kind) {
    case Kind::NoMemory:
      return tr(STR_LOW_MEMORY_RETRY);
    case Kind::Network:
    case Kind::Http:
      return text(service.unreachable);
    case Kind::Unauthorized:
      return text(service.keyRefused);
    case Kind::Forbidden:
      return text(service.notAllowed);
    case Kind::DeckMissing:
      return text(service.deckMissing);
    case Kind::Cancelled:
      return tr(STR_ANKI_SYNC_CANCELLED);
    default:
      // The server has no login for the service's cloud, or the cloud refused it.
      return text(error.type == "authenticationRequired" ? service.loginMissing : service.replyError);
  }
}
}  // namespace

const char* AnkiSyncActivity::syncLabel() {
  const bool anki = ANKI_STORE.isConfigured();
  const bool migaku = MIGAKU_STORE.isConfigured();
  if (anki && migaku) return tr(STR_SYNC_BOTH_NOW);
  if (anki) return tr(STR_ANKI_SYNC_NOW);
  if (migaku) return tr(STR_MIGAKU_SYNC_NOW);
  return nullptr;
}

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
  if (syncLabel() == nullptr) {
    state = State::Failed;
    const bool bothOff = ANKI_STORE.getServer() == SyncServiceStore::Server::Off &&
                         MIGAKU_STORE.getServer() == SyncServiceStore::Server::Off;
    message = bothOff ? tr(STR_SYNC_NO_SERVICE) : tr(STR_SYNC_NO_SERVER);
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
    Outcome outcome = carried;  // the first service's result still shows
    if (outcome.failure.empty()) outcome.failure = tr(STR_LOW_MEMORY_RETRY);
    showOutcome(outcome);
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
      if (nextServicePending) silentRestartToFlashcardSync();  // the next service, on a fresh heap
      silentRestartToHomeTab(static_cast<int>(HomeTab::Flashcards));
    } else {
      silentRestartToSettings(static_cast<int>(SettingsSection::Connectivity));
    }
  }
}

void AnkiSyncActivity::onWifi(const bool connected) {
  if (!connected && resumed) {
    // After the restart: the first service's result still shows, with why the rest did not sync.
    Outcome outcome = carried;
    if (outcome.failure.empty()) {
      outcome.failure = tr(STR_MIGAKU_NOT_SYNCED);
      outcome.failedAt = ankisync::migakuService().name;
    }
    // The Back that left the Wi-Fi screen must not close the result on its release.
    holdingBack = mappedInput.isPressed(MappedInputManager::Button::Back);
    showOutcome(outcome);
    return;
  }
  if (!connected) {
    state = State::Cancelled;
    return;
  }
  // Modem sleep stalls requests for seconds at a time; the sync is short.
  WiFi.setSleep(false);
  setStatus(State::Working, mode == Mode::Sync ? syncLabel() : tr(STR_ANKI_TEST));
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

// One block per service that is on: its name, then the result. Blocks are separated by '\n', and
// the name from the result by RESULT_SEPARATOR.
void AnkiSyncActivity::test() {
  WifiPowerSaveGuard psGuard;
  std::string text;
  bool failed = false;
  for (const ankisync::Service& service : {ankisync::ankiService(), ankisync::migakuService()}) {
    if (!service.store.isConfigured()) continue;
    AnkiClient client;
    std::string version;
    char line[160];
    if (client.begin(service.store.endpoint(), service.store.serverKey()) &&
        ankisync::testConnection(client, version)) {
      snprintf(line, sizeof(line), I18n::getInstance().get(service.connected), version.c_str());
    } else {
      snprintf(line, sizeof(line), "%s", errorText(client.error(), service));
      failed = true;
    }
    client.end();
    if (!text.empty()) text += '\n';
    text += service.name;
    text += RESULT_SEPARATOR;
    text += line;
  }
  setStatus(failed ? State::Failed : State::Done, text);
}

void AnkiSyncActivity::run() {
  if (mode == Mode::Test) {
    test();
    return;
  }
  WifiPowerSaveGuard psGuard;
  AnkiClient client;
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
  // Each service set up, one after the other: one that fails does not keep the next from syncing.
  // The client's buffer is given back between them, so the next takes the largest block again.
  const bool both = ANKI_STORE.isConfigured() && MIGAKU_STORE.isConfigured();
  Outcome outcome = carried;
  bool first = true;
  bool skipped = false;  // a service left out because the sync was cancelled
  for (const ankisync::Service& service : {ankisync::ankiService(), ankisync::migakuService()}) {
    pollCancel(this);  // a Back seen after the last request of the service before
    if (!service.store.isConfigured()) continue;
    if (stopping) {
      skipped = true;
      continue;
    }
    if (resumed && &service.store == &ANKI_STORE) continue;  // synced before the restart
    // The second service on a fresh heap: after one Wi-Fi session the largest block is too small.
    if (!first && !resumed && silentRestartAvailable() && stashOutcome(outcome)) {
      nextServicePending = true;
      break;
    }
    first = false;
    const char* name = service.name;
    ankisync::Result result;
    if (client.begin(service.store.endpoint(), service.store.serverKey())) {
      client.setPoll(pollCancel, this);
      result = ankisync::syncAll(client, service, progress, nowMs);
      client.end();
    } else {
      result.error = client.error();
    }
    if (result.ok) {
      appendSummary(outcome.summary, name, result);
      if (result.remoteSkipped) {
        outcome.summary += ". ";
        outcome.summary += tr(STR_ANKIWEB_NOT_SYNCED);
      }
    } else {
      LOG_ERR("ANKI", "%s sync stopped at %s", name, result.failedDeck.c_str());
      if (outcome.failure.empty()) {
        outcome.failure = result.remoteBlocked ? tr(STR_ANKIWEB_NEEDS_ANKI) : errorText(result.error, service);
        outcome.failedAt =
            both ? std::string(name) + (result.failedDeck.empty() ? "" : ": " + result.failedDeck) : result.failedDeck;
      }
    }
  }
  watchStop = true;
  while (watchRunning) delay(5);
  // Back still held from the cancel: its release must not close the result straight away.
  gpio.update();
  holdingBack = stopping && mappedInput.isPressed(MappedInputManager::Button::Back);
  if (skipped && outcome.failure.empty()) outcome.failure = tr(STR_ANKI_SYNC_CANCELLED);
  if (nextServicePending) {
    LOG_INF("ANKI", "Restarting before the next service");
    finish();  // onExit restarts into it
    return;
  }
  showOutcome(outcome);
}

void AnkiSyncActivity::showOutcome(const Outcome& outcome) {
  if (outcome.failure.empty()) {
    detail.clear();
    setStatus(State::Done, outcome.summary);
  } else {
    detail = outcome.failedAt;
    setStatus(State::Failed, outcome.summary.empty() ? outcome.failure : outcome.summary + "; " + outcome.failure);
  }
}

bool AnkiSyncActivity::stashOutcome(const Outcome& outcome) {
  const std::string path = stashPath();
  HalFile stash;
  if (!Storage.openFileForWrite("ANKI", path.c_str(), stash)) {
    LOG_ERR("ANKI", "Sync stash not written; the next service syncs without a restart");
    return false;
  }
  const std::string text = outcome.summary + "\n" + outcome.failure + "\n" + outcome.failedAt + "\n";
  const size_t written = stash.write(reinterpret_cast<const uint8_t*>(text.data()), text.size());
  if (!stash.close() || written != text.size()) {
    Storage.remove(path.c_str());
    LOG_ERR("ANKI", "Sync stash cut short; the next service syncs without a restart");
    return false;
  }
  return true;
}

std::unique_ptr<AnkiSyncActivity> AnkiSyncActivity::resumeAfterRestart(GfxRenderer& renderer,
                                                                       MappedInputManager& mappedInput) {
  const std::string path = stashPath();
  std::string text;
  const bool read = Storage.readFileToString("ANKI", path, 1024, text);
  Storage.remove(path.c_str());  // read once: a later restart must not replay it
  if (!read) {
    LOG_ERR("ANKI", "No sync stash after the restart; the next service is not synced");
    return nullptr;
  }
  auto activity = makeUniqueNoThrow<AnkiSyncActivity>(renderer, mappedInput, Mode::Sync);
  if (!activity) {
    LOG_ERR("ANKI", "OOM: AnkiSyncActivity");
    return nullptr;
  }
  Outcome& carried = activity->carried;
  size_t at = 0;
  for (std::string* part : {&carried.summary, &carried.failure, &carried.failedAt}) {
    const size_t end = text.find('\n', at);
    if (end == std::string::npos) break;
    *part = text.substr(at, end - at);
    at = end + 1;
  }
  activity->resumed = true;
  return activity;
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
                 mode == Mode::Sync ? syncLabel() : tr(STR_ANKI_TEST));
  const int top = screen.y + screen.height / 2 - 40;
  if (state == State::Failed && mode == Mode::Sync) {
    UITheme::drawCenteredText(renderer, screen, UI_12_FONT_ID, top - 40, tr(STR_ANKI_SYNC_FAILED), true,
                              EpdFontFamily::BOLD);
  }
  if (message.find(RESULT_SEPARATOR) == std::string::npos) {
    UITheme::drawCenteredWrappedText(renderer, Rect{screen.x + 30, top, screen.width - 60, 120}, UI_10_FONT_ID,
                                     message.c_str(), 4);
  } else {
    // The test's blocks, one per service: the name in bold, the result wrapped below it.
    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const int block = 5 * lineHeight;
    int y = top - block / 2;
    size_t start = 0;
    while (start < message.size()) {
      size_t end = message.find('\n', start);
      if (end == std::string::npos) end = message.size();
      const size_t split = message.find(RESULT_SEPARATOR, start);
      const bool named = split != std::string::npos && split < end;
      const std::string name = named ? message.substr(start, split - start) : std::string();
      const std::string result = message.substr(named ? split + 1 : start, end - (named ? split + 1 : start));
      int textTop = y;
      if (named) {
        UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, y, name.c_str(), true, EpdFontFamily::BOLD);
        textTop += lineHeight;
      }
      UITheme::drawCenteredWrappedText(renderer, Rect{screen.x + 30, textTop, screen.width - 60, 4 * lineHeight},
                                       UI_10_FONT_ID, result.c_str(), 4);
      y += block + 10;
      start = end + 1;
    }
  }
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
