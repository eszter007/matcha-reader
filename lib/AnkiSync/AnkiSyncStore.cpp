#include "AnkiSyncStore.h"

#include <Logging.h>
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

void FlashcardSyncStore::toJson(JsonDocument& doc) const {
  doc["enabled"] = enabled;
  doc["url"] = url;
  doc["key_obf"] = obfuscation::obfuscateToBase64(key);
  doc["minedTo"] = static_cast<uint8_t>(minedTo);
}

bool FlashcardSyncStore::fromJson(JsonVariantConst doc) {
  enabled = doc["enabled"] | true;
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
  minedTo = (doc["minedTo"] | 0) == 1 ? MinedTo::Migaku : MinedTo::Anki;
  return true;
}

namespace {
const std::string NO_SERVER;
}  // namespace

const std::string& SyncServiceStore::serverUrl() const {
  if (server != Server::Shared) return url;
  return FLASHCARD_SYNC_STORE.isEnabled() ? FLASHCARD_SYNC_STORE.getUrl() : NO_SERVER;
}

const std::string& SyncServiceStore::serverKey() const {
  if (server != Server::Shared) return key;
  return FLASHCARD_SYNC_STORE.isEnabled() ? FLASHCARD_SYNC_STORE.getKey() : NO_SERVER;
}

std::string SyncServiceStore::endpoint() const {
  std::string base = serverUrl();
  while (!base.empty() && base.back() == '/') base.pop_back();
  if (base.find("://") == std::string::npos) base = "http://" + base;
  return base + apiPath;
}

void SyncServiceStore::writeJson(JsonDocument& doc) const {
  doc["server"] = static_cast<uint8_t>(server);
  doc["url"] = url;
  doc["key_obf"] = obfuscation::obfuscateToBase64(key);
  JsonArray syncedJson = doc["synced"].to<JsonArray>();
  for (const auto& d : synced) {
    JsonObject o = syncedJson.add<JsonObject>();
    o["name"] = d.name;
    if (!d.id.empty()) o["id"] = d.id;
  }
}

void SyncServiceStore::readJson(JsonVariantConst doc, bool& upgraded) {
  upgraded = false;
  url = doc["url"] | "";
  const JsonVariantConst serverJson = doc["server"];
  if (serverJson.is<uint8_t>() && serverJson.as<uint8_t>() <= static_cast<uint8_t>(Server::Own)) {
    server = static_cast<Server>(serverJson.as<uint8_t>());
  }
  bool ok = true;
  key = obfuscation::deobfuscateFromBase64(doc["key_obf"] | "", &ok);
  if (!ok) key.clear();
  // A key typed into the file by hand is taken, and stored obfuscated from then on.
  const char* plain = doc["key"] | "";
  if (key.empty() && *plain) {
    key = plain;
    upgraded = true;
  }
  synced.clear();
  for (JsonVariantConst d : doc["synced"].as<JsonArrayConst>()) {
    if (synced.size() >= MAX_SYNCED) break;
    SyncedDeck s{d["name"] | "", d["id"] | ""};
    if (!s.name.empty()) synced.push_back(std::move(s));
  }
}

void SyncServiceStore::setSyncedDecks(std::vector<SyncedDeck> decks) {
  if (decks.size() > MAX_SYNCED) decks.resize(MAX_SYNCED);
  for (auto& d : decks) {
    if (!d.id.empty()) continue;
    for (const auto& old : synced) {
      if (old.name == d.name) d.id = old.id;
    }
  }
  synced = std::move(decks);
}

void AnkiSyncStore::toJson(JsonDocument& doc) const { writeJson(doc); }

bool AnkiSyncStore::fromJson(JsonVariantConst doc) {
  bool upgraded = false;
  readJson(doc, upgraded);
  if (upgraded) requestResave();
  return true;
}

void MigakuSyncStore::toJson(JsonDocument& doc) const { writeJson(doc); }

bool MigakuSyncStore::fromJson(JsonVariantConst doc) {
  bool upgraded = false;
  readJson(doc, upgraded);
  if (upgraded) requestResave();
  return true;
}

namespace ankisync {
void loadSyncStores() {
  FLASHCARD_SYNC_STORE.loadFromFile();
  ANKI_STORE.loadFromFile();
  MIGAKU_STORE.loadFromFile();
}

bool minedGoToMigaku() {
  return MIGAKU_STORE.isConfigured() &&
         (FLASHCARD_SYNC_STORE.getMinedTo() == FlashcardSyncStore::MinedTo::Migaku || !ANKI_STORE.isConfigured());
}
}  // namespace ankisync
