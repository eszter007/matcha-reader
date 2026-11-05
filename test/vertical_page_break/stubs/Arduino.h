#pragma once

#include <cstdint>

struct EspHostStub {
  uint32_t getMaxAllocHeap() const { return UINT32_MAX; }
};

inline EspHostStub ESP;
