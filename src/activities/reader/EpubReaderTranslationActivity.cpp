#include "EpubReaderTranslationActivity.h"

#include <ArduinoJson.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <SdSystemDir.h>
#include <SecureHttpClient.h>
#include <WiFi.h>
#include <esp_crt_bundle.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "PanelTouch.h"
#include "SilentRestart.h"
#include "WifiCredentialStore.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/DictionaryPanel.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

constexpr int HTTP_BUF_SIZE = 2048;
// wolfSSL, not mbedTLS: this request used to go through esp_http_client (the project's only
// mbedTLS user), whose 16KB-per-direction record buffers are baked into the prebuilt framework
// and needed ~55KB contiguous -- measured on an X4, a handshake at 53236 bytes failed with
// ESP_ERR_HTTP_CONNECT after driving free heap to 356 bytes, so every translation from a
// reading session paid a silent restart. SecureHttpClient runs wolfSSL, which is built from
// source with our own user_settings.h (scripts/patch_wolfssl.py) and whose largest single
// allocation is the ~17KB record buffer. Floors adopted from KOReaderSyncClient, which ported
// the same way and measured handshakes succeeding inside a 43KB largest block.
constexpr uint32_t MIN_FREE_FOR_TLS = 35000;
constexpr uint32_t MIN_HEAP_FOR_TLS = 20000;
// What bringing up the WiFi/lwIP stack takes out of the LARGEST CONTIGUOUS block, not out of
// total free. Measured on device: 86004 at translation entry, 53236 left at the TLS gate --
// 32768 exactly. The margin on top covers association-time variation; too generous a value
// here costs a restart that wasn't needed, too tight costs the doubled WiFi+NTP this check
// exists to avoid. Used to decide BEFORE the connect whether one pass can work.
constexpr uint32_t WIFI_STACK_RESERVE = 36000;
// esp_wifi_init() (triggered by the first WiFi.mode(WIFI_STA) call) allocates its own TX/RX
// buffer pools, NVS state, and wpa_supplicant/RRM tables -- many small-to-medium allocations, not
// one big contiguous block, so this checks total free heap rather than getMaxAllocHeap(). Confirmed
// on a real device: entering Translation right after reading a memory-heavy CJK chapter (free heap
// down to ~37KB) crashed with a null-pointer fault inside wpa_supplicant's eloop_cancel_timeout --
// some internal allocation failed and the driver didn't null-check it before dereferencing. There's
// no public API to ask ESP-IDF's WiFi driver "do you have enough heap", so this margin is a
// conservative empirical floor above the crash point, not a documented ESP-IDF constant.
constexpr uint32_t MIN_HEAP_FOR_WIFI_INIT = 70000;
static std::string apiKeyPath() { return sdsystem::findUserFile("gemini.key"); }
constexpr const char* GEMINI_MODEL = "gemini-3.8-flash";
// The page behind the panel, kept across the full-screen Wi-Fi list and a silent restart.
static std::string backgroundPath() { return sdsystem::path("translate_bg.bin"); }
// A saved network either answers within this or is out of range; the Wi-Fi list takes over then.
constexpr unsigned long QUICK_CONNECT_TIMEOUT_MS = 12000;
// Translations are prose, and a Japanese page's is read beside Japanese: the UI face carries the
// CJK fallback.
constexpr int TRANSLATION_FONT_ID = UI_12_FONT_ID;

}  // namespace

EpubReaderTranslationActivity::EpubReaderTranslationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                             std::string sourceText, std::string preTranslatedText,
                                                             const bool resumedAfterRestart, const StrId title)
    : Activity("Translation", renderer, mappedInput),
      sourceText(std::move(sourceText)),
      resumedAfterRestart(resumedAfterRestart),
      title(title) {
  if (!preTranslatedText.empty()) {
    translatedText = std::move(preTranslatedText);
    hasPreTranslation = true;
    state = SHOWING_RESULT;
  }
}

