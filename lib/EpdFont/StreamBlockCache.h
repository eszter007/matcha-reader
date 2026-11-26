#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>

// A read cache of fixed-size file blocks for a streamed font file. 4-way set associative with a
// per-set round-robin victim. The caller owns the storage (PSRAM on boards that have it) and
// passes it to init(); bytesFor() says how much a given set count needs.
//
// A block read that comes back short is not cached: it may be a transient SD error, and a short
// block stored as the truth would serve wrong outline bytes until the font is reloaded.
class StreamBlockCache {
 public:
  using ReadFn = unsigned long (*)(void* ctx, unsigned long offset, unsigned char* buffer, unsigned long count);

  static constexpr uint32_t BLOCK_BYTES = 2048;
  static constexpr uint32_t WAYS = 4;

  static constexpr size_t bytesFor(const size_t sets) { return sets * WAYS * (BLOCK_BYTES + sizeof(uint32_t)) + sets; }

  // `storage` must hold bytesFor(sets) bytes and outlive the cache. `sets` must be a power of two.
  void init(uint8_t* storage, size_t sets);
  bool active() const { return sets_ != 0; }

  // Reads [offset, offset + count) of a file of `fileSize` bytes, from the cache where it can.
  unsigned long read(ReadFn fn, void* ctx, unsigned long fileSize, unsigned long offset, unsigned char* out,
                     unsigned long count);

  uint32_t hits() const { return hits_; }
  uint32_t misses() const { return misses_; }

 private:
  // The slot holding `block`, loading it on a miss; nullptr when the load came back short.
  // `validBytes` is the block's length (shorter for the file's last block).
  const uint8_t* blockData(ReadFn fn, void* ctx, unsigned long fileSize, uint32_t block, uint32_t* validBytes);

  uint8_t* data_ = nullptr;    // sets * WAYS blocks
  uint32_t* tags_ = nullptr;   // block index + 1 per slot, 0 = empty
  uint8_t* victim_ = nullptr;  // next way to evict, per set
  size_t sets_ = 0;
  uint32_t hits_ = 0;
  uint32_t misses_ = 0;
  std::mutex mutex_;
};
