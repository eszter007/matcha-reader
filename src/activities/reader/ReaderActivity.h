#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "EndOfBookOptions.h"
#include "activities/Activity.h"

class ReaderActivity : public Activity {
 protected:
  const std::string bookPath;
  int pagesUntilFullRefresh;
  bool forcedRefreshPending = false;

  std::unique_ptr<EndOfBookOptions> endOfBookOptions;
  std::atomic<bool> endOfBookOptionsReady{false};

  explicit ReaderActivity(const char* name, GfxRenderer& renderer, MappedInputManager& mappedInput,
                          std::string bookPath, bool allowFastInitialRefresh);

  virtual bool loadBook() = 0;
  virtual bool hasBook() const = 0;
  virtual std::string getBookTitle() const = 0;
  virtual std::string getBookAuthor() const { return ""; }
  virtual std::string getBookThumbBmpPath() const { return ""; }
  virtual const char* getBookLanguage() const { return nullptr; }
  virtual void onReaderEnter() = 0;
  virtual void onReaderExit() = 0;
  virtual void readerLoop() = 0;

  virtual bool isAtEndOfBook() const { return false; }
  virtual void onReturnFromEndOfBook() {}

  void clearEndOfBookOptionsIfNeeded();
  bool handleBackNavigation();
  bool endOfBookMenuActive() const;
  bool handleEndOfBookMenu(bool suppressConfirmRelease = false);
  bool handleEndOfBookPageTurn(bool prevTriggered, bool nextTriggered);
  bool renderEndOfBook(const char* logTag);
  void disableFastInitialRefresh() { pagesUntilFullRefresh = 0; }

 public:
  ~ReaderActivity() override = default;

  static std::unique_ptr<Activity> create(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string path,
                                          bool allowFastInitialRefresh);

  void onEnter() final;
  void onExit() final;
  void loop() final;

  bool isReaderActivity() const final { return true; }
  bool appliesNightMode() const final { return true; }
  bool handleForcedRefresh() final;

 private:
  unsigned long readingSessionStartMs = 0;
};
