#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

// In-memory file: reads and writes go to a buffer owned by TestStorage.
class HalFile {
 public:
  explicit operator bool() const { return open_; }
  void close() {
    EXPECT_TRUE(initialized_) << "Closing a default HalFile asserts on device";
    open_ = false;
  }
  bool seek(size_t position) {
    position_ = position;
    return true;
  }
  size_t position() const { return position_; }
  int available() const { return data_ && position_ < data_->size() ? static_cast<int>(data_->size() - position_) : 0; }
  void markOpen(std::string* data) {
    initialized_ = true;
    open_ = true;
    data_ = data;
    position_ = 0;
  }
  void writeBytes(const std::string& bytes) {
    data_->replace(position_, std::min(bytes.size(), data_->size() - position_), bytes);
    position_ += bytes.size();
  }
  bool readBytes(char* out, const size_t size) {
    if (!data_ || position_ + size > data_->size()) return false;
    data_->copy(out, size, position_);
    position_ += size;
    return true;
  }

 private:
  bool open_ = false;
  bool initialized_ = false;
  size_t position_ = 0;
  std::string* data_ = nullptr;
};

struct TestStorage {
  int writeOpens = 0;
  int readOpens = 0;
  std::map<std::string, std::string> files;

  bool openFileForWrite(const char*, const std::string& path, HalFile& file) {
    writeOpens++;
    files[path].clear();
    file.markOpen(&files[path]);
    return true;
  }
  bool openFileForRead(const char*, const std::string& path, HalFile& file) {
    readOpens++;
    file.markOpen(&files[path]);
    return true;
  }
  bool exists(const char*) const { return false; }
  bool remove(const char*) { return true; }
};

inline TestStorage Storage;
