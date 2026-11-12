#pragma once

#include <cstdint>
#include <string>

#include "AnkiClient.h"

// One sync with Anki (desktop with AnkiConnect, or the companion server), run with Wi-Fi up:
// the Anki decks chosen in the settings come onto the device (cards rendered as Anki renders
// them, their CSS and images), and reviews go both ways. See docs/anki-sync-design.md.
namespace ankisync {

struct Progress {
  std::string deck;  // the deck being synced
  uint32_t done = 0;
  uint32_t total = 0;
};
struct ProgressFn {
  void* ctx = nullptr;
  void (*fn)(void* ctx, const Progress& progress) = nullptr;
  void operator()(const Progress& p) const {
    if (fn) fn(ctx, p);
  }
};

struct Result {
  bool ok = false;
  std::string failedDeck;  // where it stopped, when not ok
  AnkiClient::Error error;
  uint32_t cardsAdded = 0;
  uint32_t reviewsPulled = 0;
  uint32_t reviewsPushed = 0;
};

// Checks the address and key; fills the running Anki's version. False with client.error() set.
bool testConnection(AnkiClient& client, std::string& ankiVersion);

// nowMs: the time from the network clock, set at the start of this sync; 0 when it could not be
// set. Reviews the device stamped later than that are sent as just before it.
Result syncAll(AnkiClient& client, const ProgressFn& progress, int64_t nowMs);

}  // namespace ankisync
