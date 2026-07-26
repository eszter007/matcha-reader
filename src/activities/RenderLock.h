#pragma once

class Activity;  // forward declaration

// RAII helper to lock rendering mutex for the duration of a scope.
class RenderLock {
  bool isLocked = false;  // true only when THIS object owns the mutex and must release it
  // True when the constructor found the lock already held BY THIS TASK. The guarantee the
  // caller wanted (nobody else renders while I work) holds, but the release belongs to the
  // outermost lock -- see the constructors for why nesting happens at all.
  bool nested = false;

  // True when the calling task is already the mutex holder (see the constructors).
  static bool heldByCurrentTask();

 public:
  // Tag for the non-blocking constructor below.
  struct Try {};

  explicit RenderLock();
  explicit RenderLock(Activity&);  // unused for now, but keep for compatibility
  // Non-blocking acquire (zero timeout): check held() for the outcome. On failure the object is
  // inert -- the destructor releases nothing. This exists because peek()-then-RenderLock has a
  // TOCTOU window: another task can take the mutex between the check and the constructor, turning
  // the "guarded" acquire into a full block. Use this from any task that must never wait on a
  // render (e.g. the input-polling loop task).
  explicit RenderLock(Try);
  RenderLock(const RenderLock&) = delete;
  RenderLock& operator=(const RenderLock&) = delete;
  ~RenderLock();
  bool held() const { return isLocked || nested; }
  void unlock();
  static bool peek();
};
