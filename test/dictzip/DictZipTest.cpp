#include <InflateReader.h>
#include <zlib.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "util/DictZip.h"

// Every allocation is counted, so the test can check what an extraction holds at once.
namespace {
size_t g_live = 0;
size_t g_peak = 0;
void* take(const size_t n) {
  auto* p = static_cast<size_t*>(std::malloc(n + 16));
  if (!p) return nullptr;
  *p = n;
  g_live += n;
  if (g_live > g_peak) g_peak = g_live;
  return reinterpret_cast<char*>(p) + 16;
}
void give(void* q) {
  if (!q) return;
  auto* p = reinterpret_cast<size_t*>(static_cast<char*>(q) - 16);
  g_live -= *p;
  std::free(p);
}
}  // namespace
void* operator new(size_t n) {
  void* p = take(n);
  if (!p) std::abort();
  return p;
}
void* operator new[](size_t n) { return operator new(n); }
void* operator new(size_t n, const std::nothrow_t&) noexcept { return take(n); }
void* operator new[](size_t n, const std::nothrow_t&) noexcept { return take(n); }
void operator delete(void* p) noexcept { give(p); }
void operator delete[](void* p) noexcept { give(p); }
void operator delete(void* p, size_t) noexcept { give(p); }
void operator delete[](void* p, size_t) noexcept { give(p); }

// tinflate.c references these for zlib/gzip checksums; raw deflate never calls them.
extern "C" uint32_t uzlib_adler32(const void*, unsigned int, uint32_t) { return 0; }
extern "C" uint32_t uzlib_crc32(const void*, unsigned int, uint32_t) { return 0; }

namespace {
// A dictzip file as dictzip(1) writes it: one raw deflate stream, fully flushed at every chunk
// boundary, the compressed size of each chunk in the gzip header's RA field.
std::vector<uint8_t> makeDictZip(const std::string& data, const uint16_t chunkLen) {
  z_stream z{};
  assert(deflateInit2(&z, 9, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) == Z_OK);
  std::vector<uint8_t> body;
  std::vector<uint16_t> sizes;
  for (size_t at = 0; at < data.size(); at += chunkLen) {
    const size_t n = std::min<size_t>(chunkLen, data.size() - at);
    uint8_t out[70000];
    z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data() + at));
    z.avail_in = static_cast<uInt>(n);
    z.next_out = out;
    z.avail_out = sizeof(out);
    assert(deflate(&z, at + n == data.size() ? Z_FINISH : Z_FULL_FLUSH) != Z_STREAM_ERROR);
    const size_t produced = sizeof(out) - z.avail_out;
    body.insert(body.end(), out, out + produced);
    sizes.push_back(static_cast<uint16_t>(produced));
  }
  deflateEnd(&z);

  const auto le16 = [](std::vector<uint8_t>& v, const uint32_t x) {
    v.push_back(x & 0xFF);
    v.push_back((x >> 8) & 0xFF);
  };
  std::vector<uint8_t> f = {0x1f, 0x8b, 8, 0x04, 0, 0, 0, 0, 0, 3};
  const uint16_t subLen = static_cast<uint16_t>(6 + sizes.size() * 2);
  le16(f, 4 + subLen);
  f.push_back('R');
  f.push_back('A');
  le16(f, subLen);
  le16(f, 1);
  le16(f, chunkLen);
  le16(f, static_cast<uint16_t>(sizes.size()));
  for (const uint16_t s : sizes) le16(f, s);
  f.insert(f.end(), body.begin(), body.end());
  const uint32_t crc = crc32(0, reinterpret_cast<const Bytef*>(data.data()), static_cast<uInt>(data.size()));
  le16(f, crc & 0xFFFF);
  le16(f, crc >> 16);
  le16(f, data.size() & 0xFFFF);
  le16(f, static_cast<uint32_t>(data.size()) >> 16);
  return f;
}

std::string extract(const char* path, const uint32_t offset, const uint32_t size) {
  const char* outPath = "dictzip_test_out.bin";
  HalFile out;
  out.attach(fopen(outPath, "wb"));
  DictZip::ExtractError err = DictZip::ExtractError::None;
  const bool ok = DictZip::extractEntry(path, offset, size, out, &err);
  out.close();
  if (!ok) return "<error " + std::to_string(static_cast<int>(err)) + ">";
  std::string got(size, '\0');
  FILE* f = fopen(outPath, "rb");
  got.resize(fread(&got[0], 1, size, f));
  fclose(f);
  remove(outPath);
  return got;
}
}  // namespace

int main(int argc, char** argv) {
  // 150 chunks of 1000 bytes: past the 64 sizes one table batch holds.
  std::string data;
  for (int i = 0; data.size() < 149500; i++) data += "entry " + std::to_string(i * 7919 % 100003) + "; ";
  const auto dz = makeDictZip(data, 1000);
  const char* path = "dictzip_test.dict.dz";
  FILE* f = fopen(path, "wb");
  fwrite(dz.data(), 1, dz.size(), f);
  fclose(f);

  const struct {
    uint32_t offset, size;
  } ranges[] = {{0, 10},       {990, 30},      {64000, 500},
                {70123, 3000}, {148000, 1500}, {static_cast<uint32_t>(data.size() - 1), 1}};
  for (const auto& r : ranges) assert(extract(path, r.offset, r.size) == data.substr(r.offset, r.size));
  assert(extract(path, static_cast<uint32_t>(data.size()), 1).rfind("<error", 0) == 0);  // past the end
  remove(path);

  // 4800 chunks of 100 bytes: a chunk table of ~19 KB, which must stay on the card. An extraction
  // holds the 32 KB inflate window and small buffers, nothing that grows with the table.
  std::string many;
  for (int i = 0; many.size() < 480000; i++) many += std::to_string(i * 104729 % 999983) + ",";
  many.resize(480000);
  const auto manyDz = makeDictZip(many, 100);
  f = fopen(path, "wb");
  fwrite(manyDz.data(), 1, manyDz.size(), f);
  fclose(f);
  const std::string want = many.substr(470050, 400);  // near the end: sums most of the table
  const size_t before = g_live;
  g_peak = g_live;
  HalFile out;
  out.attach(fopen("/dev/null", "wb"));
  assert(DictZip::extractEntry(path, 470050, 400, out));
  out.close();
  assert(g_peak - before <= InflateReader::RING_BYTES + 4096);
  assert(extract(path, 470050, 400) == want);
  remove(path);

  // A real dictionary when given: <file.dict.dz> <file.dict> <offset> <size>.
  if (argc == 5) {
    const uint32_t offset = static_cast<uint32_t>(strtoul(argv[3], nullptr, 10));
    const uint32_t size = static_cast<uint32_t>(strtoul(argv[4], nullptr, 10));
    std::string want(size, '\0');
    FILE* plain = fopen(argv[2], "rb");
    fseek(plain, offset, SEEK_SET);
    want.resize(fread(&want[0], 1, size, plain));
    fclose(plain);
    assert(extract(argv[1], offset, size) == want);
    puts("real dictionary entry matches");
  }
  puts("dictzip tests passed");
  return 0;
}
