#pragma once

class Activity;  // forward declaration

// RAII helper to lock rendering mutex for the duration of a scope.
class RenderLock {
  bool isLocked = false;

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
  bool held() const { return isLocked; }
  void unlock();
  static bool peek();
};
