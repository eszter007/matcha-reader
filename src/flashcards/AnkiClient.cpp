#include "AnkiClient.h"

#include <Arduino.h>
#include <Logging.h>
#include <esp_heap_caps.h>

#include <cstring>

bool AnkiClient::begin(const std::string& endpoint, const std::string& key) {
  url = endpoint;
  auth = key.empty() ? std::string() : "Bearer " + key;
  // Stepped down until it leaves room for the parsed reply and the sync's own work; larger replies then
  // fail as TooLarge, which the sync handles. The smallest size is taken whenever it fits at all.
  constexpr size_t LEAVE_FREE = 56 * 1024;  // measured on an X4: Wi-Fi traffic, parsed replies, the card table
  for (const size_t size : {MAX_RESPONSE, MAX_RESPONSE * 2 / 3, MIN_RESPONSE}) {
    if (buffer) break;
    if (size > MIN_RESPONSE && heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < size + LEAVE_FREE) continue;
    buffer = makeUniqueNoThrow<char[]>(size + 1);
    if (buffer) capacity_ = size;
  }
  LOG_DBG("ANKI", "Reply buffer %u bytes", static_cast<unsigned>(capacity_));
  if (!buffer) {
    LOG_ERR("ANKI", "OOM: %u bytes for responses", static_cast<unsigned>(MIN_RESPONSE));
    error_ = {Error::Kind::NoMemory, 0, {}, {}};
    return false;
  }
  http.setTimeout(30000);
  http.setInsecure();  // the companion server may sit behind any certificate
  return true;
}

void AnkiClient::end() {
  http.end();
  buffer.reset();
  capacity_ = 0;
}

bool AnkiClient::call(const char* method, const JsonDocument& params, JsonDocument& result,
                      const JsonDocument* filter) {
  error_ = {};
  result.clear();
  if (!buffer) {
    error_.kind = Error::Kind::NoMemory;
    return false;
  }
  std::string body;
  {
    JsonDocument request;
    request["jsonrpc"] = "2.0";
    request["id"] = nextId++;
    request["method"] = method;
    request["params"] = params.as<JsonVariantConst>();
    body.reserve(measureJson(request) + 1);
    serializeJson(request, body);
  }
  if (!http.begin(url)) {
    error_ = {Error::Kind::Network, 0, {}, "Bad address"};
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  if (!auth.empty()) http.addHeader("Authorization", auth);
  length = 0;
  overflow = false;
  const int status = http.sendRequest("POST", reinterpret_cast<const uint8_t*>(body.data()), body.size(),
                                      [this](const uint8_t* data, const size_t len) {
                                        if (length + len > capacity_) {
                                          overflow = true;
                                          return false;
                                        }
                                        memcpy(buffer.get() + length, data, len);
                                        length += len;
                                        return true;
                                      });
  if (overflow) {
    error_ = {Error::Kind::TooLarge, 0, {}, "Reply too large"};
    LOG_ERR("ANKI", "%s: reply over %u bytes", method, static_cast<unsigned>(capacity_));
    return false;
  }
  if (status < 0 || !http.responseComplete()) {
    error_ = {Error::Kind::Network, status, {}, "No reply"};
    LOG_ERR("ANKI", "%s: no reply (%d)", method, status);
    return false;
  }
  if (status == 401 || status == 403) {
    error_ = {status == 401 ? Error::Kind::Unauthorized : Error::Kind::Forbidden, status, {}, {}};
    LOG_ERR("ANKI", "%s: HTTP %d", method, status);
    return false;
  }
  if (status != 200) {
    error_ = {Error::Kind::Http, status, {}, {}};
    LOG_ERR("ANKI", "%s: HTTP %d", method, status);
    return false;
  }
  buffer[length] = '\0';
  JsonDocument reply;
  DeserializationError parsed;
  if (filter) {
    JsonDocument wanted;
    wanted["result"] = filter->as<JsonVariantConst>();
    wanted["error"] = true;
    parsed = deserializeJson(reply, buffer.get(), length, DeserializationOption::Filter(wanted));
  } else {
    parsed = deserializeJson(reply, buffer.get(), length);
  }
  if (parsed) {
    error_ = {Error::Kind::BadReply, 0, {}, parsed.c_str()};
    LOG_ERR("ANKI", "%s: unreadable reply (%s)", method, parsed.c_str());
    return false;
  }
  if (!reply["error"].isNull()) {
    error_.kind = Error::Kind::Rpc;
    error_.code = reply["error"]["code"] | 0;
    error_.type = reply["error"]["data"]["type"] | "";
    error_.message = reply["error"]["message"] | "";
    LOG_ERR("ANKI", "%s: %s (%s)", method, error_.message.c_str(), error_.type.c_str());
    return false;
  }
  result.set(reply["result"]);
  return true;
}

bool AnkiClient::callOperation(const char* method, const JsonDocument& params, JsonDocument& result,
                               const uint32_t timeoutMs) {
  JsonDocument handle;
  if (!call(method, params, handle)) return false;
  JsonDocument poll;
  poll["operationId"] = handle["operationId"].as<const char*>();
  const uint32_t start = millis();
  for (;;) {
    JsonDocument op;
    if (!call("operations.get", poll, op)) return false;
    const char* state = op["state"] | "";
    if (std::strcmp(state, "succeeded") == 0) {
      result.set(op["result"]);
      return true;
    }
    if (std::strcmp(state, "failed") == 0 || std::strcmp(state, "cancelled") == 0) {
      error_.kind = Error::Kind::Rpc;
      error_.code = op["error"]["code"] | 0;
      error_.type = op["error"]["data"]["type"] | "";
      error_.message = op["error"]["message"] | "";
      LOG_ERR("ANKI", "%s failed: %s", method, error_.message.c_str());
      return false;
    }
    if (std::strcmp(state, "waitingForUser") == 0 || millis() - start > timeoutMs) {
      error_ = {Error::Kind::Rpc, 0, "timeout", std::string(method) + " did not finish"};
      return false;
    }
    delay(500);
  }
}

std::string AnkiClient::describeError() const {
  switch (error_.kind) {
    case Error::Kind::None:
      return {};
    case Error::Kind::NoMemory:
      return "low memory";
    case Error::Kind::Network:
      return "could not reach Anki";
    case Error::Kind::Unauthorized:
      return "key refused";
    case Error::Kind::Forbidden:
      return "this device is not allowed";
    case Error::Kind::Http:
      return "HTTP " + std::to_string(error_.code);
    case Error::Kind::TooLarge:
      return "reply too large";
    case Error::Kind::BadReply:
      return "unreadable reply";
    case Error::Kind::Rpc:
      return error_.message.empty() ? error_.type : error_.message;
    case Error::Kind::DeckMissing:
      return "deck not in Anki";
  }
  return {};
}
