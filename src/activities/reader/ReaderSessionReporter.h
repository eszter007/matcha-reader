#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "ReaderSession.h"

// Feeds a ReaderSession from a reader's two tasks and emits it as the reader.session plugin
// event. Shared by every reader -- ReaderActivity (EPUB, TXT, XTC) and the manga reader, which
// has no common base with them -- so a format reports its reading the same way or not at all.
class ReaderSessionReporter {
 public:
  // Loop task, when a page turn was handled. Takes no lock: the session is owned by the render
  // task, which holds the render lock for the whole of a vertical chapter build, and waiting on
  // it here would stall every press made during the build. The outcome is parked and applied by
  // pageRendered(), just before the page it produced is counted.
  void noteTurn(bool forward, bool succeeded);

  // Render task, once a page is on the panel. progressBp: whole-book progress, 0..10000.
  void pageRendered(int progressBp);

  // Emits the session when it is worth reporting and something subscribes, then starts a new
  // one. documentPath is the file the document id is computed from: the book itself, or for a
  // folder-based book a file inside it.
  void flush(const std::string& bookPath, const std::string& documentPath);

 private:
  enum : uint8_t { TURN_NONE, TURN_FORWARD, TURN_OTHER };
  ReaderSession session_;
  std::atomic<uint8_t> pendingTurn_{TURN_NONE};
};
