#pragma once

#include <PersistableStore.h>

#include <cstdint>
#include <string>
#include <vector>

// Flashcard sync settings (Settings > Connectivity > Flashcards Sync):
// - FlashcardSyncStore: the sync server both services can share, the Matcha companion server.
// - One store per service: Anki (the companion server, or Anki desktop with AnkiConnect as its
//   own server) and Migaku (only the companion server's Migaku bridge, which answers in the same
//   AnkiConnect API under /migaku/api/v1). Each is off or uses its server, and says which of its
//   decks come onto the device.
// Keys are obfuscated like other credentials.
namespace ankisync {

// A deck reviewed on the device.
struct SyncedDeck {
  std::string name;  // the service's deck name, e.g. "Kaishi 1.5k"
  std::string id;    // the service's deck id once a sync has found it; follows the deck through renames
};

}  // namespace ankisync

// The server both services can share, and where mined words go when both are on.
class FlashcardSyncStore : public PersistableStore<FlashcardSyncStore> {
 public:
  enum class MinedTo : uint8_t { Anki = 0, Migaku = 1 };

  static const char* getFilePath() { return "/.crosspoint/flashcard_sync.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  // Off: no shared server; each service is off or has its own. The address and key are kept.
  bool isEnabled() const { return enabled; }
  void setEnabled(bool on) { enabled = on; }
  // Base URL of the server, e.g. "http://192.168.1.20:8080".
  const std::string& getUrl() const { return url; }
  void setUrl(const std::string& value) { url = value; }
  const std::string& getKey() const { return key; }
  void setKey(const std::string& value) { key = value; }
  MinedTo getMinedTo() const { return minedTo; }
  void setMinedTo(MinedTo value) { minedTo = value; }

 private:
  FlashcardSyncStore() = default;
  ~FlashcardSyncStore() = default;
  friend class PersistableStore<FlashcardSyncStore>;

  bool enabled = true;
  std::string url;
  std::string key;
  MinedTo minedTo = MinedTo::Anki;
};

#define FLASHCARD_SYNC_STORE FlashcardSyncStore::getInstance()

// The settings both services have. Not a store itself: AnkiSyncStore and MigakuSyncStore below
// each keep one in their own file.
class SyncServiceStore {
 public:
  static constexpr size_t MAX_SYNCED = 16;
  // Stored as a number: the order is fixed by files already on cards.
  enum class Server : uint8_t { Off = 0, Shared = 1, Own = 2 };

  Server getServer() const { return server; }
  void setServer(Server value) { server = value; }
  // The service's own server, used when getServer() is Own; e.g. "http://192.168.1.20:8765".
  const std::string& getUrl() const { return url; }
  void setUrl(const std::string& value) { url = value; }
  const std::string& getKey() const { return key; }
  void setKey(const std::string& value) { key = value; }
  // The server this service talks to now: the shared one (none while it is turned off) or its own.
  const std::string& serverUrl() const;
  const std::string& serverKey() const;
  // The endpoint requests go to: serverUrl() with "http://" added when it has no scheme, and the
  // service's API path.
  std::string endpoint() const;
  // On, with an address to reach.
  bool isConfigured() const { return server != Server::Off && !serverUrl().empty(); }

  const std::vector<ankisync::SyncedDeck>& getSyncedDecks() const { return synced; }
  // Names already synced keep their deck ids.
  void setSyncedDecks(std::vector<ankisync::SyncedDeck> decks);
  // Writes the store's own file.
  virtual bool save() const = 0;

 protected:
  SyncServiceStore(const char* apiPath, Server defaultServer) : apiPath(apiPath), server(defaultServer) {}
  ~SyncServiceStore() = default;
  void writeJson(JsonDocument& doc) const;
  // False in `upgraded` unless the file used an older shape that should be saved again.
  void readJson(JsonVariantConst doc, bool& upgraded);

 private:
  const char* apiPath;
  Server server;
  std::string url;
  std::string key;
  std::vector<ankisync::SyncedDeck> synced;
};

class AnkiSyncStore : public SyncServiceStore, public PersistableStore<AnkiSyncStore> {
 public:
  static const char* getFilePath() { return "/.crosspoint/anki.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);
  bool save() const override { return saveToFile(); }

 private:
  AnkiSyncStore() : SyncServiceStore("/api/v1", Server::Shared) {}
  ~AnkiSyncStore() = default;
  friend class PersistableStore<AnkiSyncStore>;
};

class MigakuSyncStore : public SyncServiceStore, public PersistableStore<MigakuSyncStore> {
 public:
  static const char* getFilePath() { return "/.crosspoint/migaku.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);
  bool save() const override { return saveToFile(); }

 private:
  MigakuSyncStore() : SyncServiceStore("/migaku/api/v1", Server::Off) {}
  ~MigakuSyncStore() = default;
  friend class PersistableStore<MigakuSyncStore>;
};

#define ANKI_STORE AnkiSyncStore::getInstance()
#define MIGAKU_STORE MigakuSyncStore::getInstance()

namespace ankisync {
// Loads the three sync stores.
void loadSyncStores();
// Mined words go to Migaku: it is on and either takes them or Anki is off.
bool minedGoToMigaku();

// The synced decks as the comma-separated text the settings show, and back. Deck names keep
// Anki's "::" for subdecks; a comma cannot appear in a name here.
std::string joinDeckNames(const std::vector<SyncedDeck>& decks);
std::vector<SyncedDeck> splitDeckNames(const std::string& text);
}  // namespace ankisync
