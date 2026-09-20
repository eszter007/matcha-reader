#pragma once

#include <cstdint>
#include <cstdio>

class SdCardFont {
 public:
  struct PrewarmCall {
    char text[32] = {};
    uint8_t styleMask = 0;
    bool accumulate = true;
  };

  void clearCache() {}
  void releaseResidentCaches() {}
  // Advance tables the emergency reclaim path surrenders; nothing to hold here.
  void clearPersistentCache() {}
  // Covered by default, so the JP-fallback prewarm finds nothing missing.
  bool coversCodepoint(uint32_t, uint8_t = 0) const { return coversAll; }
  // Defaults mirror the real SdCardFont::prewarm, which callers rely on (the fallback-font
  // prewarm passes only text + styleMask).
  int prewarm(const char* text, uint8_t styleMask = 0x0F, bool = false, bool = true, bool accumulate = true) {
    auto& call = prewarmCalls[prewarmCallCount++];
    std::snprintf(call.text, sizeof(call.text), "%s", text);
    call.styleMask = styleMask;
    call.accumulate = accumulate;
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
