#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "activities/Activity.h"

namespace ankisync {
struct Service;
}

// Brings Wi-Fi up, then either syncs the decks of every service set up, Anki first, then Migaku
// (Flashcards > Sync Flashcards), or only checks each one's connection (Settings > Connectivity >
// Flashcards Sync > Test Connection). Wi-Fi goes down on exit.
class AnkiSyncActivity final : public Activity {
 public:
  enum class Mode : uint8_t { Sync, Test };
  // The sync screen's title for the services set up; nullptr when none is (no sync row then).
  static const char* syncLabel();
  AnkiSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Mode mode);
  // With both services set up, the device restarts between them: one Wi-Fi session leaves too
  // little contiguous heap for the second. The sync screen after that restart, carrying the first
  // service's outcome; nullptr when none was waiting.
  static std::unique_ptr<AnkiSyncActivity> resumeAfterRestart(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // A sync's result so far: the services done, and the first failure with where it happened.
  struct Outcome {
    std::string summary;
    std::string failure;
    std::string failedAt;
  };
  enum class State : uint8_t { Connecting, Working, Done, Failed, Cancelled };
  void onWifi(bool connected);
  void run();
  void test();
  void setStatus(State state, std::string message);
  // Keeps the outcome on SD for the sync after the restart; false when it could not.
  static bool stashOutcome(const Outcome& outcome);
  void showOutcome(const Outcome& outcome);

  Mode mode;
  State state = State::Connecting;
  std::string message;
  std::string detail;
  bool wifiUsed = false;
  bool started = false;             // run() is called once, from loop()
  bool stopping = false;            // Back was pressed during the sync: it stops after the current card
  bool resumed = false;             // started after the restart between services: Anki is done
  bool nextServicePending = false;  // onExit restarts into the sync of the next service
  Outcome carried;                  // the first service's outcome, from before that restart
  // The sync holds the main loop, so the buttons are read by a small task meanwhile: it only sets
  // backSeen. watchStop ends it; the task clears watchRunning on its way out.
  volatile bool backSeen = false;
  bool holdingBack = false;  // the cancel's Back is still down when the result shows
  volatile bool watchStop = false;
  volatile bool watchRunning = false;
  static void watchBack(void* arg);
  static void pollCancel(void* ctx);
};