bool EpubReaderTranslationActivity::stashAndRestart() {
  HalFile stash;
  if (!Storage.openFileForWrite("XLAT", translateStashPath().c_str(), stash)) {
    LOG_ERR("XLAT", "Could not write translation stash; showing low-memory error instead");
    return false;
  }
  const size_t written = stash.write(reinterpret_cast<const uint8_t*>(sourceText.data()), sourceText.size());
  stash.close();
  if (written != sourceText.size()) {
    LOG_ERR("XLAT", "Short write on translation stash (%u/%u); showing low-memory error instead",
            static_cast<unsigned>(written), static_cast<unsigned>(sourceText.size()));
    Storage.remove(translateStashPath().c_str());
    return false;
  }
  LOG_DBG("XLAT", "Stashed %u bytes; restarting for a fresh heap", static_cast<unsigned>(sourceText.size()));
  silentRestartToTranslation();  // does not return
  return true;
}

void EpubReaderTranslationActivity::onEnter() {
  Activity::onEnter();

  if (hasPreTranslation) {
    const auto body = DictionaryPanel::compute(renderer).body;
    textPages.layout(renderer, TRANSLATION_FONT_ID, translatedText, body.width, body.height);
    requestUpdate();
    return;
  }

  // A restarted translation boots to a blank framebuffer; the page was saved before the restart.
  if (resumedAfterRestart) {
    restoreBackgroundPending = true;
  } else {
    saveBackground(renderer);
  }

  // The reader activity underneath is only paused, not destroyed, so its font decompressor's
  // hot-group buffer (up to tens of KB, see FontDecompressor.cpp) is still resident and dead
  // weight here -- free it before WiFi init needs the headroom, same rationale as the identical
  // call before a chapter build in EpubReaderActivity.
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->releaseAllFontMemory();
  }

  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t maxAllocAtEntry = ESP.getMaxAllocHeap();
  LOG_DBG("XLAT", "Entering translation (free heap: %u, max alloc: %u)", static_cast<unsigned>(freeHeap),
          static_cast<unsigned>(maxAllocAtEntry));
  // Decide about the restart HERE, before WiFi. The old gate sat after the connect, so a heap
  // that couldn't carry TLS still paid for association and the NTP sync first (device log: 3.7s
  // + 1.2s), then restarted and paid for both a second time. Bringing WiFi up costs a
  // predictable slice of the largest block -- measured 100KB+ at entry down to 53KB at the TLS
  // gate -- so if that slice would leave us short, restarting now makes it a single payment.
  // Deliberately NOT "try TLS anyway and see": on the X3 the framebuffer is larger and the
  // margin thinner, and a handshake that runs out mid-way is a crash, not a clean error.
  const bool wontFitAfterWifi = maxAllocAtEntry < MIN_HEAP_FOR_TLS + WIFI_STACK_RESERVE;
  // With wolfSSL's much lower requirement this should now be false for any normal reading
  // session (measured entry: 86004, WiFi takes 32768, leaving 53236 against a 20000 floor) --
  // the restart becomes the exception it was meant to be rather than the rule.
  if (freeHeap < MIN_HEAP_FOR_WIFI_INIT || wontFitAfterWifi) {
    LOG_ERR("XLAT", "Heap too tight for a one-pass translation (free %u, maxAlloc %u, need %u)",
            static_cast<unsigned>(freeHeap), static_cast<unsigned>(maxAllocAtEntry),
            static_cast<unsigned>(MIN_HEAP_FOR_TLS + WIFI_STACK_RESERVE));
    // A long reading session (Word Lookup, chapter builds) can leave the heap too fragmented for
    // the WiFi/TLS stack even after everything reclaimable was freed -- but a silent restart
    // clears it completely (~110KB contiguous right after boot). Stash the text and retry once
    // on a fresh heap; only a post-restart failure is a real error worth showing.
    if (!resumedAfterRestart && stashAndRestart()) return;
    errorMessage = tr(STR_TRANSLATION_LOW_MEMORY);
    state = ERROR;
    requestUpdate();
    return;
  }

  WiFi.mode(WIFI_STA);
  if (!startQuickConnect()) startWifiSelection();
}

