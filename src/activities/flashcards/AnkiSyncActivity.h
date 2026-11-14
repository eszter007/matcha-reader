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
  bool started = false;   // run() is called once, from loop()
  bool stopping = false;  // Back was pressed during the sync: it stops after the current card
  // The sync holds the main loop, so the buttons are read by a small task meanwhile: it only sets
  // backSeen. watchStop ends it; the task clears watchRunning on its way out.
  volatile bool backSeen = false;
  bool holdingBack = false;  // the cancel's Back is still down when the result shows
  volatile bool watchStop = false;
  volatile bool watchRunning = false;
  static void watchBack(void* arg);
  static void pollCancel(void* ctx);
};
