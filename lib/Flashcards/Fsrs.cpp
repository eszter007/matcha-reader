#include "Fsrs.h"

#include <algorithm>
#include <cmath>

namespace fsrs {
namespace {
// FSRS-5 default weights (fsrs-rs 1.x / Anki 23.10+), used until a user's own are optimized.
constexpr float W[19] = {0.40255f, 1.18385f, 3.173f, 15.69105f, 7.1949f, 0.5345f, 1.4604f, 0.0046f,  1.54575f, 0.1192f,
                         1.01925f, 1.9395f,  0.11f,  0.29605f,  2.2698f, 0.2315f, 2.9898f, 0.51655f, 0.6621f};
constexpr float DECAY = -0.5f;
constexpr float FACTOR = 19.0f / 81.0f;  // so that R(S, S) = 0.9

float clampDifficulty(const float d) { return std::clamp(d, 1.0f, 10.0f); }

float initialDifficulty(const Rating rating) {
  return W[4] - std::exp(W[5] * (static_cast<float>(rating) - 1.0f)) + 1.0f;
}

float nextDifficulty(const float d, const Rating rating) {
  const float delta = -W[6] * (static_cast<float>(rating) - 3.0f);
  const float damped = d + delta * (10.0f - d) / 9.0f;  // linear damping near the ceiling
  // Mean reversion towards the difficulty of a first "Easy", so cards cannot drift forever.
  return clampDifficulty(W[7] * initialDifficulty(Rating::Easy) + (1.0f - W[7]) * damped);
}

float recallStability(const Memory& m, const float r, const Rating rating) {
  const float hardPenalty = rating == Rating::Hard ? W[15] : 1.0f;
  const float easyBonus = rating == Rating::Easy ? W[16] : 1.0f;
  return m.stability * (std::exp(W[8]) * (11.0f - m.difficulty) * std::pow(m.stability, -W[9]) *
                            (std::exp(W[10] * (1.0f - r)) - 1.0f) * hardPenalty * easyBonus +
                        1.0f);
}

float forgetStability(const Memory& m, const float r) {
  const float s = W[11] * std::pow(m.difficulty, -W[12]) * (std::pow(m.stability + 1.0f, W[13]) - 1.0f) *
                  std::exp(W[14] * (1.0f - r));
  return std::min(s, m.stability);  // forgetting never makes a card more stable
}
}  // namespace

float retrievability(const float elapsedDays, const float stability) {
  if (stability <= 0) return 0;
  return std::pow(1.0f + FACTOR * std::max(elapsedDays, 0.0f) / stability, DECAY);
}

uint16_t intervalDays(const float stability, const Params& params) {
  const float retention = std::clamp(params.retention, 0.5f, 0.99f);
  const float days = stability / FACTOR * (std::pow(retention, 1.0f / DECAY) - 1.0f);
  const long rounded = std::lround(days);
  return static_cast<uint16_t>(std::clamp<long>(rounded, 1, std::max<long>(params.maxInterval, 1)));
}

Memory initial(const Rating rating) {
  return {W[static_cast<int>(rating) - 1], clampDifficulty(initialDifficulty(rating))};
}

Memory next(const Memory& memory, const float elapsedDays, const Rating rating) {
  if (memory.stability <= 0) return initial(rating);
  Memory out;
  out.difficulty = nextDifficulty(memory.difficulty, rating);
  if (elapsedDays < 1.0f) {
    // Same-day review: FSRS-5's short-term stability update.
    out.stability = memory.stability * std::exp(W[17] * (static_cast<float>(rating) - 3.0f + W[18]));
  } else {
    const float r = retrievability(elapsedDays, memory.stability);
    out.stability = rating == Rating::Again ? forgetStability(memory, r) : recallStability(memory, r, rating);
  }
  out.stability = std::max(out.stability, 0.01f);
  return out;
}

void previewIntervals(const Memory& memory, const bool isNew, const float elapsedDays, const Params& params,
                      uint16_t out[4]) {
  out[0] = 0;
  uint16_t days[3];
  for (int i = 0; i < 3; i++) {
    const auto rating = static_cast<Rating>(i + 2);
    const Memory m = isNew ? initial(rating) : next(memory, elapsedDays, rating);
    days[i] = intervalDays(m.stability, params);
  }
  // Keep the buttons in order, as Anki does: Hard <= Good < Easy.
  days[0] = std::min(days[0], days[1]);
  if (days[1] <= days[0]) days[1] = static_cast<uint16_t>(std::min<int>(days[0] + 1, params.maxInterval));
  if (days[2] <= days[1]) days[2] = static_cast<uint16_t>(std::min<int>(days[1] + 1, params.maxInterval));
  out[1] = days[0];
  out[2] = days[1];
  out[3] = days[2];
}

}  // namespace fsrs
