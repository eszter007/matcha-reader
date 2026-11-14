#include "AnkiSyncStore.h"

#include <ObfuscationUtils.h>

#include <algorithm>
#include <cstring>

namespace ankisync {
std::string joinDeckNames(const std::vector<SyncedDeck>& decks) {
  std::string out;
  for (const auto& d : decks) {
    if (!out.empty()) out += ", ";
    out += d.name;
  }
  return out;
}

std::vector<SyncedDeck> splitDeckNames(const std::string& text) {
  std::vector<SyncedDeck> out;
  out.reserve(4);
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find(',', start);
    if (end == std::string::npos) end = text.size();
    size_t a = start, b = end;
    while (a < b && (text[a] == ' ' || text[a] == '\t')) a++;
    while (b > a && (text[b - 1] == ' ' || text[b - 1] == '\t')) b--;
    if (b > a && out.size() < AnkiSyncStore::MAX_SYNCED) out.push_back({text.substr(a, b - a)});
    start = end + 1;
  }
  return out;
}
}  // namespace ankisync

using namespace ankisync;

std::string AnkiSyncStore::endpoint() const {
  std::string base = url;
  while (!base.empty() && base.back() == '/') base.pop_back();
  if (base.find("://") == std::string::npos) base = "http://" + base;
  return base + "/api/v1";
}

void AnkiSyncStore::toJson(JsonDocument& doc) const {
  doc["url"] = url;
  doc["key_obf"] = obfuscation::obfuscateToBase64(key);
  doc["syncAnkiWeb"] = syncAnkiWeb;
  JsonArray syncedJson = doc["synced"].to<JsonArray>();
  for (const auto& d : synced) {
    JsonObject o = syncedJson.add<JsonObject>();
    o["name"] = d.name;
    if (!d.id.empty()) o["id"] = d.id;
  }
}

bool AnkiSyncStore::fromJson(JsonVariantConst doc) {
  url = doc["url"] | "";
  bool ok = true;
  key = obfuscation::deobfuscateFromBase64(doc["key_obf"] | "", &ok);
  if (!ok) key.clear();
  // A key typed into the file by hand is taken, and stored obfuscated from then on.
  const char* plain = doc["key"] | "";
  if (key.empty() && *plain) {
    key = plain;
    requestResave();
  }
  syncAnkiWeb = doc["syncAnkiWeb"] | true;
  synced.clear();
  for (JsonVariantConst d : doc["synced"].as<JsonArrayConst>()) {
    if (synced.size() >= MAX_SYNCED) break;
    SyncedDeck s{d["name"] | "", d["id"] | ""};
    if (!s.name.empty()) synced.push_back(std::move(s));
  }
  return true;
}

void AnkiSyncStore::setSyncedDecks(std::vector<SyncedDeck> decks) {
  if (decks.size() > MAX_SYNCED) decks.resize(MAX_SYNCED);
  for (auto& d : decks) {
    if (!d.id.empty()) continue;
    for (const auto& old : synced) {
      if (old.name == d.name) d.id = old.id;
    }
  }
  synced = std::move(decks);
}
