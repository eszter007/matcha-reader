#pragma once
// Host stand-in for the SD-backed HalStorage, sufficient for DictIndex: the same call surface,
// backed by real files under a per-process root directory. Mirrors the signatures DictIndex
// depends on (int read(), size_t size(), bool seek()/isOpen()/close()).
#include <sys/stat.h>

#include <cstdint>
#include <cstdio>
#include <string>

// Settable because ctest runs each test as its own process; the fixture points this at a
// unique temp directory holding the synthetic dictionary it built.
inline std::string& testRoot() {
  static std::string root = "sdroot";
  return root;
}
inline std::string testRootPath(const char* p) { return testRoot() + p; }

class HalFile {
  FILE* f = nullptr;

 public:
  HalFile() = default;
  explicit HalFile(FILE* h) : f(h) {}
  HalFile(HalFile&& o) noexcept : f(o.f) { o.f = nullptr; }
  HalFile& operator=(HalFile&& o) noexcept {
    if (f) fclose(f);
    f = o.f;
    o.f = nullptr;
    return *this;
  }
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;
  ~HalFile() { close(); }

  bool isOpen() const { return f != nullptr; }

  size_t size() {
    if (!f) return 0;
    const long cur = ftell(f);
    fseek(f, 0, SEEK_END);
    const long end = ftell(f);
    fseek(f, cur, SEEK_SET);
    return end < 0 ? 0 : static_cast<size_t>(end);
  }

  bool seek(size_t pos) { return f && fseek(f, static_cast<long>(pos), SEEK_SET) == 0; }

  int read(void* b, size_t n) { return f ? static_cast<int>(fread(b, 1, n, f)) : -1; }

  size_t write(const void* b, size_t n) { return f ? fwrite(b, 1, n, f) : 0; }

  bool close() {
    if (f) fclose(f);
    f = nullptr;
    return true;
  }
};

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage inst;
    return inst;
  }

  bool exists(const char* p) {
    struct stat s{};
    return stat(testRootPath(p).c_str(), &s) == 0;
  }
  bool mkdir(const char* p, bool = true) {
    std::string s = testRootPath(p), acc;
    for (const char c : s) {
      acc += c;
      if (c == '/') ::mkdir(acc.c_str(), 0755);
    }
    ::mkdir(s.c_str(), 0755);
    return true;
  }
  bool remove(const char* p) { return ::remove(testRootPath(p).c_str()) == 0; }
  bool openFileForRead(const char*, const char* p, HalFile& out) {
    FILE* h = fopen(testRootPath(p).c_str(), "rb");
    if (!h) return false;
    out = HalFile(h);
    return true;
  }
  bool openFileForWrite(const char*, const char* p, HalFile& out) {
    FILE* h = fopen(testRootPath(p).c_str(), "wb");
    if (!h) return false;
    out = HalFile(h);
    return true;
  }
};

#define Storage HalStorage::getInstance()

// DictIndex gates large definition allocations on the ESP heap introspection API; the host heap
// is effectively unlimited, so report a block big enough that nothing is ever skipped.
struct EspStubClass {
  unsigned getMaxAllocHeap() const { return 0x7FFFFFFFu; }
  unsigned getFreeHeap() const { return 0x7FFFFFFFu; }
};
inline EspStubClass ESP;
