#pragma once
// Host stand-in for HalStorage: the calls DictZip makes, on plain files.
#include <cstdint>
#include <cstdio>

class HalFile {
  FILE* f = nullptr;

 public:
  HalFile() = default;
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;
  ~HalFile() { close(); }
  void attach(FILE* h) { f = h; }
  explicit operator bool() const { return f != nullptr; }
  int read(void* b, size_t n) { return f ? static_cast<int>(fread(b, 1, n, f)) : -1; }
  int read() { return f ? fgetc(f) : -1; }
  size_t write(const void* b, size_t n) { return f ? fwrite(b, 1, n, f) : 0; }
  bool seekSet(uint64_t pos) { return f && fseek(f, static_cast<long>(pos), SEEK_SET) == 0; }
  uint64_t position() const { return f ? static_cast<uint64_t>(ftell(f)) : 0; }
  uint64_t fileSize() {
    const long pos = ftell(f);
    fseek(f, 0, SEEK_END);
    const long end = ftell(f);
    fseek(f, pos, SEEK_SET);
    return static_cast<uint64_t>(end);
  }
  bool close() {
    const bool ok = !f || fclose(f) == 0;
    f = nullptr;
    return ok;
  }
};

struct HalStorage {
  bool openFileForRead(const char*, const char* path, HalFile& file) {
    file.attach(fopen(path, "rb"));
    return static_cast<bool>(file);
  }
};
inline HalStorage Storage;