bool EpubReaderTranslationActivity::saveBackground(const GfxRenderer& renderer) {
  HalFile file;
  if (!Storage.openFileForWrite("XLAT", backgroundPath().c_str(), file)) return false;
  const size_t size = renderer.getBufferSize();
  if (file.write(renderer.getFrameBuffer(), size) != size) {
    LOG_ERR("XLAT", "Short write saving the page behind the panel");
    return false;
  }
  return true;
}

bool EpubReaderTranslationActivity::restoreBackground(const GfxRenderer& renderer) {
  HalFile file;
  if (!Storage.openFileForRead("XLAT", backgroundPath().c_str(), file)) return false;
  const size_t size = renderer.getBufferSize();
  if (file.size() != size || file.read(renderer.getFrameBuffer(), size) != static_cast<int>(size)) {
    // A page from another orientation or a truncated file: a blank backdrop beats a garbled one.
    renderer.clearScreen();
    return false;
  }
  return true;
}

bool EpubReaderTranslationActivity::startQuickConnect() {
  {
    RenderLock lock(*this);  // SD access shares the SPI bus with the display
    WIFI_STORE.loadFromFile();
  }
  const std::string lastSsid = WIFI_STORE.getLastConnectedSsid();
  if (lastSsid.empty()) return false;
  const auto cred = WIFI_STORE.findCredential(lastSsid);
  if (!cred) return false;
  if (cred->password.empty()) {
    WiFi.begin(cred->ssid.c_str());
  } else {
    WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
  }
  connectStartMs = millis();
  state = CONNECTING;
  requestUpdate();
  return true;
}

void EpubReaderTranslationActivity::pollQuickConnect() {
  const wl_status_t status = WiFi.status();
  if (status == WL_CONNECTED) {
    // Same clock rule as the Wi-Fi list: any connection is a chance to correct the time, which
    // TLS certificate checks and the reading stats both depend on.
    if (!SETTINGS.clockHasBeenSynced || !HalClock::systemTimeValid() || !halClock.isAvailable()) {
      if (halClock.syncFromNTP() && !SETTINGS.clockHasBeenSynced) {
        SETTINGS.clockHasBeenSynced = 1;
        SETTINGS.saveToFile();
      }
    }
    onWifiComplete(true);
    return;
  }
  if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL ||
      millis() - connectStartMs > QUICK_CONNECT_TIMEOUT_MS) {
    LOG_DBG("XLAT", "Saved network did not answer (status %d); opening the Wi-Fi list", static_cast<int>(status));
    WiFi.disconnect(false);
    startWifiSelection();
  }
}

void EpubReaderTranslationActivity::startWifiSelection() {
  state = WIFI_SELECTION;
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           // The list drew over the page behind the panel.
                           restoreBackgroundPending = true;
                           onWifiComplete(!result.isCancelled);
                         });
}

void EpubReaderTranslationActivity::onExit() {
  Activity::onExit();
  Storage.remove(backgroundPath().c_str());

  if (!hasPreTranslation && WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestartToReader();
  }
}

