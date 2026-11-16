#include "AnkiSyncStore.h"

#include <HalStorage.h>
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
  doc["url"] = url;
  doc["key_obf"] = obfuscation::obfuscateToBase64(key);
  doc["minedTo"] = static_cast<uint8_t>(minedTo);
}

bool FlashcardSyncStore::fromJson(JsonVariantConst doc) {
  url = doc["url"] | "";
  bool ok = true;
  key = obfuscation::deobfuscateFromBase64(doc["key_obf"] | "", &ok);
  if (!ok) key.clear();
  minedTo = (doc["minedTo"] | 0) == 1 ? MinedTo::Migaku : MinedTo::Anki;
  return true;
}

const std::string& SyncServiceStore::serverUrl() const {
  return server == Server::Shared ? FLASHCARD_SYNC_STORE.getUrl() : url;
}

const std::string& SyncServiceStore::serverKey() const {
  return server == Server::Shared ? FLASHCARD_SYNC_STORE.getKey() : key;
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
  // A file from before the shared server has no "server": its own address, if any, stays in use.
  // loadSyncStores() saves the result, after the shared server.
  const JsonVariantConst serverJson = doc["server"];
  if (serverJson.is<uint8_t>() && serverJson.as<uint8_t>() <= static_cast<uint8_t>(Server::Own)) {
    server = static_cast<Server>(serverJson.as<uint8_t>());
  } else if (!url.empty()) {
    server = Server::Own;
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

void AnkiSyncStore::toJson(JsonDocument& doc) const {
  writeJson(doc);
  doc["syncAnkiWeb"] = syncAnkiWeb;
}

bool AnkiSyncStore::fromJson(JsonVariantConst doc) {
  bool upgraded = false;
  readJson(doc, upgraded);
  syncAnkiWeb = doc["syncAnkiWeb"] | true;
  if (upgraded) requestResave();
  return true;
}

void MigakuSyncStore::toJson(JsonDocument& doc) const {
  writeJson(doc);
  // Kept for loadSyncStores(), which reads it while flashcard_sync.json is missing.
  if (legacyTakesMined) doc["takesMined"] = true;
}

bool MigakuSyncStore::fromJson(JsonVariantConst doc) {
  bool upgraded = false;
  readJson(doc, upgraded);
  legacyTakesMined = doc["takesMined"] | false;
  if (upgraded) requestResave();
  return true;
}

namespace ankisync {
namespace {
// Addresses that differ only in "http://" or a trailing slash name the same server.
std::string comparableUrl(const std::string& url) {
  size_t start = url.rfind("http://", 0) == 0 ? 7 : 0;
  size_t end = url.size();
  while (end > start && url[end - 1] == '/') end--;
  return url.substr(start, end - start);
}

// A service with its own server that is the shared one switches to the shared server.
void shareIfSame(SyncServiceStore& store) {
  if (store.getServer() != SyncServiceStore::Server::Own) return;
  const auto& shared = FLASHCARD_SYNC_STORE;
  if (comparableUrl(store.getUrl()) != comparableUrl(shared.getUrl()) || store.getKey() != shared.getKey()) return;
  store.setServer(SyncServiceStore::Server::Shared);
  store.setUrl("");
  store.setKey("");
}
}  // namespace

void loadSyncStores() {
  ANKI_STORE.loadFromFile();
  MIGAKU_STORE.loadFromFile();
  // Only a missing file means older firmware: an unreadable one must not reset the shared server.
  // A save cut short leaves the file as ".tmp", which the load recovers.
  const std::string path = FlashcardSyncStore::getFilePath();
  if (Storage.exists(path.c_str()) || Storage.exists((path + ".tmp").c_str())) {
    FLASHCARD_SYNC_STORE.loadFromFile();
    return;
  }

  // No shared server yet: set it up from the services' own servers.
  auto& shared = FLASHCARD_SYNC_STORE;
  using Server = SyncServiceStore::Server;
  const SyncServiceStore* source = nullptr;
  if (MIGAKU_STORE.getServer() == Server::Own) {
    source = &MIGAKU_STORE;
  } else if (ANKI_STORE.getServer() == Server::Own) {
    source = &ANKI_STORE;
  }
  if (source) {
    shared.setUrl(source->getUrl());
    shared.setKey(source->getKey());
    // Older firmware synced only the services with an address: the others stay off, saved first
    // so a failed save below cannot leave one on.
    for (SyncServiceStore* store :
         {static_cast<SyncServiceStore*>(&ANKI_STORE), static_cast<SyncServiceStore*>(&MIGAKU_STORE)}) {
      if (store->getServer() == Server::Own) continue;
      store->setServer(Server::Off);
      store->save();
    }
  }
  shared.setMinedTo(MIGAKU_STORE.getLegacyTakesMined() ? FlashcardSyncStore::MinedTo::Migaku
                                                       : FlashcardSyncStore::MinedTo::Anki);
  // The services keep their own addresses until the shared one is safely on the card.
  if (!shared.saveToFile()) {
    LOG_ERR("ANKI", "Flashcards Sync: could not save the shared server");
    return;
  }
  if (!source) return;  // a fresh device: the services keep their defaults
  shareIfSame(MIGAKU_STORE);
  shareIfSame(ANKI_STORE);
  ANKI_STORE.saveToFile();
  MIGAKU_STORE.saveToFile();
  LOG_INF("ANKI", "Flashcards Sync: shared server set up (Anki %u, Migaku %u)",
          static_cast<unsigned>(ANKI_STORE.getServer()), static_cast<unsigned>(MIGAKU_STORE.getServer()));
}

bool minedGoToMigaku() {
  return MIGAKU_STORE.isConfigured() &&
         (FLASHCARD_SYNC_STORE.getMinedTo() == FlashcardSyncStore::MinedTo::Migaku || !ANKI_STORE.isConfigured());
}
}  // namespace ankisync
