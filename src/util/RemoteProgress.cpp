#include "RemoteProgress.h"

#include <HalMemory.h>
#include <HalStorage.h>
#include <KOReaderCredentialStore.h>
#include <KOReaderDocumentId.h>
#include <Logging.h>
#include <MD5Builder.h>
#include <SdSystemDir.h>

#include <algorithm>
#include <cstring>

namespace remoteProgress {
namespace {
// Version, account key, cursor, count, then the sorted keys and, in the same order, their percents.
constexpr uint8_t VERSION = 1;
constexpr size_t HEADER_BYTES = 1 + 4 + 8 + 2;
constexpr size_t ENTRY_BYTES = sizeof(uint32_t) + 1;

std::string indexPath() { return sdsystem::path("remote_progress.idx"); }

bool fits(const size_t count) {
  return HalMemory::getDefaultHeap().largestBlockBytes >= count * sizeof(uint32_t) + 1024;
}

// Opens the index past its header. False when there is none or it is not one this firmware wrote.
bool openIndex(HalFile& f, uint32_t& account, uint64_t& cursor, uint16_t& count) {
  if (!Storage.exists(indexPath().c_str()) || !Storage.openFileForRead("RPROG", indexPath().c_str(), f)) return false;
  uint8_t version = 0;
  if (f.read(&version, 1) != 1 || version != VERSION || f.read(reinterpret_cast<uint8_t*>(&account), 4) != 4 ||
      f.read(reinterpret_cast<uint8_t*>(&cursor), 8) != 8 || f.read(reinterpret_cast<uint8_t*>(&count), 2) != 2 ||
      count > MAX_ENTRIES || f.size() != HEADER_BYTES + count * ENTRY_BYTES) {
    LOG_ERR("RPROG", "Progress index unreadable");
    return false;
  }
  return true;
}

// The whole index with room for `extra` more books, each vector in one block; false (with both empty) when
// it is missing, damaged or does not fit.
bool readIndex(uint32_t& account, uint64_t& cursor, std::vector<uint32_t>& keys, std::vector<uint8_t>& percents,
               const size_t extra = 0) {
  keys.clear();
  percents.clear();
  account = 0;
  cursor = 0;
  HalFile f;
  uint16_t count = 0;
  if (!openIndex(f, account, cursor, count)) return false;
  const size_t room = std::min(MAX_ENTRIES, count + extra);
  // reserve() aborts on failure under -fno-exceptions, so check the blocks first.
  if (!fits(room) || HalMemory::getDefaultHeap().freeBytes < room * ENTRY_BYTES + 2048) {
    LOG_ERR("RPROG", "Not enough heap for %u books", count);
    return false;
  }
  keys.reserve(room);
  percents.reserve(room);
  keys.resize(count);
  percents.resize(count);
  if (count && (f.read(reinterpret_cast<uint8_t*>(keys.data()), count * sizeof(uint32_t)) !=
                    static_cast<int>(count * sizeof(uint32_t)) ||
                f.read(percents.data(), count) != static_cast<int>(count))) {
    keys.clear();
    percents.clear();
    return false;
  }
  return true;
}

// Through a temp file, so an interrupted write leaves the old index whole.
bool writeIndex(const uint32_t account, const uint64_t cursor, const std::vector<uint32_t>& keys,
                const std::vector<uint8_t>& percents) {
  const std::string path = indexPath();
  const std::string tmp = path + ".tmp";
  {
    HalFile f;
    if (!Storage.openFileForWrite("RPROG", tmp, f)) return false;
    const auto count = static_cast<uint16_t>(keys.size());
    if (f.write(&VERSION, 1) != 1 || f.write(reinterpret_cast<const uint8_t*>(&account), 4) != 4 ||
        f.write(reinterpret_cast<const uint8_t*>(&cursor), 8) != 8 ||
        f.write(reinterpret_cast<const uint8_t*>(&count), 2) != 2 ||
        (count && (f.write(reinterpret_cast<const uint8_t*>(keys.data()), count * sizeof(uint32_t)) !=
                       count * sizeof(uint32_t) ||
                   f.write(percents.data(), count) != count))) {
      LOG_ERR("RPROG", "Progress index not written");
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

// `lookup(key)` for the keys the book is known by, until one answers (>= 0): its filename hash, then, only when
// that misses and sync matches by content, its content hash, which reads the book.
template <typename Lookup>
int lookUp(const std::string& path, Lookup&& lookup) {
  const uint32_t name = filenameKey(path);
  const int byName = name ? lookup(name) : -1;
  if (byName >= 0 || KOREADER_STORE.getMatchMethod() != DocumentMatchMethod::BINARY) return byName;
  const uint32_t content = keyOfMd5(KOReaderDocumentId::calculate(path));
  return content ? lookup(content) : -1;
}

// Position of `key` in the sorted keys, or -1.
int indexOf(const std::vector<uint32_t>& keys, const uint32_t key) {
  const auto at = std::lower_bound(keys.begin(), keys.end(), key);
  return at != keys.end() && *at == key ? static_cast<int>(at - keys.begin()) : -1;
}

// One key's position, streamed from the file with no heap; `percent` set when found.
int fileIndexOf(const uint32_t key, uint8_t& percent) {
  HalFile f;
  uint32_t account;
  uint64_t cursor;
  uint16_t count = 0;
  if (!openIndex(f, account, cursor, count)) return -1;
  uint32_t buf[32];
  for (uint16_t done = 0; done < count;) {
    const uint16_t take = std::min<uint16_t>(32, count - done);
    if (f.read(reinterpret_cast<uint8_t*>(buf), take * sizeof(uint32_t)) != static_cast<int>(take * sizeof(uint32_t)))
      return -1;
    const auto hit = std::find(buf, buf + take, key);
    if (hit != buf + take) {
      const int at = done + static_cast<int>(hit - buf);
      if (!f.seek(HEADER_BYTES + count * sizeof(uint32_t) + at) || f.read(&percent, 1) != 1) return -1;
      return at;
    }
    done += take;
  }
  return -1;
}
}  // namespace

void Snapshot::load() {
  uint32_t account;
  uint64_t cursor;
  readIndex(account, cursor, keys, percents);
}

int Snapshot::percent(const std::string& path) const {
  if (keys.empty()) return -1;
  return lookUp(path, [this](const uint32_t k) {
    const int at = indexOf(keys, k);
    return at < 0 ? -1 : static_cast<int>(percents[at]);
  });
}

int percent(const std::string& path) {
  if (!Storage.exists(indexPath().c_str())) return -1;
  return lookUp(path, [](const uint32_t k) {
    uint8_t pct = 0;
    return fileIndexOf(k, pct) < 0 ? -1 : static_cast<int>(pct);
  });
}

bool markRead(const std::string& path) {
  uint32_t account;
  uint64_t cursor;
  std::vector<uint32_t> keys;
  std::vector<uint8_t> percents;
  if (!readIndex(account, cursor, keys, percents, 1) && Storage.exists(indexPath().c_str())) return false;
  // The filename key is enough: it is checked whatever the sync setting.
  const uint32_t key = filenameKey(path);
  if (key == 0) return false;
  const int at = indexOf(keys, key);
  if (at >= 0) {
    percents[at] = 100;
  } else {
    if (keys.size() >= MAX_ENTRIES) return false;
    if (keys.capacity() == keys.size()) {  // no index yet: nothing was reserved
      if (!fits(1)) return false;
      keys.reserve(1);
      percents.reserve(1);
    }
    const auto pos = std::upper_bound(keys.begin(), keys.end(), key) - keys.begin();
    keys.insert(keys.begin() + pos, key);
    percents.insert(percents.begin() + pos, 100);
  }
  return writeIndex(account, cursor, keys, percents);
}

bool forget(const std::string& path) {
  // Every book open asks: only one that is in the index loads it.
  uint32_t hit = 0;
  const bool found = lookUp(path, [&hit](const uint32_t k) {
                       uint8_t pct;
                       if (fileIndexOf(k, pct) < 0) return -1;
                       hit = k;
                       return 0;
                     }) >= 0;
  if (!found) return false;
  uint32_t account;
  uint64_t cursor;
  std::vector<uint32_t> keys;
  std::vector<uint8_t> percents;
  if (!readIndex(account, cursor, keys, percents)) {
    LOG_ERR("RPROG", "%s stays in the progress index", path.c_str());
    return false;
  }
  const int at = indexOf(keys, hit);
  if (at < 0) return false;
  keys.erase(keys.begin() + at);
  percents.erase(percents.begin() + at);
  return writeIndex(account, cursor, keys, percents);
}

void readHere(const std::string& path, const int enterPercent, const int exitPercent) {
  if (exitPercent < 0 || exitPercent >= 100 || exitPercent == enterPercent) return;
  if (percent(path) == 100) forget(path);
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
  // Room for the books the feed adds, reserved with the rest: growing the vectors later would need room for
  // two copies. What does not fit this time comes with the next sync. One that reads but does not fit now
  // waits for the next sync; a damaged one is rebuilt from the feed.
  if (readable && !readIndex(savedAccount, feedCursor, keys, percents, 1024)) return false;
  account = accountKey;
  if (!readable || savedAccount != accountKey) {
    // Another account's books go: the feed sends this one's from the start.
    keys.clear();
    percents.clear();
    feedCursor = 0;
    if (!fits(1024)) return false;
    keys.reserve(1024);
    percents.reserve(1024);
  }
  return true;
}

bool Feed::set(const uint32_t key, const uint8_t percent) {
  const auto pos = std::lower_bound(keys.begin(), keys.end(), key) - keys.begin();
  const bool present = pos < static_cast<std::ptrdiff_t>(keys.size()) && keys[pos] == key;
  if (present && percent == 0) {
    keys.erase(keys.begin() + pos);
    percents.erase(percents.begin() + pos);
  } else if (present) {
    percents[pos] = percent;
  } else if (percent > 0) {
    if (keys.size() >= keys.capacity()) {
      // This sync's room is used up: the page comes again next time, with more room.
      if (keys.capacity() < MAX_ENTRIES) return false;
      full = true;  // the index itself is full: skipped, or every later page would stop here too
      return true;
    }
    keys.insert(keys.begin() + pos, key);
    percents.insert(percents.begin() + pos, percent);
  }
  return true;
}

bool Feed::save() const { return writeIndex(account, feedCursor, keys, percents); }

}  // namespace remoteProgress