bool EpubReaderTranslationActivity::readApiKey(std::string& keyOut) {
  // 192, not 256: the stack budget for a local is under 256 bytes (CLAUDE.md), and this is
  // still more than three times the longest key Google issues (a current AI Studio key is ~53
  // characters, the older AIza form 39).
  char buf[192];
  size_t len = Storage.readFileToBuffer(apiKeyPath().c_str(), buf, sizeof(buf));
  if (len == 0) return false;
  // readFileToBuffer stops at bufferSize-1 and reports the truncated length, so a key longer
  // than the buffer arrives silently cut in half and is rejected by the API as malformed. A
  // full buffer now means "this is not a key" rather than "here is most of one".
  if (len >= sizeof(buf) - 1) {
    LOG_ERR("XLAT", "gemini.key is %u+ bytes; that is not an API key", static_cast<unsigned>(len));
    return false;
  }

  size_t start = 0;
  // A UTF-8 BOM is what an editor on Windows writes when the file is saved as UTF-8, and the
  // documented way to install a key is to paste it into a text file. The three bytes go out in
  // front of the key, the API rejects it, and nothing on screen suggests the file is at fault.
  if (len - start >= 3 && static_cast<unsigned char>(buf[start]) == 0xEF &&
      static_cast<unsigned char>(buf[start + 1]) == 0xBB && static_cast<unsigned char>(buf[start + 2]) == 0xBF) {
    start += 3;
  }
  // Trim whitespace/newlines from BOTH ends: a leading space or newline corrupts the key exactly
  // as invisibly as a trailing one, and only the trailing end was handled.
  while (start < len && (buf[start] == '\n' || buf[start] == '\r' || buf[start] == ' ' || buf[start] == '\t')) {
    start++;
  }
  while (len > start && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || buf[len - 1] == ' ' || buf[len - 1] == '\t')) {
    len--;
  }
  if (len <= start) return false;

  keyOut.assign(buf + start, len - start);
  return true;
}

// What the screen says when Google refuses the request. The status alone separates the causes a
// reader can actually act on -- a rejected key, an account with no billing set up, a quota that
// has run out -- from one another and from "the network is down", which is what a single generic
// failure message left every one of them looking like. The code is appended because it is the
// one detail that makes a bug report actionable without asking for logs.
static std::string translationHttpError(const int httpCode) {
  // tr() pastes its argument into StrId::, so each case names its string directly.
  const char* base = tr(STR_TRANSLATION_FAILED);
  switch (httpCode) {
    case 400:
      base = tr(STR_TRANSLATION_KEY_REJECTED);
      break;
    case 401:
    case 403:
      base = tr(STR_TRANSLATION_ACCESS_DENIED);
      break;
    case 429:
      base = tr(STR_TRANSLATION_QUOTA);
      break;
    default:
      if (httpCode >= 500) base = tr(STR_TRANSLATION_SERVICE_DOWN);
      break;
  }
  std::string msg = base;
  if (httpCode > 0) {
    char code[16];
    snprintf(code, sizeof(code), " (%d)", httpCode);
    msg += code;
  }
  return msg;
}

