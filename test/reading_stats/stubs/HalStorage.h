#pragma once
// Host stand-in for the SD-backed HalStorage, sufficient for the stats stores: the same call
// surface, backed by real files under the test's working directory.
#include <sys/stat.h>

#include <cstdio>
#include <string>

inline std::string testRootPath(const char* p) { return std::string("sdroot") + p; }

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

  size_t read(void* b, size_t n) { return f ? fread(b, 1, n, f) : 0; }
  size_t write(const void* b, size_t n) { return f ? fwrite(b, 1, n, f) : 0; }
  void close() {
    if (f) fclose(f);
    f = nullptr;
  }
};

class HalStorage {
  static HalStorage inst;

 public:
  static HalStorage& getInstance() { return inst; }

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
