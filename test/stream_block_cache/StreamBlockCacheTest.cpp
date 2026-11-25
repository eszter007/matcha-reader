#include "StreamBlockCache.h"

#include <gtest/gtest.h>

#include <cstring>
#include <random>
#include <vector>

namespace {

struct FakeFile {
  std::vector<unsigned char> bytes;
  unsigned reads = 0;
  bool failNext = false;
  static unsigned long read(void* ctx, const unsigned long offset, unsigned char* out, const unsigned long count) {
    auto* f = static_cast<FakeFile*>(ctx);
    ++f->reads;
    if (f->failNext) {
      f->failNext = false;
      return 0;
    }
    if (offset >= f->bytes.size()) return 0;
    const unsigned long n = std::min<unsigned long>(count, f->bytes.size() - offset);
    memcpy(out, f->bytes.data() + offset, n);
    return n;
  }
};

FakeFile makeFile(const size_t size) {
  FakeFile f;
  f.bytes.resize(size);
  for (size_t i = 0; i < size; ++i) f.bytes[i] = static_cast<unsigned char>(i * 31 + (i >> 8));
  return f;
}

TEST(StreamBlockCacheTest, ServesTheSameBytesAsTheFile) {
  FakeFile file = makeFile(100 * 1024 + 123);  // last block is partial
  std::vector<uint8_t> storage(StreamBlockCache::bytesFor(4));
  StreamBlockCache cache;
  cache.init(storage.data(), 4);
  std::mt19937 rng(7);
  for (int i = 0; i < 2000; ++i) {
    const unsigned long off = rng() % file.bytes.size();
    const unsigned long len = 1 + rng() % 3000;
    std::vector<unsigned char> got(len, 0xEE), want(len, 0xEE);
    const auto n = cache.read(&FakeFile::read, &file, file.bytes.size(), off, got.data(), len);
    const auto m = FakeFile::read(&file, off, want.data(), len);
    ASSERT_EQ(n, m) << "offset " << off << " length " << len;
    ASSERT_EQ(0, memcmp(got.data(), want.data(), n)) << "offset " << off << " length " << len;
  }
  EXPECT_GT(cache.hits(), 0u);
}

TEST(StreamBlockCacheTest, ARepeatedReadDoesNotGoBackToTheFile) {
  FakeFile file = makeFile(64 * 1024);
  std::vector<uint8_t> storage(StreamBlockCache::bytesFor(8));
  StreamBlockCache cache;
  cache.init(storage.data(), 8);
  unsigned char buf[300];
  cache.read(&FakeFile::read, &file, file.bytes.size(), 5000, buf, sizeof(buf));
  const unsigned readsAfterFirst = file.reads;
  cache.read(&FakeFile::read, &file, file.bytes.size(), 5100, buf, sizeof(buf));
  EXPECT_EQ(file.reads, readsAfterFirst);
}

TEST(StreamBlockCacheTest, AShortBlockReadIsNotCached) {
  FakeFile file = makeFile(64 * 1024);
  std::vector<uint8_t> storage(StreamBlockCache::bytesFor(8));
  StreamBlockCache cache;
  cache.init(storage.data(), 8);
  unsigned char buf[100];
  file.failNext = true;
  EXPECT_EQ(cache.read(&FakeFile::read, &file, file.bytes.size(), 9000, buf, sizeof(buf)), sizeof(buf));
  EXPECT_EQ(0, memcmp(buf, file.bytes.data() + 9000, sizeof(buf)));
  const unsigned before = file.reads;
  cache.read(&FakeFile::read, &file, file.bytes.size(), 9000, buf, sizeof(buf));
  EXPECT_GT(file.reads, before) << "the failed block was served from the cache";
  EXPECT_EQ(0, memcmp(buf, file.bytes.data() + 9000, sizeof(buf)));
}

TEST(StreamBlockCacheTest, InactiveCachePassesThrough) {
  FakeFile file = makeFile(8 * 1024);
  StreamBlockCache cache;
  unsigned char buf[16];
  EXPECT_EQ(cache.read(&FakeFile::read, &file, file.bytes.size(), 10, buf, sizeof(buf)), sizeof(buf));
  EXPECT_EQ(0, memcmp(buf, file.bytes.data() + 10, sizeof(buf)));
}

}  // namespace