bool EpubReaderTranslationActivity::callGeminiApi(const std::string& apiKey) {
  std::string url = "https://generativelanguage.googleapis.com/v1beta/models/";
  url += GEMINI_MODEL;
  url += ":generateContent?key=";
  url += apiKey;
  // Only the translated text. The full reply also carries the model's thought signature and
  // usage counters -- 9 KB for a page whose translation is 600 bytes -- and the HTTP client
  // gathers the body in one growing string beside the TLS session: on the device that growth
  // ran out of contiguous heap and aborted.
  url += "&$fields=candidates.content.parts.text";

  // TLS/HTTP client init needs one large *contiguous* buffer (record buffers, X.509 parsing,
  // etc.), so the gate must check the largest allocatable block, not total free heap -- on a
  // fragmented heap (e.g. after CJK font/vertical-text work, which this session found leaves the
  // heap more fragmented than plain-text reading) total free can look comfortably above
  // MIN_HEAP_FOR_TLS while no single block that size actually exists, silently passing this check
  // only to fail deeper inside the TLS handshake instead of with this clear message.
  // Release the glyph caches AGAIN, right at the gate. onEnter() already did it, but the WiFi
  // selection screen and the return render in between re-warm them -- device log: 112KB free at
  // entry, 51KB largest block here, 4KB short of the threshold, so every translation paid a
  // silent restart (~10s: reboot, re-enter, reconnect WiFi) instead of just calling the API.
  // Nothing is drawn between here and the request, and the reader re-warms on its next page.
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->releaseAllFontMemory();
  }

  // Both floors, for the two different failure modes: the record buffer needs one contiguous
  // block, the handshake's session object and cert-verify temps need total room. A wrong guess
  // fails soft -- wolfSSL returns MEMORY_E rather than aborting under -fno-exceptions.
  const uint32_t maxAllocHeap = ESP.getMaxAllocHeap();
  const uint32_t freeHeapNow = ESP.getFreeHeap();
  LOG_DBG("XLAT", "Calling Gemini (free: %u, max alloc: %u)", static_cast<unsigned>(freeHeapNow),
          static_cast<unsigned>(maxAllocHeap));
  if (maxAllocHeap < MIN_HEAP_FOR_TLS || freeHeapNow < MIN_FREE_FOR_TLS) {
    LOG_ERR("XLAT", "Insufficient heap for TLS: free %u (need %u), largest block %u (need %u)",
            static_cast<unsigned>(freeHeapNow), static_cast<unsigned>(MIN_FREE_FOR_TLS),
            static_cast<unsigned>(maxAllocHeap), static_cast<unsigned>(MIN_HEAP_FOR_TLS));
    // Retry once on a pristine post-restart heap; see onEnter() for the rationale.
    if (!resumedAfterRestart && stashAndRestart()) return false;
    errorMessage = tr(STR_TRANSLATION_LOW_MEMORY);
    return false;
  }

  JsonDocument reqDoc;
  auto contents = reqDoc["contents"].to<JsonArray>();
  auto part = contents.add<JsonObject>();
  auto parts = part["parts"].to<JsonArray>();
  auto textPart = parts.add<JsonObject>();
  textPart["text"] = std::string(
                         "Translate the following text to English. "
                         "Return only the translation, no commentary.\n\n") +
                     sourceText;

  auto config = reqDoc["generationConfig"].to<JsonObject>();
  config["maxOutputTokens"] = 2048;
  // The model thinks before answering, and those tokens count against maxOutputTokens: at its
  // default level a long page could use up the budget and return no translation, or outlast the
  // timeout below. A translation needs little reasoning; "low" is the lowest level 3.8 accepts.
  config["thinkingConfig"].to<JsonObject>()["thinkingLevel"] = "low";

  std::string body;
  serializeJson(reqDoc, body);

  freeink::SecureHttpClient http;
  http.setInsecure();  // same as KOReaderSync: the wolfSSL transport has no CA bundle wired up
  http.setTimeout(30000);
  if (!http.begin(url)) {
    LOG_ERR("XLAT", "Failed to open connection");
    errorMessage = tr(STR_TRANSLATION_FAILED);
    return false;
  }
  http.addHeader("Content-Type", "application/json");

  const int httpCode = http.POST(body);
  const std::string response = http.getString();
  http.end();

  LOG_DBG("XLAT", "Gemini response: HTTP %d (%u bytes)", httpCode, static_cast<unsigned>(response.size()));

  if (httpCode != 200 || response.empty()) {
    LOG_ERR("XLAT", "API call failed: http=%d", httpCode);
    errorMessage = translationHttpError(httpCode);
    return false;
  }

  JsonDocument respDoc;
  DeserializationError jsonErr = deserializeJson(respDoc, response);
  if (jsonErr) {
    LOG_ERR("XLAT", "JSON parse error: %s", jsonErr.c_str());
    errorMessage = tr(STR_TRANSLATION_FAILED);
    return false;
  }

  const char* text = respDoc["candidates"][0]["content"]["parts"][0]["text"];
  if (!text) {
    LOG_ERR("XLAT", "No text in Gemini response");
    errorMessage = tr(STR_TRANSLATION_FAILED);
    return false;
  }

  translatedText = text;
  return true;
}

