#pragma once

#include <PersistableStore.h>

#include <cstdint>
#include <string>
#include <vector>

// Anki sync settings (Settings > Connectivity > Anki, and the web settings page): where Anki is
// reached (Anki desktop with AnkiConnect, or the Matcha companion server), which Anki decks come
// onto the device. The key is obfuscated like other credentials.
namespace ankisync {

// An Anki deck reviewed on the device.
struct SyncedDeck {
  std::string name;  // Anki deck name, e.g. "Kaishi 1.5k"
};

}  // namespace ankisync

class AnkiSyncStore : public PersistableStore<AnkiSyncStore> {
 public:
  static constexpr size_t MAX_SYNCED = 16;

  static const char* getFilePath() { return "/.crosspoint/anki.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  // Base URL of AnkiConnect or the companion server, e.g. "http://192.168.1.20:8765".
  const std::string& getUrl() const { return url; }
  void setUrl(const std::string& value) { url = value; }
  // The endpoint requests go to: the URL with "http://" added when it has no scheme, and "/api/v1".
  std::string endpoint() const;
  const std::string& getKey() const { return key; }
  void setKey(const std::string& value) { key = value; }
  bool getSyncAnkiWeb() const { return syncAnkiWeb; }
  void setSyncAnkiWeb(bool on) { syncAnkiWeb = on; }
  bool isConfigured() const { return !url.empty(); }

  const std::vector<ankisync::SyncedDeck>& getSyncedDecks() const { return synced; }
  void setSyncedDecks(std::vector<ankisync::SyncedDeck> decks);

 private:
  AnkiSyncStore() = default;
  ~AnkiSyncStore() = default;
  friend class PersistableStore<AnkiSyncStore>;

  std::string url;
  std::string key;
  bool syncAnkiWeb = true;
  std::vector<ankisync::SyncedDeck> synced;
};

#define ANKI_STORE AnkiSyncStore::getInstance()

namespace ankisync {
// The synced decks as the comma-separated text the settings show, and back. Deck names keep
// Anki's "::" for subdecks; a comma cannot appear in a name here.
std::string joinDeckNames(const std::vector<SyncedDeck>& decks);
std::vector<SyncedDeck> splitDeckNames(const std::string& text);
}  // namespace ankisync
