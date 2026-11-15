#pragma once

#include <PersistableStore.h>

#include <cstdint>
#include <string>
#include <vector>

// Flashcard sync settings, one store per service the device syncs with:
// - Anki (Settings > Connectivity > Anki Sync): Anki desktop with AnkiConnect, or the Matcha
//   companion server.
// - Migaku (Settings > Connectivity > Migaku Sync): the companion server's Migaku bridge, which
//   answers in the same AnkiConnect API under /migaku/api/v1.
// Each says where the service is reached and which of its decks come onto the device. The key is
// obfuscated like other credentials.
namespace ankisync {

// A deck reviewed on the device.
struct SyncedDeck {
  std::string name;  // the service's deck name, e.g. "Kaishi 1.5k"
  std::string id;    // the service's deck id once a sync has found it; follows the deck through renames
};

}  // namespace ankisync

// The settings both services have. Not a store itself: AnkiSyncStore and MigakuSyncStore below
// each keep one in their own file.
class SyncServiceStore {
 public:
  static constexpr size_t MAX_SYNCED = 16;

  // Base URL of AnkiConnect or the companion server, e.g. "http://192.168.1.20:8765".
  const std::string& getUrl() const { return url; }
  void setUrl(const std::string& value) { url = value; }
  // The endpoint requests go to: the URL with "http://" added when it has no scheme, and the
  // service's API path.
  std::string endpoint() const;
  const std::string& getKey() const { return key; }
  void setKey(const std::string& value) { key = value; }
  bool isConfigured() const { return !url.empty(); }

  const std::vector<ankisync::SyncedDeck>& getSyncedDecks() const { return synced; }
  // Names already synced keep their deck ids.
  void setSyncedDecks(std::vector<ankisync::SyncedDeck> decks);
  // Writes the store's own file.
  virtual bool save() const = 0;

 protected:
  explicit SyncServiceStore(const char* apiPath) : apiPath(apiPath) {}
  ~SyncServiceStore() = default;
  void writeJson(JsonDocument& doc) const;
  // False in `upgraded` unless the file used an older shape that should be saved again.
  void readJson(JsonVariantConst doc, bool& upgraded);

 private:
  const char* apiPath;
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

  // Ask the server to sync with AnkiWeb before and after. (The Migaku bridge always syncs with
  // Migaku: that is its sync.)
  bool getSyncAnkiWeb() const { return syncAnkiWeb; }
  void setSyncAnkiWeb(bool on) { syncAnkiWeb = on; }

 private:
  AnkiSyncStore() : SyncServiceStore("/api/v1") {}
  ~AnkiSyncStore() = default;
  friend class PersistableStore<AnkiSyncStore>;

  bool syncAnkiWeb = true;
};

class MigakuSyncStore : public SyncServiceStore, public PersistableStore<MigakuSyncStore> {
 public:
  static const char* getFilePath() { return "/.crosspoint/migaku.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);
  bool save() const override { return saveToFile(); }

  // Mined words become Migaku cards instead of Anki notes.
  bool getTakesMined() const { return takesMined; }
  void setTakesMined(bool on) { takesMined = on; }

 private:
  MigakuSyncStore() : SyncServiceStore("/migaku/api/v1") {}
  ~MigakuSyncStore() = default;
  friend class PersistableStore<MigakuSyncStore>;

  bool takesMined = false;
};

#define ANKI_STORE AnkiSyncStore::getInstance()
#define MIGAKU_STORE MigakuSyncStore::getInstance()

namespace ankisync {
// The synced decks as the comma-separated text the settings show, and back. Deck names keep
// Anki's "::" for subdecks; a comma cannot appear in a name here.
std::string joinDeckNames(const std::vector<SyncedDeck>& decks);
std::vector<SyncedDeck> splitDeckNames(const std::string& text);
}  // namespace ankisync
