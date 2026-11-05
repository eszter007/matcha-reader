#pragma once

// The protected-content reader clears the renderer's evictable caches when the heap is too
// fragmented to inflate an entry (see reclaimContentCaches() in ContentProtection.cpp). That is
// sound on a task that owns rendering and unsound on one that runs beside it: the cover worker
// opens and thumbnails books while a screen is drawing, and the glyph caches have no lock of
// their own, so a reclaim there frees them under a drawText in progress.
//
// A task that must not touch the render caches holds a Blocked for as long as it reads books.
// Its reads then go without the reclaim: on a fragmented heap the entry fails to open, which the
// worker already treats as "not now" and retries on a later pass.
//
// One slot, because one such task exists at a time (CoverWorker owns a single job thread).
namespace contentreclaim {

class Blocked {
 public:
  Blocked();
  ~Blocked();
  Blocked(const Blocked&) = delete;
  Blocked& operator=(const Blocked&) = delete;
};

// True when the calling task holds a Blocked.
bool blockedOnThisTask();

}  // namespace contentreclaim
