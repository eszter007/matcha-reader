#include "FinishedIndex.h"

#include <HalMemory.h>
#include <HalStorage.h>
#include <KOReaderCredentialStore.h>
#include <KOReaderDocumentId.h>
#include <Logging.h>
#include <MD5Builder.h>
#include <SdSystemDir.h>

#include <algorithm>
#include <cstring>

namespace finishedIndex {
namespace {
// Version, account key, cursor, count, then the sorted keys.
constexpr uint8_t VERSION = 1;
constexpr size_t HEADER_BYTES = 1 + 4 + 8 + 2;

std::string indexPath() { return sdsystem::path("finished.idx"); }

bool fits(const size_t count) {
  return HalMemory::getDefaultHeap().largestBlockBytes >= count * sizeof(uint32_t) + 1024;
}

// Opens the index past its header. False when there is none or it is not one this firmware wrote.
bool openIndex(HalFile& f, uint32_t& account, uint64_t& cursor, uint16_t& count) {
  if (!Storage.exists(indexPath().c_str()) || !Storage.openFileForRead("FIN", indexPath().c_str(), f)) return false;
  uint8_t version = 0;
  if (f.read(&version, 1) != 1 || version != VERSION || f.read(reinterpret_cast<uint8_t*>(&account), 4) != 4 ||
      f.read(reinterpret_cast<uint8_t*>(&cursor), 8) != 8 || f.read(reinterpret_cast<uint8_t*>(&count), 2) != 2 ||
      count > MAX_ENTRIES || f.size() != HEADER_BYTES + count * sizeof(uint32_t)) {
    LOG_ERR("FIN", "Finished index unreadable");
    return false;
  }
  return true;
}

// The whole index with room for `extra` more keys, in one block; false (with `keys` empty) when it is
// missing, damaged or does not fit.
bool readIndex(uint32_t& account, uint64_t& cursor, std::vector<uint32_t>& keys, const size_t extra = 0) {
  keys.clear();
  account = 0;
  cursor = 0;
  HalFile f;
  uint16_t count = 0;
  if (!openIndex(f, account, cursor, count)) return false;
  const size_t room = std::min(MAX_ENTRIES, count + extra);
  // reserve() aborts on failure under -fno-exceptions, so check the block first.
  if (!fits(room)) {
    LOG_ERR("FIN", "Not enough heap for %u finished books", count);
    return false;
  }
  keys.reserve(room);
  keys.resize(count);
  if (count && f.read(reinterpret_cast<uint8_t*>(keys.data()), count * sizeof(uint32_t)) !=
                   static_cast<int>(count * sizeof(uint32_t))) {
    keys.clear();
    return false;
  }
  return true;
}

// Through a temp file, so an interrupted write leaves the old index whole.
bool writeIndex(const uint32_t account, const uint64_t cursor, const std::vector<uint32_t>& keys) {
  const std::string path = indexPath();
  const std::string tmp = path + ".tmp";
  {
    HalFile f;
    if (!Storage.openFileForWrite("FIN", tmp, f)) return false;
    const auto count = static_cast<uint16_t>(keys.size());
    if (f.write(&VERSION, 1) != 1 || f.write(reinterpret_cast<const uint8_t*>(&account), 4) != 4 ||
        f.write(reinterpret_cast<const uint8_t*>(&cursor), 8) != 8 ||
        f.write(reinterpret_cast<const uint8_t*>(&count), 2) != 2 ||
        (count && f.write(reinterpret_cast<const uint8_t*>(keys.data()), count * sizeof(uint32_t)) !=
                      count * sizeof(uint32_t))) {
      LOG_ERR("FIN", "Finished index not written");
      return false;
    }
  }
  Storage.remove(path.c_str());
  return Storage.rename(tmp.c_str(), path.c_str());
}

uint32_t keyOfMd5(const std::string& hash) { return keyOf(hash.data(), hash.size()); }

// Same hash as KOReaderDocumentId::calculateFromFilename(), without its log line per book.
uint32_t filenameKey(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  const char* name = path.c_str() + (slash == std::string::npos ? 0 : slash + 1);
  if (!*name) return 0;
  MD5Builder md5;
  md5.begin();
  md5.add(name);
  md5.calculate();
  return keyOfMd5(md5.toString().c_str());
}

// True when `found` holds for one of the keys the book is known by: its filename hash, then, only when that
// misses and sync matches by content, its content hash, which reads the book.
template <typename Found>
bool matches(const std::string& path, Found&& found) {
  const uint32_t name = filenameKey(path);
  if (name && found(name)) return true;
  if (KOREADER_STORE.getMatchMethod() != DocumentMatchMethod::BINARY) return false;
  const uint32_t content = keyOfMd5(KOReaderDocumentId::calculate(path));
  return content && found(content);
}

bool hasKey(const std::vector<uint32_t>& keys, const uint32_t key) {
  return std::binary_search(keys.begin(), keys.end(), key);
}

// One key, streamed from the file with no heap.
bool fileHasKey(const uint32_t key) {
  HalFile f;
  uint32_t account;
  uint64_t cursor;
  uint16_t count = 0;
  if (!openIndex(f, account, cursor, count)) return false;
  uint32_t buf[32];
  for (uint16_t done = 0; done < count;) {
    const uint16_t take = std::min<uint16_t>(32, count - done);
    if (f.read(reinterpret_cast<uint8_t*>(buf), take * sizeof(uint32_t)) != static_cast<int>(take * sizeof(uint32_t)))
      return false;
    if (std::find(buf, buf + take, key) != buf + take) return true;
    done += take;
  }
  return false;
}
}  // namespace

void Snapshot::load() {
  uint32_t account;
  uint64_t cursor;
  readIndex(account, cursor, keys);
}

bool Snapshot::has(const std::string& path) const {
  return !keys.empty() && matches(path, [this](const uint32_t k) { return hasKey(keys, k); });
}

bool contains(const std::string& path) {
  if (!Storage.exists(indexPath().c_str())) return false;
  return matches(path, fileHasKey);
}

bool markRead(const std::string& path) {
  uint32_t account;
  uint64_t cursor;
  std::vector<uint32_t> keys;
  if (!readIndex(account, cursor, keys, 1) && Storage.exists(indexPath().c_str())) return false;
  // The filename key is enough: it is checked whatever the sync setting.
  const uint32_t key = filenameKey(path);
  if (key == 0) return false;
  if (hasKey(keys, key)) return true;
  if (keys.size() >= MAX_ENTRIES) return false;
  if (keys.capacity() == keys.size()) {  // no index yet: nothing was reserved
    if (!fits(1)) return false;
    keys.reserve(1);
  }
  keys.insert(std::upper_bound(keys.begin(), keys.end(), key), key);
  return writeIndex(account, cursor, keys);
}

bool forget(const std::string& path) {
  // Every book open asks: only one that is in the index loads it.
  uint32_t hit = 0;
  const auto inFile = [&hit](const uint32_t k) {
    if (!fileHasKey(k)) return false;
    hit = k;
    return true;
  };
  if (!Storage.exists(indexPath().c_str()) || !matches(path, inFile)) return false;
  uint32_t account;
  uint64_t cursor;
  std::vector<uint32_t> keys;
  if (!readIndex(account, cursor, keys)) {
    LOG_ERR("FIN", "%s stays in the finished index", path.c_str());
    return false;
  }
  const auto at = std::lower_bound(keys.begin(), keys.end(), hit);
  if (at == keys.end() || *at != hit) return false;
  keys.erase(at);
  return writeIndex(account, cursor, keys);
}

bool Feed::begin(const uint32_t accountKey) {
  uint32_t savedAccount = 0;
  bool readable;
  {
    HalFile f;
    uint64_t cursor;
    uint16_t count;
    readable = openIndex(f, savedAccount, cursor, count);
  }
  // Room for the books the feed adds, reserved with the rest: growing the vector later would need room for
  // two copies. What does not fit this time comes with the next sync. One that reads but does not fit now
  // waits for the next sync; a damaged one is rebuilt from the feed.
  if (readable && !readIndex(savedAccount, feedCursor, keys, 1024)) return false;
  account = accountKey;
  if (!readable || savedAccount != accountKey) {
    // Another account's books go: the feed sends this one's from the start.
    keys.clear();
    feedCursor = 0;
    if (!fits(1024)) return false;
    keys.reserve(1024);
  }
  return true;
}

bool Feed::set(const uint32_t key, const bool finished) {
  const auto at = std::lower_bound(keys.begin(), keys.end(), key);
  const bool present = at != keys.end() && *at == key;
  if (finished && !present) {
    if (keys.size() >= keys.capacity()) {
      // This sync's room is used up: the page comes again next time, with more room.
      if (keys.capacity() < MAX_ENTRIES) return false;
      full = true;  // the index itself is full: skipped, or every later page would stop here too
      return true;
    }
    keys.insert(at, key);
  } else if (!finished && present) {
    keys.erase(at);
  }
  return true;
}

bool Feed::save() const { return writeIndex(account, feedCursor, keys); }

}  // namespace finishedIndex
