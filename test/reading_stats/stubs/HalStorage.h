#pragma once
// Host stand-in for the SD-backed HalStorage, sufficient for the stats stores: the same call
// surface, backed by real files under the test's working directory.
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

// Settable because ctest runs each test as its own parallel process from a shared working
// directory; a fixed root would have them deleting each other's files mid-run.
inline std::string& testRoot() {
  static std::string root = "sdroot";
  return root;
}
inline std::string testRootPath(const char* p) { return testRoot() + p; }

class HalFile {
  FILE* f = nullptr;
  DIR* d = nullptr;  // a folder opened with HalStorage::open()
  std::string dirPath;

 public:
  HalFile() = default;
  explicit HalFile(FILE* h) : f(h) {}
  HalFile(DIR* dir, std::string path) : d(dir), dirPath(std::move(path)) {}
  HalFile(HalFile&& o) noexcept : f(o.f), d(o.d), dirPath(std::move(o.dirPath)) {
    o.f = nullptr;
    o.d = nullptr;
  }
  HalFile& operator=(HalFile&& o) noexcept {
    close();
    f = o.f;
    d = o.d;
    dirPath = std::move(o.dirPath);
    o.f = nullptr;
    o.d = nullptr;
    return *this;
  }
  explicit operator bool() const { return f || d; }
  bool isDirectory() const { return d != nullptr; }
  // The next file in an opened folder, opened for reading (sub-folders come back as folders).
  HalFile openNextFile() {
    if (!d) return HalFile();
    for (const dirent* e = readdir(d); e; e = readdir(d)) {
      const std::string name = e->d_name;
      if (name == "." || name == "..") continue;
      const std::string full = dirPath + "/" + name;
      if (DIR* sub = opendir(full.c_str())) return HalFile(sub, full);
      if (FILE* h = fopen(full.c_str(), "rb")) return HalFile(h);
    }
    return HalFile();
  }
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;
  ~HalFile() { close(); }

  size_t read(void* b, size_t n) { return f ? fread(b, 1, n, f) : 0; }
  size_t write(const void* b, size_t n) { return f ? fwrite(b, 1, n, f) : 0; }
  size_t size() {
    if (!f) return 0;
    const long pos = ftell(f);
    fseek(f, 0, SEEK_END);
    const long end = ftell(f);
    fseek(f, pos, SEEK_SET);
    return static_cast<size_t>(end);
  }
  bool close() {
    const bool ok = !f || fclose(f) == 0;
    f = nullptr;
    if (d) closedir(d);
    d = nullptr;
    return ok;
  }
  bool seek(size_t pos) { return f && fseek(f, static_cast<long>(pos), SEEK_SET) == 0; }
  bool seekCur(int64_t off) { return f && fseek(f, static_cast<long>(off), SEEK_CUR) == 0; }
  size_t position() const { return f ? static_cast<size_t>(ftell(f)) : 0; }
  bool truncate(uint64_t n) { return f && fflush(f) == 0 && ftruncate(fileno(f), static_cast<off_t>(n)) == 0; }
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
  HalFile open(const char* p) {
    const std::string full = testRootPath(p);
    if (DIR* dir = opendir(full.c_str())) return HalFile(dir, full);
    if (FILE* h = fopen(full.c_str(), "rb")) return HalFile(h);
    return HalFile();
  }
  bool rename(const char* from, const char* to) {
    return ::rename(testRootPath(from).c_str(), testRootPath(to).c_str()) == 0;
  }
  bool replaceFile(const char* tmp, const char* to) {
    ::remove(testRootPath(to).c_str());
    return rename(tmp, to);
  }
  bool openFileForAppend(const char*, const char* p, HalFile& out) {
    FILE* h = fopen(testRootPath(p).c_str(), "ab");
    if (!h) return false;
    out = HalFile(h);
    return true;
  }
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
