#include <ArduinoJson.h>
#include <ClippingStore.h>
#include <HalMemory.h>
#include <HalStorage.h>
#include <KOReaderCredentialStore.h>
#include <SecureHttpClient.h>
#include <strings.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

#include "BuildScratch.h"
#include "clippings/ClippingSync.h"
#include "sync/SyncRequest.h"

namespace {
constexpr const char* book = "/book.epub";
std::vector<uint8_t> framebuffer(52272);

void reset() {
  CLIPPINGS.unload();
  fake::reset();
  fake::syncHeap = 68000;
  fake::syncBufferAllocated = false;
  fake::headers.clear();
  fake::uploads.clear();
  fake::requests = 0;
  fake::failHttpAt = -1;
  fake::incomplete = false;
  fake::remoteResponse = "{\"cursor\":0,\"more\":false,\"items\":[]}";
  syncCredentials.enabled = true;
  syncCredentials.customHeaders.clear();
}
// Mirrors syncEngine::run for clippings alone.
bool runClippings() {
  if (!syncCredentials.enabled) return true;
  SyncRequest sync;
  return sync.ready() && clippingSync::run(sync, book, "hash");
}
void released() {
  auto* scratch = buildscratch::claim(32768);
  assert(scratch == framebuffer.data());
  buildscratch::release(scratch);
}
void add(const std::string& text) {
  assert(CLIPPINGS.loadForBook(book, "Book", "Author", "epub"));
  assert(CLIPPINGS.addClipping(0, 0, 0, 1, 0, 0, 1, "Chapter", 0, text, 1, 0, 3) == ClippingStore::AddResult::Added);
  CLIPPINGS.unload();
}
void maximumTextRoundTrip() {
  reset();
#if defined(CONFIG_IDF_TARGET_ESP32S3) && CONFIG_IDF_TARGET_ESP32S3
  fake::syncHeap = 100000;
#endif
  // The longest JSON a full clipping can take: every character a 4-byte emoji, escaped as a
  // 12-byte surrogate pair.
  std::string text;
  for (size_t i = 0; i < CLIPPING_TEXT_MAX / 4; ++i) text += "\xF0\x9F\x98\x80";
  add(text);
  JsonDocument response;
  response["cursor"] = 1;
  response["more"] = false;
  auto remote = response["items"].to<JsonArray>().add<JsonObject>();
  remote["id"] = "remote";
  remote["deleted"] = 0;
  remote["revision"] = 1;
  for (auto key : {"spine", "start_page", "end_page", "pages", "start_word", "end_word", "words", "created_at",
                   "layout_signature"})
    remote[key] = 1;
  std::string escapedText = "\"";
  for (size_t i = 0; i < CLIPPING_TEXT_MAX / 4; ++i) escapedText += "\\ud83d\\ude00";
  escapedText += '"';
  remote["text"] = serialized(escapedText);
  fake::remoteResponse.clear();
  serializeJson(response, fake::remoteResponse);
  assert(fake::remoteResponse.size() > 12 * 1024 && fake::remoteResponse.size() < SyncRequest::BUFFER_SIZE);
  assert(runClippings());
  released();
  assert(fake::uploads.size() == 1);
  assert(fake::uploads.front().size() > 4 * 1024);
  JsonDocument upload;
  assert(!deserializeJson(upload, fake::uploads.front()));
  assert(upload["items"][0]["text"].as<std::string>() == text);
  assert(CLIPPINGS.loadForBook(book, "", "", "epub"));
  assert(CLIPPINGS.clippingCount() == 2);
  std::string stored;
  assert(CLIPPINGS.readClippingText(1, stored));
  assert(stored == text);
  CLIPPINGS.unload();
}
bool hasHeader(const fake::Headers& headers, const std::string& name, const std::string& value) {
  return std::find(headers.begin(), headers.end(), std::make_pair(name, value)) != headers.end();
}
// Every clipping request sends one Authorization header: the configured one, else the Basic auth of progress sync.
void everyRequestSendsAuthHeaders(const SyncCredentials::Header& custom, const std::string& authorization) {
  reset();
  syncCredentials.customHeaders = {custom};
#if defined(CONFIG_IDF_TARGET_ESP32S3) && CONFIG_IDF_TARGET_ESP32S3
  fake::syncHeap = 100000;
#endif
  add("kept");
  add("removed");
  assert(runClippings());  // Probe, two uploads, pull.
  released();
  assert(CLIPPINGS.loadForBook(book, "", "", "epub"));
  assert(CLIPPINGS.removeClippingAt(1));
  CLIPPINGS.unload();
  fake::uploads.clear();
  assert(runClippings());  // Probe, delete, pull.
  released();
  assert(fake::uploads.size() == 1);
  assert(fake::uploads.front().find("\"deleted\":1") != std::string::npos);
  assert(fake::requests == 7 && fake::headers.size() == 7);
  for (const auto& headers : fake::headers) {
    assert(hasHeader(headers, "x-auth-user", "reader"));
    assert(hasHeader(headers, "x-auth-key", "key"));
    if (!custom.name.empty()) assert(hasHeader(headers, custom.name, custom.value));
    std::vector<std::string> values;
    for (const auto& [name, value] : headers) {
      if (strcasecmp(name.c_str(), "Authorization") == 0) values.push_back(value);
    }
    assert(values == std::vector<std::string>{authorization});
  }
}
}  // namespace

int main() {
  buildscratch::lend(framebuffer.data(), framebuffer.size());
  maximumTextRoundTrip();
  const std::string basic = "Basic cmVhZGVyOnBhc3M=";
  everyRequestSendsAuthHeaders({}, basic);
  everyRequestSendsAuthHeaders({"CF-Access-Client-Id", "client"}, basic);
  for (const char* name : {"Authorization", "authorization", "AuThOrIzAtIoN"}) {
    everyRequestSendsAuthHeaders({name, "Bearer fixture"}, "Bearer fixture");
  }
  for (int failure = 0; failure < 3; ++failure) {
    reset();
#if defined(CONFIG_IDF_TARGET_ESP32S3) && CONFIG_IDF_TARGET_ESP32S3
    fake::syncHeap = 100000;
#endif
    if (failure == 0) fake::failHttpAt = 0;
    if (failure == 1) fake::incomplete = true;
    if (failure == 2) fake::remoteResponse.assign(32768, 'x');
    assert(!runClippings());
    released();
  }
  reset();
  fake::failAlloc = 0;
  assert(!runClippings());
  released();
  reset();
  fake::syncHeap = 40000;  // too little left for TLS once the buffer is taken
  assert(!runClippings());
  assert(fake::requests == 0);
  buildscratch::reclaim();
  std::puts("Clipping sync tests passed");
}
