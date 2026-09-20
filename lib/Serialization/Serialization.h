#pragma once
#include <HalStorage.h>

#include <cstring>
#include <iostream>

namespace serialization {
// Bounded RAM sink: batches many small writePod/writeString calls into one
// buffer so the caller can flush with a SINGLE file.write. Every HalFile write
// takes the storage mutex plus an SdFat call; serializing a vertical page
// field-by-field cost ~10 mutexed writes per glyph -- hundreds of thousands of
// SD transactions per chapter build. Writing past capacity sets overflow
// instead of corrupting memory; callers fall back to the direct file path.
struct BufWriter {
  uint8_t* buf;
  size_t cap;
  size_t len = 0;
  bool overflow = false;
  BufWriter(uint8_t* b, const size_t c) : buf(b), cap(c) {}
  void write(const uint8_t* p, const size_t n) {
    if (overflow || len + n > cap) {
      overflow = true;
      return;
    }
    memcpy(buf + len, p, n);
    len += n;
  }
};

template <typename T>
void writePod(BufWriter& w, const T& value) {
  w.write(reinterpret_cast<const uint8_t*>(&value), sizeof(T));
}

inline void writeString(BufWriter& w, const std::string& s) {
  const uint32_t len = s.size();
  writePod(w, len);
  w.write(reinterpret_cast<const uint8_t*>(s.data()), len);
}

template <typename T>
void writePod(std::ostream& os, const T& value) {
  os.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
void writePod(HalFile& file, const T& value) {
  file.write(reinterpret_cast<const uint8_t*>(&value), sizeof(T));
}

// Every serialized string is preceded by its length, and that length is then handed to
// std::string::resize(). A cache file truncated or misread -- an SD card failing mid-read is the
// case seen in the wild -- yields a nonsense length, and resize() on it either throws
// length_error or exhausts the heap. Under -fno-exceptions both end at abort(), taking the
// device down while it was only trying to read a book's title. So: the readers below zero their
// output before reading, report whether the read was complete, and refuse a length that cannot
// be true. No string this format stores is anywhere near this size.
constexpr uint32_t MAX_SERIALIZED_STRING = 64 * 1024;

// Returns false on a short read, having left `value` zeroed rather than holding stack garbage.
template <typename T>
bool readPod(std::istream& is, T& value) {
  value = T{};
  is.read(reinterpret_cast<char*>(&value), sizeof(T));
  return is.gcount() == static_cast<std::streamsize>(sizeof(T));
}

template <typename T>
bool readPod(HalFile& file, T& value) {
  value = T{};
  return file.read(reinterpret_cast<uint8_t*>(&value), sizeof(T)) == static_cast<int>(sizeof(T));
}

inline void writeString(std::ostream& os, const std::string& s) {
  const uint32_t len = s.size();
  writePod(os, len);
  os.write(s.data(), len);
}

inline void writeString(HalFile& file, const std::string& s) {
  const uint32_t len = s.size();
  writePod(file, len);
  file.write(reinterpret_cast<const uint8_t*>(s.data()), len);
}

inline bool readString(std::istream& is, std::string& s) {
  s.clear();
  uint32_t len = 0;
  if (!readPod(is, len) || len > MAX_SERIALIZED_STRING) return false;
  s.resize(len);
  if (len == 0) return true;
  is.read(&s[0], len);
  return is.gcount() == static_cast<std::streamsize>(len);
}

inline bool readString(HalFile& file, std::string& s) {
  s.clear();
  uint32_t len = 0;
  if (!readPod(file, len) || len > MAX_SERIALIZED_STRING) return false;
  // A length past the end of the file is corruption however plausible its size looks.
  const int remaining = file.available();
  if (remaining >= 0 && len > static_cast<uint32_t>(remaining)) return false;
  s.resize(len);
  if (len == 0) return true;
  return file.read(&s[0], len) == static_cast<int>(len);
}
}  // namespace serialization
