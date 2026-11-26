#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>

// Host PSRAM: plain malloc.
struct HalMemory {
  struct PsramDeleter {
    void operator()(uint8_t* buffer) const { std::free(buffer); }
  };
  using PsramBuffer = std::unique_ptr<uint8_t[], PsramDeleter>;
  static PsramBuffer allocatePsram(const size_t bytes) {
    return PsramBuffer(static_cast<uint8_t*>(std::malloc(bytes)));
  }
};
