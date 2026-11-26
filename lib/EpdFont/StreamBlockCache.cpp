#include "StreamBlockCache.h"

#include <cstring>

void StreamBlockCache::init(uint8_t* storage, const size_t sets) {
  std::lock_guard<std::mutex> lock(mutex_);
  sets_ = 0;
  data_ = nullptr;
  tags_ = nullptr;
  victim_ = nullptr;
  if (storage == nullptr || sets == 0 || (sets & (sets - 1)) != 0) return;
  data_ = storage;
  tags_ = reinterpret_cast<uint32_t*>(storage + sets * WAYS * BLOCK_BYTES);  // BLOCK_BYTES keeps it aligned
  victim_ = reinterpret_cast<uint8_t*>(tags_ + sets * WAYS);
  memset(tags_, 0, sets * WAYS * sizeof(uint32_t));
  memset(victim_, 0, sets);
  sets_ = sets;
  hits_ = misses_ = 0;
}

const uint8_t* StreamBlockCache::blockData(const ReadFn fn, void* ctx, const unsigned long fileSize,
                                           const uint32_t block, uint32_t* validBytes) {
  const unsigned long start = static_cast<unsigned long>(block) * BLOCK_BYTES;
  const unsigned long want = fileSize - start < BLOCK_BYTES ? fileSize - start : BLOCK_BYTES;
  *validBytes = static_cast<uint32_t>(want);
  const size_t set = block & (sets_ - 1);
  uint32_t* tags = tags_ + set * WAYS;
  for (uint32_t w = 0; w < WAYS; ++w) {
    if (tags[w] == block + 1) {
      ++hits_;
      return data_ + (set * WAYS + w) * BLOCK_BYTES;
    }
  }
  ++misses_;
  const uint32_t way = victim_[set];
  uint8_t* slot = data_ + (set * WAYS + way) * BLOCK_BYTES;
  tags[way] = 0;  // the slot is being overwritten
  if (fn(ctx, start, slot, want) != want) return nullptr;
  tags[way] = block + 1;
  victim_[set] = static_cast<uint8_t>((way + 1) % WAYS);
  return slot;
}

unsigned long StreamBlockCache::read(const ReadFn fn, void* ctx, const unsigned long fileSize,
                                     const unsigned long offset, unsigned char* out, const unsigned long count) {
  // Large reads (a whole table) gain nothing from block caching and would evict the glyph working set.
  if (count == 0 || count > 2 * BLOCK_BYTES || offset >= fileSize) return fn(ctx, offset, out, count);
  std::lock_guard<std::mutex> lock(mutex_);
  // Checked under the lock: init(nullptr, 0) clears the cache before its storage is freed.
  if (sets_ == 0 || data_ == nullptr) return fn(ctx, offset, out, count);
  const unsigned long end = offset + count > fileSize ? fileSize : offset + count;
  unsigned long pos = offset;
  while (pos < end) {
    const uint32_t block = static_cast<uint32_t>(pos / BLOCK_BYTES);
    uint32_t valid = 0;
    const uint8_t* data = blockData(fn, ctx, fileSize, block, &valid);
    if (data == nullptr) {
      // Short block read: serve the rest straight from the file.
      return (pos - offset) + fn(ctx, pos, out + (pos - offset), count - (pos - offset));
    }
    const unsigned long inBlock = pos - static_cast<unsigned long>(block) * BLOCK_BYTES;
    unsigned long n = valid - inBlock;
    if (n > end - pos) n = end - pos;
    memcpy(out + (pos - offset), data + inBlock, n);
    pos += n;
  }
  return pos - offset;
}
