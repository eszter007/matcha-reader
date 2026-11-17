#pragma once

// In-memory SD card for content_cache: files and folders by full path.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace fake {
struct Node {
  bool directory = false;
  uint32_t modified = 1;
  std::vector<uint8_t> bytes;
};
inline std::map<std::string, std::shared_ptr<Node>> nodes;
inline bool failReads = false;
inline std::string failRemove;
inline void reset() {
  nodes.clear();
  failReads = false;
  failRemove.clear();
}
inline void addDir(const std::string& path) {
  auto n = std::make_shared<Node>();
  n->directory = true;
  nodes[path] = n;
}
inline void addFile(const std::string& path, const std::string& bytes) {
  auto n = std::make_shared<Node>();
  n->bytes.assign(bytes.begin(), bytes.end());
  nodes[path] = n;
}
inline bool has(const std::string& path) { return nodes.count(path) != 0; }
}  // namespace fake

class HalFile {
 public:
  HalFile() = default;
  HalFile(std::string path, std::shared_ptr<fake::Node> node) : path(std::move(path)), node(std::move(node)) {}
  explicit operator bool() const { return node != nullptr; }
  bool isOpen() const { return node != nullptr; }
  bool isDirectory() const { return node && node->directory; }
  bool close() {
    node.reset();
    return true;
  }
  size_t getName(char* name, size_t len) {
    const std::string base = path.substr(path.rfind('/') + 1);
    std::snprintf(name, len, "%s", base.c_str());
    return base.size();
  }
  uint64_t fileSize64() const { return node ? node->bytes.size() : 0; }
  uint32_t modifiedStamp() const { return node ? node->modified : 0; }
  bool seek64(uint64_t pos) {
    if (!node || pos > node->bytes.size()) return false;
    at = pos;
    return true;
  }
  int read(void* buf, size_t count) {
    if (!node || fake::failReads) return -1;
    const size_t n = std::min<size_t>(count, node->bytes.size() - at);
    std::memcpy(buf, node->bytes.data() + at, n);
    at += n;
    return static_cast<int>(n);
  }
  size_t write(const void* buf, size_t count) {
    if (!node) return 0;
    const auto* b = static_cast<const uint8_t*>(buf);
    node->bytes.insert(node->bytes.end(), b, b + count);
    return count;
  }
  HalFile openNextFile() {
    const std::string prefix = path + "/";
    for (auto it = fake::nodes.upper_bound(lastChild.empty() ? prefix : lastChild); it != fake::nodes.end(); ++it) {
      if (it->first.rfind(prefix, 0) != 0) break;
      if (it->first.find('/', prefix.size()) != std::string::npos) continue;  // grandchild
      lastChild = it->first;
      return HalFile(it->first, it->second);
    }
    return HalFile();
  }

 private:
  std::string path;
  std::shared_ptr<fake::Node> node;
  uint64_t at = 0;
  std::string lastChild;
};

class HalStorage {
 public:
  bool exists(const char* path) { return fake::has(path); }
  HalFile open(const char* path) { return fake::has(path) ? HalFile(path, fake::nodes[path]) : HalFile(); }
  bool openFileForRead(const char*, const std::string& path, HalFile& out) {
    if (!fake::has(path) || fake::nodes[path]->directory) return false;
    out = HalFile(path, fake::nodes[path]);
    return true;
  }
  bool openFileForWrite(const char*, const std::string& path, HalFile& out) {
    fake::addFile(path, "");
    out = HalFile(path, fake::nodes[path]);
    return true;
  }
  bool remove(const char* path) { return fake::nodes.erase(path) != 0; }
  bool removeDir(const char* path) {
    if (fake::failRemove == path) return false;
    const std::string prefix = std::string(path) + "/";
    for (auto it = fake::nodes.begin(); it != fake::nodes.end();) {
      it = it->first.rfind(prefix, 0) == 0 ? fake::nodes.erase(it) : std::next(it);
    }
    return fake::nodes.erase(path) != 0;
  }
};

inline HalStorage Storage;
