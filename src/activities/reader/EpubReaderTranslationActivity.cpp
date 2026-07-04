#include "EpubReaderTranslationActivity.h"

#include <ArduinoJson.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

constexpr int HTTP_BUF_SIZE = 2048;
constexpr uint32_t MIN_HEAP_FOR_TLS = 55000;
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
constexpr const char* GEMINI_MODEL = "gemini-2.5-flash";

struct ResponseBuffer {
  char* data = nullptr;
  int len = 0;
  int capacity = 0;

  ~ResponseBuffer() { free(data); }

  bool ensure(int size) {
    if (size <= capacity) return true;
    int newCap = capacity == 0 ? 1024 : capacity;
    while (newCap < size) newCap *= 2;
    char* newData = static_cast<char*>(realloc(data, newCap));
    if (!newData) return false;
    data = newData;
    capacity = newCap;
    return true;
  }
};

esp_err_t httpEventHandler(esp_http_client_event_t* evt) {
  auto* buf = static_cast<ResponseBuffer*>(evt->user_data);
  if (evt->event_id == HTTP_EVENT_ON_DATA && buf) {
    if (buf->ensure(buf->len + evt->data_len + 1)) {
      memcpy(buf->data + buf->len, evt->data, evt->data_len);
      buf->len += evt->data_len;
      buf->data[buf->len] = '\0';
    } else {
      LOG_ERR("XLAT", "Response buffer OOM (%d bytes)", evt->data_len);
    }
  }
  return ESP_OK;
}

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
  LOG_DBG("XLAT", "Entering translation (free heap: %u)", static_cast<unsigned>(freeHeap));
  if (freeHeap < MIN_HEAP_FOR_WIFI_INIT) {
    LOG_ERR("XLAT", "Insufficient heap for WiFi init: %u < %u", static_cast<unsigned>(freeHeap),
            static_cast<unsigned>(MIN_HEAP_FOR_WIFI_INIT));
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
  char buf[128];
  size_t len = Storage.readFileToBuffer(API_KEY_PATH, buf, sizeof(buf));
  if (len == 0) return false;

  // Trim trailing whitespace/newlines
  while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || buf[len - 1] == ' ')) {
    len--;
  }
  if (len == 0) return false;

  keyOut.assign(buf, len);
  return true;
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
  const uint32_t maxAllocHeap = ESP.getMaxAllocHeap();
  LOG_DBG("XLAT", "Calling Gemini (max alloc: %u)", static_cast<unsigned>(maxAllocHeap));
  if (maxAllocHeap < MIN_HEAP_FOR_TLS) {
    LOG_ERR("XLAT", "Insufficient contiguous heap for TLS: %u < %u", static_cast<unsigned>(maxAllocHeap),
            static_cast<unsigned>(MIN_HEAP_FOR_TLS));
    // Fragmentation, not exhaustion: total free is typically ~100KB+ here with no 55KB hole (the
    // WiFi driver's own init sprinkles allocations through whatever holes the reading session
    // left). Retry once on a pristine post-restart heap; see onEnter() for the rationale.
    if (!resumedAfterRestart && stashAndRestart()) return false;
    errorMessage = tr(STR_TRANSLATION_LOW_MEMORY);
    return false;
  }

  JsonDocument reqDoc;
  auto contents = reqDoc["contents"].to<JsonArray>();
  auto part = contents.add<JsonObject>();
  auto parts = part["parts"].to<JsonArray>();
  auto textPart = parts.add<JsonObject>();
  textPart["text"] =
      std::string("Translate the following Japanese text to English. "
                   "Return only the translation, no commentary.\n\n") +
      sourceText;

  auto config = reqDoc["generationConfig"].to<JsonObject>();
  config["temperature"] = 0.3;
  config["maxOutputTokens"] = 2048;

  std::string body;
  serializeJson(reqDoc, body);

  ResponseBuffer responseBuf;

  esp_http_client_config_t httpConfig = {};
  httpConfig.url = url.c_str();
  httpConfig.event_handler = httpEventHandler;
  httpConfig.user_data = &responseBuf;
  httpConfig.method = HTTP_METHOD_POST;
  httpConfig.timeout_ms = 30000;
  httpConfig.buffer_size = HTTP_BUF_SIZE;
  httpConfig.buffer_size_tx = HTTP_BUF_SIZE;
  httpConfig.crt_bundle_attach = esp_crt_bundle_attach;

  esp_http_client_handle_t client = esp_http_client_init(&httpConfig);
  if (!client) {
    errorMessage = tr(STR_TRANSLATION_FAILED);
    return false;
  }

  bool headerOk =
      esp_http_client_set_header(client, "Content-Type", "application/json") == ESP_OK;
  if (headerOk) {
    esp_http_client_set_post_field(client, body.c_str(), static_cast<int>(body.length()));
  }

  if (!headerOk) {
    esp_http_client_cleanup(client);
    errorMessage = tr(STR_TRANSLATION_FAILED);
    return false;
  }

  esp_err_t err = esp_http_client_perform(client);
  const int httpCode = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);

  LOG_DBG("XLAT", "Gemini response: HTTP %d (err: %d)", httpCode, err);

  if (err != ESP_OK || httpCode != 200 || !responseBuf.data) {
    LOG_ERR("XLAT", "API call failed: err=%d http=%d", err, httpCode);
    errorMessage = tr(STR_TRANSLATION_FAILED);
    return false;
  }

  JsonDocument respDoc;
  DeserializationError jsonErr = deserializeJson(respDoc, responseBuf.data);
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
      std::string scrollInfo =
          std::to_string(scrollOffset + 1) + "/" + std::to_string(maxScrollOffset + 1);
      renderer.drawText(SMALL_FONT_ID, screen.x + screen.width - metrics.contentSidePadding - 40,
                        contentBottom + 2, scrollInfo.c_str(), true);
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
