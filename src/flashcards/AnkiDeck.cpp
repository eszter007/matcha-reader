#include "AnkiDeck.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <PersistableStore.h>
#include <SdSystemDir.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cstring>

namespace ankideck {
namespace {
constexpr size_t RECORD_BYTES = INDEX_RECORD_BYTES;
constexpr size_t MAX_CARDS = 2000;  // as many as the flashcard store keeps progress for

template <typename T>
void put(uint8_t*& p, const T v) {
  memcpy(p, &v, sizeof(T));
  p += sizeof(T);
}
template <typename T>
T take(const uint8_t*& p) {
  T v;
  memcpy(&v, p, sizeof(T));  // records are not aligned
  p += sizeof(T);
  return v;
}
CardRecord decode(const uint8_t* p) {
  CardRecord r;
  r.cardId = take<uint64_t>(p);
  r.noteId = take<uint64_t>(p);
  r.noteMod = take<uint32_t>(p);
  r.flags = take<uint32_t>(p);
  return r;
}
}  // namespace

std::string dirFor(const std::string& deckId) { return sdsystem::path(("anki/" + deckId + "/").c_str()); }

std::string idOf(const std::string& key) { return isAnkiKey(key) ? key.substr(std::strlen(KEY_PREFIX)) : key; }

std::string frontPath(const std::string& deckId, const uint64_t cardId) {
  return dirFor(deckId) + std::to_string(cardId) + "q.xhtml";
}
std::string backPath(const std::string& deckId, const uint64_t cardId) {
  return dirFor(deckId) + std::to_string(cardId) + "a.xhtml";
}
std::string cssPath(const std::string& deckId) { return dirFor(deckId) + "style.css"; }

bool readInfo(const std::string& deckId, DeckInfo& out) {
  JsonDocument doc;
  if (!PersistableStoreBase::readDocFromFile((dirFor(deckId) + "deck.json").c_str(), doc)) return false;
  out.id = deckId;
  out.name = doc["name"] | "";
  out.lastReviewId = doc["lastReviewId"] | static_cast<uint64_t>(0);
  out.cardsSyncedMs = doc["cardsSyncedMs"] | static_cast<int64_t>(0);
  out.newPerDay = doc["newPerDay"] | -1;
  out.retention = doc["retention"] | 0.0f;
  out.maxInterval = doc["maxInterval"] | static_cast<uint16_t>(0);
  out.reviewsPerDay = doc["reviewsPerDay"] | -1;
  const auto readSteps = [](JsonVariantConst list, uint16_t* steps, uint8_t& count) {
    if (!list.is<JsonArrayConst>()) return;
    count = 0;
    for (JsonVariantConst m : list.as<JsonArrayConst>()) {
      if (count < DeckInfo::MAX_STEPS) steps[count++] = m.as<uint16_t>();
    }
  };
  readSteps(doc["learnSteps"], out.learnSteps, out.learnCount);
  readSteps(doc["relearnSteps"], out.relearnSteps, out.relearnCount);
  out.fetchVersion = doc["fetchVersion"] | static_cast<uint8_t>(0);
  out.cssHashes.clear();
  for (JsonVariantConst h : doc["cssHashes"].as<JsonArrayConst>()) {
    if (out.cssHashes.size() < MAX_CSS_HASHES) out.cssHashes.push_back(h.as<uint32_t>());
  }
  return !out.name.empty();
}

bool writeInfo(const DeckInfo& info) {
  JsonDocument doc;
  doc["name"] = info.name;
  doc["lastReviewId"] = info.lastReviewId;
  if (info.cardsSyncedMs > 0) doc["cardsSyncedMs"] = info.cardsSyncedMs;
  if (info.newPerDay >= 0) doc["newPerDay"] = info.newPerDay;
  if (info.retention > 0) doc["retention"] = info.retention;
  if (info.maxInterval > 0) doc["maxInterval"] = info.maxInterval;
  if (info.reviewsPerDay >= 0) doc["reviewsPerDay"] = info.reviewsPerDay;
  const auto writeSteps = [&doc](const char* key, const uint16_t* steps, const uint8_t count) {
    if (count == 0xFF) return;
    JsonArray list = doc[key].to<JsonArray>();
    for (uint8_t i = 0; i < count; i++) list.add(steps[i]);
  };
  writeSteps("learnSteps", info.learnSteps, info.learnCount);
  writeSteps("relearnSteps", info.relearnSteps, info.relearnCount);
  if (info.fetchVersion > 0) doc["fetchVersion"] = info.fetchVersion;
  if (!info.cssHashes.empty()) {
    JsonArray hashes = doc["cssHashes"].to<JsonArray>();
    for (const uint32_t h : info.cssHashes) hashes.add(h);
  }
  return PersistableStoreBase::writeDocToFile((dirFor(info.id) + "deck.json").c_str(), doc);
}

void forgetCardsSynced(const std::string& deckId) {
  DeckInfo info;
  if (!readInfo(deckId, info) || info.cardsSyncedMs == 0) return;
  info.cardsSyncedMs = 0;
  writeInfo(info);
}

bool writeBuried(const std::string& deckId, const int32_t day, const std::vector<uint64_t>& ids) {
  const std::string path = dirFor(deckId) + "buried.bin";
  if (ids.empty()) {
    if (Storage.exists(path.c_str())) Storage.remove(path.c_str());
    return true;
  }
  HalFile file;
  if (!Storage.openFileForWrite("ANKI", path, file)) return false;
  const uint32_t count = static_cast<uint32_t>(std::min(ids.size(), MAX_BURIED));
  if (file.write(reinterpret_cast<const uint8_t*>(&day), sizeof(day)) != sizeof(day) ||
      file.write(reinterpret_cast<const uint8_t*>(&count), sizeof(count)) != sizeof(count)) {
    return false;
  }
  const size_t bytes = count * sizeof(uint64_t);
  return file.write(reinterpret_cast<const uint8_t*>(ids.data()), bytes) == bytes;
}

void readBuried(const std::string& deckId, const int32_t day, std::vector<uint64_t>& out) {
  out.clear();
  const std::string path = dirFor(deckId) + "buried.bin";
  if (day < 0 || !Storage.exists(path.c_str())) return;
  HalFile file;
  if (!Storage.openFileForRead("ANKI", path, file)) return;
  uint8_t head[8];
  if (file.read(head, sizeof(head)) != static_cast<int>(sizeof(head))) return;
  int32_t stored = 0;
  uint32_t count = 0;
  memcpy(&stored, head, sizeof(stored));
  memcpy(&count, head + 4, sizeof(count));
  if (stored != day || count == 0 || count > MAX_BURIED) return;
  // std::vector allocates with throwing new: check the block first.
  if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < count * sizeof(uint64_t) + 8 * 1024) {
    LOG_ERR("ANKI", "OOM: %u buried cards", static_cast<unsigned>(count));
    return;
  }
  out.resize(count);
  const int bytes = static_cast<int>(count * sizeof(uint64_t));
  if (file.read(reinterpret_cast<uint8_t*>(out.data()), bytes) != bytes) out.clear();
}

