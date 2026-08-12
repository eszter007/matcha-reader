#include "EpubReaderTranslationActivity.h"

#include <ArduinoJson.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <SecureHttpClient.h>
#include <WiFi.h>
#include <esp_crt_bundle.h>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
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
constexpr const char* API_KEY_PATH = "/system/gemini.key";
constexpr const char* GEMINI_MODEL = "gemini-3.6-flash";

}  // namespace

EpubReaderTranslationActivity::EpubReaderTranslationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                             std::string sourceText, std::string preTranslatedText,
                                                             const bool resumedAfterRestart)
    : Activity("Translation", renderer, mappedInput),
      sourceText(std::move(sourceText)),
      resumedAfterRestart(resumedAfterRestart) {
  if (!preTranslatedText.empty()) {
    translatedText = std::move(preTranslatedText);
    hasPreTranslation = true;
    state = SHOWING_RESULT;
  }
}

bool EpubReaderTranslationActivity::stashAndRestart() {
  HalFile stash;
  if (!Storage.openFileForWrite("XLAT", TRANSLATE_STASH_PATH, stash)) {
    LOG_ERR("XLAT", "Could not write translation stash; showing low-memory error instead");
    return false;
  }
  const size_t written = stash.write(reinterpret_cast<const uint8_t*>(sourceText.data()), sourceText.size());
  stash.close();
  if (written != sourceText.size()) {
    LOG_ERR("XLAT", "Short write on translation stash (%u/%u); showing low-memory error instead",
            static_cast<unsigned>(written), static_cast<unsigned>(sourceText.size()));
    Storage.remove(TRANSLATE_STASH_PATH);
    return false;
  }
  LOG_DBG("XLAT", "Stashed %u bytes; restarting for a fresh heap", static_cast<unsigned>(sourceText.size()));
  silentRestartToTranslation();  // does not return
  return true;
}

void EpubReaderTranslationActivity::onEnter() {
  Activity::onEnter();

  if (hasPreTranslation) {
    requestUpdate();
    return;
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

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiComplete(!result.isCancelled); });
}

void EpubReaderTranslationActivity::onExit() {
  Activity::onExit();

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
  size_t len = Storage.readFileToBuffer(API_KEY_PATH, buf, sizeof(buf));
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
                         "Translate the following Japanese text to English. "
                         "Return only the translation, no commentary.\n\n") +
                     sourceText;

  auto config = reqDoc["generationConfig"].to<JsonObject>();
  config["maxOutputTokens"] = 2048;

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
    state = SHOWING_RESULT;
  } else {
    RenderLock lock(*this);
    state = ERROR;
  }
  requestUpdate();
}

void EpubReaderTranslationActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  if (state == SHOWING_RESULT) {
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down}, [this] {
      if (scrollOffset < maxScrollOffset) {
        scrollOffset++;
        requestUpdate();
      }
    });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up}, [this] {
      if (scrollOffset > 0) {
        scrollOffset--;
        requestUpdate();
      }
    });
  }
}

void EpubReaderTranslationActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto& theme = UITheme::getInstance();
  auto metrics = theme.getMetrics();
  Rect screen = theme.getScreenSafeArea(renderer, true, false);

  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_TRANSLATE_PAGE));

  const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int footerHeight = renderer.getLineHeight(SMALL_FONT_ID) + metrics.verticalSpacing;
  const int contentBottom = screen.y + screen.height - footerHeight;
  const int maxWidth = screen.width - metrics.contentSidePadding * 2;
  const int textX = screen.x + metrics.contentSidePadding;

  if (state == TRANSLATING) {
    UITheme::drawCenteredText(renderer, screen, UI_12_FONT_ID, screen.y + screen.height / 2, tr(STR_TRANSLATING), true);
  } else if (state == ERROR) {
    UITheme::drawCenteredText(renderer, screen, UI_12_FONT_ID, screen.y + screen.height / 2, errorMessage.c_str(),
                              true);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == SHOWING_RESULT) {
    const int fontId = UI_12_FONT_ID;
    const int lineHeight = renderer.getLineHeight(fontId);

    auto lines = renderer.wrappedText(fontId, translatedText.c_str(), maxWidth, 64);

    maxScrollOffset = static_cast<int>(lines.size()) - (contentBottom - contentTop) / lineHeight;
    if (maxScrollOffset < 0) maxScrollOffset = 0;
    if (scrollOffset > maxScrollOffset) scrollOffset = maxScrollOffset;

    int y = contentTop;
    for (int i = scrollOffset; i < static_cast<int>(lines.size()) && y + lineHeight <= contentBottom; i++) {
      renderer.drawText(fontId, textX, y, lines[i].c_str(), true);
      y += lineHeight;
    }

    if (maxScrollOffset > 0) {
      std::string scrollInfo = std::to_string(scrollOffset + 1) + "/" + std::to_string(maxScrollOffset + 1);
      renderer.drawText(SMALL_FONT_ID, screen.x + screen.width - metrics.contentSidePadding - 40, contentBottom + 2,
                        scrollInfo.c_str(), true);
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
