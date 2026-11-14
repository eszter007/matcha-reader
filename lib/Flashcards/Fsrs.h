#pragma once

#include <cstdint>

// FSRS-5 (Free Spaced Repetition Scheduler), the scheduler Anki ships as its recommended default.
// It models each card's memory with two numbers: stability (days until recall falls to 90%) and
// difficulty (1..10), and schedules the next review for when recall is predicted to drop to the
// desired retention. Pure arithmetic, no I/O: host-testable.
// Reference: https://github.com/open-spaced-repetition/fsrs4anki/wiki/The-Algorithm
namespace fsrs {

enum class Rating : uint8_t { Again = 1, Hard = 2, Good = 3, Easy = 4 };

struct Memory {
  float stability = 0;
  float difficulty = 0;
};

struct Params {
  float retention = 0.9f;        // target probability of recall at review time
  uint16_t maxInterval = 36500;  // days
};

// Probability of recall after `elapsedDays` for a card of this stability.
float retrievability(float elapsedDays, float stability);

// Days until recall falls to the desired retention, rounded and clamped to [1, maxInterval].
uint16_t intervalDays(float stability, const Params& params);

// Memory after the very first rating of a new card.
Memory initial(Rating rating);

// Memory after a review `elapsedDays` after the last one. 0 is a second look the same day (a card
// rated Again earlier in the session), which uses FSRS's short-term formula.
Memory next(const Memory& memory, float elapsedDays, Rating rating);

// The interval each rating would give, as Anki shows them over its buttons: indexed Again..Easy.
// `isNew`: the card has no memory yet. Again is 0 (seen again this session). Hard <= Good < Easy.
void previewIntervals(const Memory& memory, bool isNew, float elapsedDays, const Params& params, uint16_t out[4]);

}  // namespace fsrs
