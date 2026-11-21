#include "SyncRequest.h"

#include <HalMemory.h>
#include <KOReaderCredentialStore.h>
#include <Logging.h>
#include <Memory.h>
#include <strings.h>

#include <cstring>

namespace {
// Left after the buffer for the TLS handshake; the same floor KOReaderSyncClient applies.
constexpr size_t MIN_FREE_FOR_TLS = 35000;
constexpr size_t MIN_BLOCK_FOR_TLS = 20000;
}  // namespace

SyncRequest::SyncRequest() {
  buffer = makeUniqueNoThrow<char[]>(BUFFER_SIZE).release();
  http = makeUniqueNoThrow<freeink::SecureHttpClient>();
  if (http) http->setInsecure();
}

SyncRequest::~SyncRequest() { delete[] buffer; }

bool SyncRequest::ready() const {
  const auto heap = HalMemory::getDefaultHeap();
  if (buffer && http && heap.freeBytes >= MIN_FREE_FOR_TLS && heap.largestBlockBytes >= MIN_BLOCK_FOR_TLS) return true;
  LOG_ERR("SYNC", "Insufficient heap for sync: %zu free, %zu max block", heap.freeBytes, heap.largestBlockBytes);
  return false;
}

std::string SyncRequest::account() { return KOREADER_STORE.getUsername() + "@" + KOREADER_STORE.getBaseUrl(); }

bool SyncRequest::supportsDeltaSync(const std::string& url) {
  JsonDocument doc;
  return send(url, doc) && doc["sync_version"].as<unsigned>() == 2;
}

size_t SyncRequest::serialize(const JsonDocument& doc, char* out, const size_t size) {
  if (doc.overflowed() || measureJson(doc) >= size) return 0;
  return serializeJson(doc, out, size);
}

bool SyncRequest::parse(JsonDocument& doc, char* text, const size_t length) {
  return !deserializeJson(doc, text, length, DeserializationOption::NestingLimit(4));
}

bool SyncRequest::send(const std::string& url, JsonDocument& doc, const bool upload) {
  size_t payloadSize = 0;
  if (upload) {
    payloadSize = serialize(doc, buffer, BUFFER_SIZE);
    if (payloadSize == 0) {
      LOG_ERR("SYNC", "Request exceeds buffer");
      return false;
    }
  }
  return transfer(url, upload ? "PUT" : "GET", payloadSize, doc);
}

bool SyncRequest::sendBody(const std::string& url, const size_t length, JsonDocument& response) {
  return length < BUFFER_SIZE && transfer(url, "PUT", length, response);
}

bool SyncRequest::transfer(const std::string& url, const char* method, const size_t payloadSize, JsonDocument& doc) {
  doc.clear();
  if (!http->begin(url)) {
    LOG_ERR("SYNC", "Invalid sync URL");
    return false;
  }
  // addHeader() appends, so a configured Authorization header replaces Basic auth instead of duplicating it.
  bool customAuthorization = false;
  for (const auto& header : KOREADER_STORE.getCustomHeaders()) {
    if (header.name.empty()) continue;
    http->addHeader(header.name, header.value);
    if (strcasecmp(header.name.c_str(), "Authorization") == 0) customAuthorization = true;
  }
  http->addHeader("x-auth-user", KOREADER_STORE.getUsername());
  http->addHeader("x-auth-key", KOREADER_STORE.getMd5Password());
  if (!customAuthorization) http->addHeader("Authorization", KOREADER_STORE.getBasicAuthorization());
  http->addHeader("Content-Type", "application/json");
  size_t received = 0;
  const int status = http->sendRequest(method, reinterpret_cast<const uint8_t*>(buffer), payloadSize,
                                       [&](const uint8_t* data, const size_t length) {
                                         if (length >= BUFFER_SIZE - received) return false;
                                         memcpy(buffer + received, data, length);
                                         received += length;
                                         return true;
                                       });
  if (status != 200 || !http->responseComplete()) {
    LOG_ERR("SYNC", "HTTP %d (complete=%d)", status, http->responseComplete());
    return false;
  }
  buffer[received] = '\0';
  if (!parse(doc, buffer, received)) {
    LOG_ERR("SYNC", "Invalid response");
    return false;
  }
  return true;
}
