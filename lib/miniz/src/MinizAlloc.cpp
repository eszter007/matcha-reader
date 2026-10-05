#include <BuildScratch.h>

#include <cstdlib>

#include "MinizConfig.h"

namespace {
// Only an allocation this large is worth the lent block: miniz's inflate state (tinfl decompressor
// plus the 32KB dictionary) is the one that fails on a fragmented heap. Smaller ones stay on the
// heap so they never hold the block another consumer needs.
constexpr size_t SCRATCH_MIN_BYTES = 16 * 1024;
const void* scratchInUse = nullptr;
}  // namespace

extern "C" void* crosspoint_mz_malloc(const size_t bytes) {
  if (bytes >= SCRATCH_MIN_BYTES && !scratchInUse) {
    if (uint8_t* lent = buildscratch::claim(bytes)) {
      scratchInUse = lent;
      return lent;
    }
  }
  return malloc(bytes);
}

extern "C" void crosspoint_mz_release(void* p) {
  if (p && p == scratchInUse) {
    buildscratch::release(static_cast<const uint8_t*>(p));
    scratchInUse = nullptr;
    return;
  }
  free(p);
}

extern "C" void* crosspoint_mz_realloc(void* p, const size_t bytes) {
  // The lent block cannot grow; the inflate path never reallocates.
  if (p && p == scratchInUse) return nullptr;
  return realloc(p, bytes);
}
