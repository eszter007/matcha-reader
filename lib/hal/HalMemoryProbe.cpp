#include "HalMemoryProbe.h"

#include <Arduino.h>
#include <HalStorage.h>

#include <cstdio>
#include <cstring>

namespace HalMemoryProbe {

namespace {
constexpr uint8_t MAX_SAMPLES = 16;
struct Sample {
  char label[28];
  uint32_t freeHeap;
  uint32_t maxAlloc;
  uint32_t atMs;
};
Sample samples[MAX_SAMPLES];
uint8_t sampleCount = 0;
}  // namespace

void sample(const char* label) {
  if (sampleCount >= MAX_SAMPLES) return;
  Sample& s = samples[sampleCount++];
  strncpy(s.label, label, sizeof(s.label) - 1);
  s.label[sizeof(s.label) - 1] = '\0';
  s.freeHeap = ESP.getFreeHeap();
  s.maxAlloc = ESP.getMaxAllocHeap();
  s.atMs = millis();
}

void flush(const char* context) {
  if (sampleCount == 0) return;
  HalFile file = Storage.open("/heap-report.txt", O_WRITE | O_CREAT | O_APPEND);
  if (!file) {
    sampleCount = 0;
    return;
  }
  char line[64];
  int n = snprintf(line, sizeof(line), "--- %s ---\n", context);
  file.write(reinterpret_cast<const uint8_t*>(line), n);
  for (uint8_t i = 0; i < sampleCount; i++) {
    const Sample& s = samples[i];
    n = snprintf(line, sizeof(line), "%8u %-27s free=%-7u maxAlloc=%u\n", s.atMs, s.label, s.freeHeap, s.maxAlloc);
    file.write(reinterpret_cast<const uint8_t*>(line), n);
  }
  file.close();
  sampleCount = 0;
}

}  // namespace HalMemoryProbe
