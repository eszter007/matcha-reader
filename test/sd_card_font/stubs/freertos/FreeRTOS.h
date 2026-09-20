#pragma once

// Host-test stub. The fork's SdCardFont guards its font-file handle with a FreeRTOS mutex
// (see FontFileLock); the host tests are single-threaded, so the primitives are no-ops that
// keep the lock's shape without needing a scheduler.

#include <cstdint>

using SemaphoreHandle_t = void*;

constexpr uint32_t portMAX_DELAY = 0xFFFFFFFFu;

inline SemaphoreHandle_t xSemaphoreCreateMutex() {
  // Non-null so FontFileLock takes its normal path rather than the null-mutex fallback.
  static int token = 0;
  return &token;
}
inline bool xSemaphoreTake(SemaphoreHandle_t, uint32_t) { return true; }
inline bool xSemaphoreGive(SemaphoreHandle_t) { return true; }
inline void vSemaphoreDelete(SemaphoreHandle_t) {}
