#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

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
    return f_ && std::fseek(f_, static_cast<long>(pos), SEEK_SET) == 0;
  }

  size_t read(uint8_t* buf, size_t len) {
    if (!f_) return 0;
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

  bool exists(const char* path) const {
    FILE* f = std::fopen(path, "rb");
    if (f) {
      std::fclose(f);
      return true;
    }
    return false;
  }

  bool openFileForRead(const char* /*tag*/, const std::string& path, HalFile& file) {
    file.close();
    file.f_ = std::fopen(path.c_str(), "rb");
    return file.f_ != nullptr;
  }

  bool openFileForWrite(const char* /*tag*/, const std::string& path, HalFile& file) {
    file.close();
    file.f_ = std::fopen(path.c_str(), "wb");
    return file.f_ != nullptr;
  }

  bool remove(const char* path) { return std::remove(path) == 0; }
  bool mkdir(const char*) { return true; }
};

#define Storage HalStorage::getInstance()