void EpubReaderTranslationActivity::onWifiComplete(bool success) {
  if (!success) {
    errorMessage = tr(STR_TRANSLATION_WIFI_FAILED);
    state = ERROR;
    requestUpdate();
    return;
  }

  std::string apiKey;
  if (!readApiKey(apiKey)) {
    errorMessage = tr(STR_TRANSLATION_NO_API_KEY);
    state = ERROR;
    requestUpdate();
    return;
  }

  {
    RenderLock lock(*this);
    state = TRANSLATING;
  }
  requestUpdateAndWait();

  if (callGeminiApi(apiKey)) {
    RenderLock lock(*this);
    const auto body = DictionaryPanel::compute(renderer).body;
    textPages.layout(renderer, TRANSLATION_FONT_ID, translatedText, body.width, body.height);
    currentPage = 0;
    state = SHOWING_RESULT;
  } else {
    RenderLock lock(*this);
    state = ERROR;
  }
  requestUpdate();
}

void EpubReaderTranslationActivity::cancel() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

void EpubReaderTranslationActivity::stepPage(const int direction) {
  if (state != SHOWING_RESULT) return;
  const int next = currentPage + direction;
  if (next < 0 || next >= textPages.pageCount()) return;
  currentPage = next;
  requestUpdate();
}

void EpubReaderTranslationActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    cancel();
    return;
  }
  if (state == CONNECTING) {
    pollQuickConnect();
    return;
  }

  switch (PanelTouch::read(renderer, mappedInput)) {
    case PanelTouch::Action::Close:
      cancel();
      return;
    case PanelTouch::Action::Next:
    case PanelTouch::Action::ScrollDown:
      stepPage(1);
      return;
    case PanelTouch::Action::Previous:
    case PanelTouch::Action::ScrollUp:
      stepPage(-1);
      return;
    case PanelTouch::Action::AddButton:
    case PanelTouch::Action::None:
      break;
  }

  buttonNavigator.onNext([this] { stepPage(1); });
  buttonNavigator.onPrevious([this] { stepPage(-1); });
}

void EpubReaderTranslationActivity::render(RenderLock&&) {
  // No clearScreen: the panel floats over the reader's page, which is in the framebuffer or, after
  // the Wi-Fi list, back from SD.
  if (restoreBackgroundPending) {
    restoreBackgroundPending = false;
    restoreBackground(renderer);
  }

  char counter[16] = "";
  if (state == SHOWING_RESULT && textPages.pageCount() > 1) {
    snprintf(counter, sizeof(counter), "%d/%d", currentPage + 1, textPages.pageCount());
  }
  const auto layout =
      DictionaryPanel::draw(renderer, I18n::getInstance().get(title), hasPreTranslation ? "" : "Gemini", counter);

  if (state == SHOWING_RESULT) {
    auto* fcm = renderer.getFontCacheManager();
    auto scope = fcm->createPrewarmScope();
    textPages.draw(renderer, TRANSLATION_FONT_ID, layout.body.x, layout.body.y, translatedText, currentPage);
    scope.endScanAndPrewarm();
    textPages.draw(renderer, TRANSLATION_FONT_ID, layout.body.x, layout.body.y, translatedText, currentPage);
  } else {
    const char* status = state == CONNECTING    ? tr(STR_CONNECTING_SAVED_WIFI)
                         : state == TRANSLATING ? tr(STR_TRANSLATING)
                         : state == ERROR       ? errorMessage.c_str()
                                                : "";
    const auto lines = renderer.wrappedText(TRANSLATION_FONT_ID, status, layout.body.width, 6);
    int y = layout.body.y;
    for (const auto& line : lines) {
      renderer.drawText(TRANSLATION_FONT_ID, layout.body.x, y, line.c_str(), true);
      y += renderer.getLineHeight(TRANSLATION_FONT_ID);
    }
  }

  const bool paged = state == SHOWING_RESULT;
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", paged && currentPage > 0 ? "<" : "",
                                            paged && currentPage + 1 < textPages.pageCount() ? ">" : "");
  DictionaryPanel::clearButtonHints(renderer);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