bool readIndex(const std::string& deckId, std::vector<CardRecord>& out) {
  out.clear();
  const std::string path = dirFor(deckId) + "cards.bin";
  if (!Storage.exists(path.c_str())) return true;
  HalFile file;
  if (!Storage.openFileForRead("ANKI", path, file)) return false;
  const size_t count = std::min<size_t>(file.fileSize() / RECORD_BYTES, MAX_CARDS);
  // std::vector allocates with throwing new: check the block first.
  if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < count * sizeof(CardRecord) + 8 * 1024) {
    LOG_ERR("ANKI", "OOM: index of %u cards", static_cast<unsigned>(count));
    return false;
  }
  out.reserve(count);
  uint8_t rec[RECORD_BYTES];
  for (size_t i = 0; i < count; i++) {
    if (file.read(rec, sizeof(rec)) != static_cast<int>(sizeof(rec))) {
      out.clear();
      return false;
    }
    out.push_back(decode(rec));
  }
  return true;
}

bool IndexReader::open(const std::string& deckId) {
  count = next = bufStart = bufCount = 0;
  const std::string path = dirFor(deckId) + "cards.bin";
  if (!Storage.exists(path.c_str())) return true;
  if (!Storage.openFileForRead("ANKI", path, file)) return false;
  count = std::min<size_t>(file.fileSize() / RECORD_BYTES, MAX_CARDS);
  return true;
}

