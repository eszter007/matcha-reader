#pragma once
// Host stand-in: the stats store only asks the heap whether a table fits.
#include <cstdint>

struct EspStub {
  uint32_t getMaxAllocHeap() const { return UINT32_MAX / 2; }
};
inline EspStub ESP;

// The journal's nonce; distinct per call is all the tests need.
inline uint32_t esp_random() {
  static uint32_t n = 0x12345678;
  return n += 0x9E3779B9;
}
