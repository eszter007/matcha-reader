#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdint>

#include "RecentBook.h"

// Cover thumbnails off the loop task. Opening a book to reach its cover costs 213ms typical and
// 2347ms worst on device, so doing it inline freezes whichever screen asked; this runs one
// conversion at a time on its own FreeRTOS task and hands the outcome back.
//
// One lower-priority job at a time. Release/acquire stores on `busy` publish the job to the
// worker and its result back to the loop, so the non-atomic structs are never accessed
// concurrently; the loop task is their only writer while busy == false.
//
// Shared by the Library and Home. What each screen keeps for itself is the policy: which book to
// convert next, and what to do with the result (the Library also records it in library.idx).
class CoverWorker {
 public:
  struct Job {
    RecentBook book;
    int gridHeight = 0;
    // Every thumb height this book still needs, generated in ONE job. Opening the book is what
    // costs (measured on device: 213ms typical, 2347ms worst, against milliseconds to scale the
    // cover once it is open), so a job per height paid that price again for each size. Unused
    // slots are 0. Three: the grid cell, the home cover, and SHELF_THUMB_HEIGHT.
    static constexpr int MAX_TARGET_HEIGHTS = 3;
    int targetHeights[MAX_TARGET_HEIGHTS] = {0, 0, 0};
    uint32_t fileSize = 0;
    uint32_t modifiedStamp = 0;

    void addTargetHeight(const int h) {
      if (h <= 0) return;
      for (int& slot : targetHeights) {
        if (slot == h) return;  // already queued
        if (slot == 0) {
          slot = h;
          return;
        }
      }
    }
  };

  struct Result {
    bool pending = false;
    bool completed = false;  // false means foreground work cancelled it; retry after idle
    bool hasGridThumb = false;
    // The book declares no cover image at all, so no future attempt can succeed. Distinct from
    // hasGridThumb=false, which usually means the conversion did not fit in the heap this time
    // and must be retried -- conflating the two costs a cover forever (see Epub::hasCoverImage).
    bool coverKnownAbsent = false;
    RecentBook book;
    uint32_t fileSize = 0;
    uint32_t modifiedStamp = 0;
  };

  // false when the task could not be created: the caller degrades to "no covers this visit"
  // rather than failing. Never call twice without stop() in between.
  bool start(const char* taskName);
  // Signals the task to exit and waits for it. MUST run before the owning activity is destroyed:
  // activities are heap-allocated and deleted on exit, and the task holds `this`.
  void stop();

  // false while a job is running or a result is still unconsumed.
  bool post(Job&& job);
  bool busy() const { return busy_.load(std::memory_order_acquire); }
  bool running() const { return task_ != nullptr; }
  // Abandon the current job. Called on real input so a key press is never queued behind a
  // conversion; the job is retried later, and heights already written to disk are kept.
  void requestCancel() { cancelRequested_ = true; }

  // The finished job, or nullptr while none is waiting. Clears the slot, so each result is
  // delivered exactly once.
  Result* takeResult() { return result_.pending ? &result_ : nullptr; }
  void consumeResult() { result_.pending = false; }

  // A cached cover thumb is only usable when it is exactly the requested height -- earlier builds
  // generated aspect-fill thumbs that could come out TALLER than requested (e.g. 232px for a
  // 226px request), which the home screen letterboxes into a white right stripe inside the cover
  // frame.
  static bool thumbHeightValid(const std::string& thumbPath, int h);

 private:
  static void trampoline(void* ctx);
  static bool shouldCancel(void* ctx);
  void loop();
  void runJob();

  TaskHandle_t task_ = nullptr;
  std::atomic<bool> exitRequested_{false};
  std::atomic<bool> exited_{false};
  std::atomic<bool> busy_{false};
  std::atomic<bool> cancelRequested_{false};
  std::atomic<bool> cancelSeen_{false};
  Job job_;
  Result result_;
};
