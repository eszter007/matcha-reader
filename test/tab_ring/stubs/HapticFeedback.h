#pragma once

// Test double for the haptic tap: counts what the ring asked for, so a test can tell an accepted
// touch from one that was only swallowed.
namespace haptic_feedback {
inline int taps = 0;
inline void touchAction(bool = false) { ++taps; }
}  // namespace haptic_feedback
