#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

// Dev-only SD-read instrumentation: each read()/seek() is one SD transaction on device, so
// these counters approximate on-device I/O cost. Reset/read from the benchmark harness.
namespace halstub {
inline long g_reads = 0;
inline long g_seeks = 0;
}  // namespace halstub

// Minimal HalFile stub for host testing — wraps stdio FILE*.
class HalFile {
  FILE* f_ = nullptr;

 public:
  HalFile() = default;
  ~HalFile() { close(); }
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  bool isOpen() const { return f_ != nullptr; }
  void close() {
    if (f_) {
      std::fclose(f_);
      f_ = nullptr;
    }
  }

  size_t size() const {
    if (!f_) return 0;
    long cur = std::ftell(f_);
    std::fseek(f_, 0, SEEK_END);
    long sz = std::ftell(f_);
    std::fseek(f_, cur, SEEK_SET);
    return static_cast<size_t>(sz);
  }

  bool seek(size_t pos) {
    halstub::g_seeks++;
    return f_ && std::fseek(f_, static_cast<long>(pos), SEEK_SET) == 0;
  }

  size_t read(uint8_t* buf, size_t len) {
    if (!f_) return 0;
    halstub::g_reads++;
    return std::fread(buf, 1, len, f_);
  }

  size_t write(const uint8_t* buf, size_t len) {
    if (!f_) return 0;
    return std::fwrite(buf, 1, len, f_);
  }

  int available() const { return f_ ? !std::feof(f_) : 0; }

  friend class HalStorage;
};

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage instance;
    return instance;
  }

  // Dev-only: prepend a root so DictIndex's absolute "/dict/..." paths resolve under a real
  // sdcard/ directory on the host. Set from CP_SD_ROOT (see spx_verify.cpp).
  std::string root_;
  std::string resolve(const std::string& path) const { return root_ + path; }

  bool exists(const char* path) const {
    FILE* f = std::fopen(resolve(path).c_str(), "rb");
    if (f) {
      std::fclose(f);
      return true;
    }
    return false;
  }

  bool openFileForRead(const char* /*tag*/, const std::string& path, HalFile& file) {
    file.close();
    file.f_ = std::fopen(resolve(path).c_str(), "rb");
    return file.f_ != nullptr;
  }

  bool openFileForWrite(const char* /*tag*/, const std::string& path, HalFile& file) {
    file.close();
    file.f_ = std::fopen(resolve(path).c_str(), "wb");
    return file.f_ != nullptr;
  }

  bool remove(const char* path) { return std::remove(path) == 0; }
  bool mkdir(const char*) { return true; }
};

#define Storage HalStorage::getInstance()

// Minimal ESP heap-introspection stub (on device this arrives via the Arduino core through the
// real HalStorage.h). Host heap is effectively unlimited, so report a huge block.
struct EspStubClass {
  unsigned getMaxAllocHeap() const { return 0x7FFFFFFF; }
  unsigned getFreeHeap() const { return 0x7FFFFFFF; }
};
inline EspStubClass ESP;
