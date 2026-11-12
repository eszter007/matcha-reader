#pragma once

#include <string>

#include "activities/Activity.h"

// Brings Wi-Fi up, then either syncs the Anki decks (Flashcards > Sync with Anki) or only checks
// the connection (Settings > Connectivity > Anki Sync > Test Connection). Wi-Fi goes down on exit.
class AnkiSyncActivity final : public Activity {
 public:
  enum class Mode : uint8_t { Sync, Test };
  AnkiSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Mode mode);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class State : uint8_t { Connecting, Working, Done, Failed, Cancelled };
  void onWifi(bool connected);
  void run();
  void setStatus(State state, std::string message);

  Mode mode;
  State state = State::Connecting;
  std::string message;
  std::string detail;
  bool wifiUsed = false;
  bool started = false;  // run() is called once, from loop()
};
