#pragma once
#include <cstdint>
#include <cstring>

inline uint32_t millis() { return 0; }
inline uint32_t micros() { return 0; }

struct EspHostStub {
  uint32_t maxAllocHeap = UINT32_MAX;
  uint32_t getMaxAllocHeap() const { return maxAllocHeap; }
};
inline EspHostStub ESP;
