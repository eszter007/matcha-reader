#pragma once

#include <cstdint>

// Persisted indices for the X3/X4 per-button side actions: append without
// reordering existing values. Mirrors CrossPointSettings::SIDE_BUTTON_ACTION.
enum class SideButtonAction : uint8_t {
  Default,
  Sleep,
  PrevPage,
  NextPage,
  Refresh,
  Footnotes,
  WordLookup,
  // "Off": the button does nothing in the reader. Restores what the pre-1.5
  // "Side Button Layout = Disabled" option used to do.
  None,
  Count
};

namespace side_button {

// True when either physical side button leaves Default, replacing the shared
// side-button roles (which the settings UI then hides).
inline bool customized(const uint8_t upper, const uint8_t lower) {
  return upper != static_cast<uint8_t>(SideButtonAction::Default) ||
         lower != static_cast<uint8_t>(SideButtonAction::Default);
}

// True when BOTH side buttons leave Default, so nothing in the reader can reach them under a
// shared name any more. Settings that only choose how the shared side-button roles are arranged
// are meaningless then -- but a setting that also governs the front pair or touch is NOT, so
// check this rather than customized() before hiding one.
inline bool fullyCustomized(const uint8_t upper, const uint8_t lower) {
  return upper != static_cast<uint8_t>(SideButtonAction::Default) &&
         lower != static_cast<uint8_t>(SideButtonAction::Default);
}

// Clamp a persisted value into range; corrupt saves fall back to Default.
inline uint8_t clampAction(const uint8_t v) {
  return v < static_cast<uint8_t>(SideButtonAction::Count) ? v : static_cast<uint8_t>(SideButtonAction::Default);
}

// Guards a side-button release against input ghosts: a side edge that
// coincides with (or closely follows) front-button activity is not treated as
// a deliberate side press. Subtraction is unsigned, so the millisecond-clock
// wrap is handled.
inline bool loneRelease(const unsigned long nowMs, const unsigned long lastFrontMs,
                        const unsigned long windowMs = 150) {
  return nowMs - lastFrontMs > windowMs;
}

}  // namespace side_button
