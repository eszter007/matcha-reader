#pragma once

#include <I18n.h>

#include <cstdint>
#include <string>

#include "AnkiClient.h"

class SyncServiceStore;

// One sync with Anki (desktop with AnkiConnect, or the companion server) or with Migaku (the
// companion server's Migaku bridge, which speaks the same API), run with Wi-Fi up: the decks
// chosen in the settings come onto the device (cards rendered as the server renders them, their
// CSS and images), and reviews go both ways. See docs/anki-sync-design.md.
namespace ankisync {

// What differs between the services. Both use the same requests.
struct Service {
  SyncServiceStore& store;
  const char* name;         // "Anki" or "Migaku", in logs and the sync summary
  const char* remoteName;   // the cloud the server syncs with: "AnkiWeb" or "Migaku"
  const char* localPrefix;  // added to the service's deck ids on the device
  bool takesMined;          // mined words go to this service
  bool requiresRemote;      // a server without the cloud's login is an error, not a local sync
                            // (the cloud is synced before and after every sync)
  // Messages that name the service.
  StrId unreachable, keyRefused, notAllowed, deckMissing, replyError, loginMissing, connected;
};
Service ankiService();
Service migakuService();

struct Progress {
  std::string deck;  // the deck being synced
  uint32_t done = 0;
  uint32_t total = 0;
};
struct ProgressFn {
  void* ctx = nullptr;
  void (*fn)(void* ctx, const Progress& progress) = nullptr;
  // Asked between cards: true stops the sync, keeping what is saved (Error::Kind::Cancelled).
  bool (*cancel)(void* ctx) = nullptr;
  void operator()(const Progress& p) const {
    if (fn) fn(ctx, p);
  }
  bool cancelled() const { return cancel && cancel(ctx); }
};

struct Result {
  bool ok = false;
  std::string failedDeck;  // where it stopped, when not ok
  AnkiClient::Error error;
  uint32_t cardsAdded = 0;
  uint32_t reviewsPulled = 0;
  uint32_t reviewsPushed = 0;
  bool remoteBlocked = false;  // the cloud (AnkiWeb) sync did not finish first; nothing was synced
  bool remoteSkipped = false;  // the cloud could not sync this time; the next sync tries again
};

// Checks the address and key; fills the running Anki's version. False with client.error() set.
bool testConnection(AnkiClient& client, std::string& ankiVersion);

// nowMs: the time from the network clock, set at the start of this sync; 0 when it could not be
// set. Reviews the device stamped later than that are sent as just before it.
Result syncAll(AnkiClient& client, const Service& service, const ProgressFn& progress, int64_t nowMs);

}  // namespace ankisync
