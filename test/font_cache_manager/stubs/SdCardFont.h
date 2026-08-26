#pragma once

#include <cstdint>
#include <cstdio>

class SdCardFont {
 public:
  struct PrewarmCall {
    char text[32] = {};
    uint8_t styleMask = 0;
  };

  void clearCache() {}
  void releaseResidentCaches() {}
  // Advance tables the emergency reclaim path surrenders; nothing to hold here.
  void clearPersistentCache() {}
  // Covered by default, so the JP-fallback prewarm finds nothing missing.
  bool coversCodepoint(uint32_t, uint8_t = 0) const { return coversAll; }
  int prewarm(const char* text, uint8_t styleMask) {
    auto& call = prewarmCalls[prewarmCallCount++];
    std::snprintf(call.text, sizeof(call.text), "%s", text);
    call.styleMask = styleMask;
    return 0;
  }
  uint8_t resolveStyle(uint8_t style) const { return resolvedStyles[style & 0x03]; }
  void logStats(const char*) {}
  void resetStats() {}

  PrewarmCall prewarmCalls[4] = {};
  int prewarmCallCount = 0;
  uint8_t resolvedStyles[4] = {0, 1, 2, 3};
  bool coversAll = true;
};
