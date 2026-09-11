#pragma once
// Host stand-in for the SD-backed HalStorage. FsHelpers.h pulls this in for hasContent();
// these tests only exercise the pure path helpers, so the call surface is all that is needed.
#include <sys/stat.h>

#include <cstdint>
#include <cstdio>
#include <string>

class HalFile {
  FILE* f = nullptr;

 public:
  HalFile() = default;
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;
  ~HalFile() { close(); }

  void adopt(FILE* h) {
    close();
    f = h;
  }
  size_t size() {
    if (!f) return 0;
    const long cur = ftell(f);
    fseek(f, 0, SEEK_END);
    const long end = ftell(f);
    fseek(f, cur, SEEK_SET);
    return end > 0 ? static_cast<size_t>(end) : 0;
  }
  int read(uint8_t* buf, size_t count) {
    if (!f) return -1;
    return static_cast<int>(fread(buf, 1, count, f));
  }
  void close() {
    if (f) fclose(f);
    f = nullptr;
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
    return stat(p, &s) == 0;
  }
  bool openFileForRead(const char* mod, const char* path, HalFile& file) {
    return openFileForRead(mod, std::string(path), file);
  }
  bool openFileForRead(const char*, const std::string& path, HalFile& file) {
    FILE* h = fopen(path.c_str(), "rb");
    if (h == nullptr) return false;
    file.adopt(h);
    return true;
  }
};

#define Storage HalStorage::getInstance()
