#pragma once

#include <cstdint>

struct EspHostStub {
  uint32_t getFreeHeap() const { return UINT32_MAX; }
  // This fork gates rule inserts on the largest contiguous block, not just free heap.
  uint32_t getMaxAllocHeap() const { return maxAllocHeap; }
  uint32_t maxAllocHeap = UINT32_MAX;
};

inline EspHostStub ESP;