bool IndexReader::at(const size_t i, CardRecord& out) {
  uint8_t rec[RECORD_BYTES];
  if (i >= count || !file.seek(i * RECORD_BYTES) || file.read(rec, sizeof(rec)) != static_cast<int>(sizeof(rec))) {
    return false;
  }
  out = decode(rec);
  return true;
}

bool IndexReader::findNext(const uint64_t cardId, CardRecord& out) {
  static_assert(sizeof(buf) == BUFFERED * RECORD_BYTES, "IndexReader buffer");
  for (; next < count; next++) {
    if (next < bufStart || next >= bufStart + bufCount) {
      const size_t n = std::min(BUFFERED, count - next);
      if (!file.seek(next * RECORD_BYTES) || file.read(buf, n * RECORD_BYTES) != static_cast<int>(n * RECORD_BYTES)) {
        bufCount = 0;
        return false;
      }
      bufStart = next;
      bufCount = n;
    }
    out = decode(buf + (next - bufStart) * RECORD_BYTES);
    if (out.cardId == cardId) return true;
    if (out.cardId > cardId) return false;
  }
  return false;
}

bool IndexReader::copyTail(const size_t first, HalFile& out, uint8_t* chunk, const size_t chunkBytes) {
  for (size_t i = first; i < count;) {
    const size_t n = std::min(count - i, chunkBytes / RECORD_BYTES);
    if (!file.seek(i * RECORD_BYTES) || file.read(chunk, n * RECORD_BYTES) != static_cast<int>(n * RECORD_BYTES) ||
        out.write(chunk, n * RECORD_BYTES) != n * RECORD_BYTES) {
      return false;
    }
    i += n;
  }
  return true;
}

static bool writeRecord(HalFile& file, const CardRecord& r) {
  uint8_t rec[RECORD_BYTES];
  uint8_t* p = rec;
  put(p, r.cardId);
  put(p, r.noteId);
  put(p, r.noteMod);
  put(p, r.flags);
  return file.write(rec, sizeof(rec)) == sizeof(rec);
}

bool IndexWriter::open(const std::string& id) {
  deckId = id;
  path = dirFor(deckId) + "cards.bin";
  tmp = path + ".tmp";
  return Storage.openFileForWrite("ANKI", tmp, file);
}

bool IndexWriter::add(const CardRecord& r) { return writeRecord(file, r); }

bool IndexWriter::commit() {
  file.close();
  return Storage.replaceFile(tmp.c_str(), path.c_str());
}

bool IndexWriter::checkpoint(IndexReader& known, const uint64_t lastDone, size_t& written) {
  file.close();
  const std::string next = path + ".ckp";
  {
    HalFile in, out;
    if (!Storage.openFileForRead("ANKI", tmp, in) || !Storage.openFileForWrite("ANKI", next, out)) return false;
    written = in.fileSize() / RECORD_BYTES;
    uint8_t chunk[RECORD_BYTES * 8];
    int n;
    while ((n = in.read(chunk, sizeof(chunk))) > 0) {
      if (out.write(chunk, n) != static_cast<size_t>(n)) return false;
    }
    if (n < 0) return false;
    // The old records after lastDone: the reader stopped at or just before them.
    size_t first = known.position();
    CardRecord r;
    while (first < known.size() && known.at(first, r) && r.cardId <= lastDone) first++;
    if (!known.copyTail(first, out, chunk, sizeof(chunk))) return false;
  }
  known.close();
  if (!Storage.replaceFile(next.c_str(), path.c_str()) || !known.open(deckId)) return false;
  known.skipTo(written);
  return Storage.openFileForAppend("ANKI", tmp.c_str(), file);
}

void list(std::vector<DeckInfo>& out) {
  out.clear();
  HalFile dir = Storage.open(sdsystem::path("anki").c_str());
  if (!dir || !dir.isDirectory()) return;
  char name[32];
  for (HalFile entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    const size_t len = entry.getName(name, sizeof(name));
    if (len == 0 || len >= sizeof(name) || !entry.isDirectory()) continue;
    DeckInfo info;
    if (readInfo(std::string(name, len), info)) out.push_back(std::move(info));
  }
  std::sort(out.begin(), out.end(), [](const DeckInfo& a, const DeckInfo& b) { return a.name < b.name; });
}

}  // namespace ankideck
