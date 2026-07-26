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

template <typename T>
void readPod(std::istream& is, T& value) {
  is.read(reinterpret_cast<char*>(&value), sizeof(T));
}

template <typename T>
void readPod(HalFile& file, T& value) {
  file.read(reinterpret_cast<uint8_t*>(&value), sizeof(T));
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

inline void readString(std::istream& is, std::string& s) {
  uint32_t len;
  readPod(is, len);
  s.resize(len);
  is.read(&s[0], len);
}

inline void readString(HalFile& file, std::string& s) {
  uint32_t len;
  readPod(file, len);
  s.resize(len);
  file.read(&s[0], len);
}
}  // namespace serialization
