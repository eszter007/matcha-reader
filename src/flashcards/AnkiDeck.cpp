#include "AnkiDeck.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <PersistableStore.h>
#include <SdSystemDir.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cstring>

#include "FlashcardStore.h"

namespace ankideck {
namespace {
constexpr size_t RECORD_BYTES = INDEX_RECORD_BYTES;
constexpr size_t MAX_CARDS = flashcards::Session::MAX_CARDS;  // as many as progress is kept for

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

// The deck's own small files, beside its folder: "anki/<deckId>.json" and so on.
std::string besideDir(const std::string& deckId, const char* suffix) {
  return sdsystem::path(("anki/" + deckId + suffix).c_str());
}

// Once per deck and boot: files an older firmware (or the import tool) kept inside the deck's
// folder move beside it. deck.json goes last, and only when the others moved, so a move cut short
// is finished next time. A file already beside the folder is kept and the old one left alone: a
// rename cut short can leave both names on the same data, and removing one would free the other's.
void moveOutOfFolder(const std::string& deckId) {
  static std::vector<std::string> done;
  if (std::find(done.begin(), done.end(), deckId) != done.end()) return;
  done.push_back(deckId);
  if (Storage.exists(besideDir(deckId, ".json").c_str())) return;
  const std::string dir = sdsystem::path(("anki/" + deckId + "/").c_str());
  static constexpr const char* MOVES[][2] = {{"cards.bin", ".cards.bin"},
                                             {"buried.bin", ".buried.bin"},
                                             {"leeches.bin", ".leeches.bin"},
                                             {"style.css", ".css"},
                                             {"deck.json", ".json"}};
  for (const auto& move : MOVES) {
    const std::string from = dir + move[0];
    if (!Storage.exists(from.c_str())) continue;
    const std::string to = besideDir(deckId, move[1]);
    if (Storage.exists(to.c_str())) continue;
    if (!Storage.rename(from.c_str(), to.c_str())) {
      LOG_ERR("ANKI", "Could not move %s", from.c_str());
      return;
    }
  }
}
}  // namespace

std::string dirFor(const std::string& deckId) { return sdsystem::path(("anki/" + deckId + "/").c_str()); }

std::string imageCacheDir(const std::string& deckId) { return sdsystem::path(("anki-cache/" + deckId + "/").c_str()); }

// Not in the deck's folder: that holds two files per card and their images, and FAT finds a name
// by reading the folder through -- about 250 ms a lookup in a 2000-card deck, every time.
std::string metaPath(const std::string& deckId, const char* suffix) {
  moveOutOfFolder(deckId);
  return besideDir(deckId, suffix);
}

std::string idOf(const std::string& key) { return isAnkiKey(key) ? key.substr(std::strlen(KEY_PREFIX)) : key; }

std::string cardsDir(const std::string& deckId) { return sdsystem::path(("anki-cards/" + deckId + "/").c_str()); }

std::string sideFolder(const std::string& deckId, const unsigned index) {
  char folder[4];
  snprintf(folder, sizeof(folder), "%02u", index % SIDE_FOLDERS);
  return cardsDir(deckId) + folder + "/";
}

std::string mediaDir(const std::string& deckId) { return cardsDir(deckId) + "media/"; }

std::string frontPath(const std::string& deckId, const uint64_t cardId) {
  return sideFolder(deckId, static_cast<unsigned>(cardId % SIDE_FOLDERS)) + std::to_string(cardId) + "q.xhtml";
}
std::string backPath(const std::string& deckId, const uint64_t cardId) {
  return sideFolder(deckId, static_cast<unsigned>(cardId % SIDE_FOLDERS)) + std::to_string(cardId) + "a.xhtml";
}
std::string oldFrontPath(const std::string& deckId, const uint64_t cardId) {
  return dirFor(deckId) + std::to_string(cardId) + "q.xhtml";
}
std::string oldBackPath(const std::string& deckId, const uint64_t cardId) {
  return dirFor(deckId) + std::to_string(cardId) + "a.xhtml";
}
// Beside the folder too: a refetch rewrites it, which puts it at the folder's end, two lookups per
// card side. Decks moved before it was (their deck.json already beside) move it on first use.
std::string cssPath(const std::string& deckId) {
  static std::vector<std::string> checked;
  const std::string path = metaPath(deckId, ".css");
  if (std::find(checked.begin(), checked.end(), deckId) == checked.end()) {
    checked.push_back(deckId);
    const std::string old = dirFor(deckId) + "style.css";
    if (!Storage.exists(path.c_str()) && Storage.exists(old.c_str()) && !Storage.rename(old.c_str(), path.c_str())) {
      LOG_ERR("ANKI", "Could not move %s", old.c_str());
      return old;
    }
  }
  return path;
}

std::string leechPath(const std::string& deckId) { return metaPath(deckId, ".leeches.bin"); }

void forgetDeckFiles(const std::string& deckId) {
  for (const char* suffix : {".json", ".cards.bin", ".buried.bin", ".leeches.bin", ".css"}) {
    const std::string path = besideDir(deckId, suffix);
    if (Storage.exists(path.c_str())) Storage.remove(path.c_str());
  }
  // Its images' decoded pixels: a new image of the same name and size would otherwise show the old.
  for (const std::string& folder : {imageCacheDir(deckId), cardsDir(deckId)}) {
    if (Storage.exists(folder.c_str())) Storage.removeDir(folder.c_str());
  }
}

bool readInfo(const std::string& deckId, DeckInfo& out) {
  JsonDocument doc;
  if (!PersistableStoreBase::readDocFromFile(metaPath(deckId, ".json").c_str(), doc)) return false;
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
  out.reviewOrder = doc["reviewOrder"] | static_cast<uint8_t>(DeckInfo::REVIEW_DUE);
  out.newRandom = doc["newRandom"] | false;
  out.newMix = doc["newMix"] | static_cast<uint8_t>(DeckInfo::NEW_AFTER);
  out.buryNew = doc["buryNew"] | false;
  out.buryReviews = doc["buryReviews"] | false;
  if (doc["easyDays"].is<JsonArrayConst>() && doc["easyDays"].size() == 7) {
    for (size_t i = 0; i < 7; i++) out.easyDays[i] = doc["easyDays"][i] | static_cast<uint8_t>(100);
  }
  out.leechThreshold = doc["leechThreshold"] | static_cast<uint8_t>(0);
  out.leechSuspend = doc["leechSuspend"] | true;
  out.fetchVersion = doc["fetchVersion"] | static_cast<uint8_t>(0);
  out.typesMod = doc["typesMod"] | static_cast<uint32_t>(0);
  out.refetchAll = doc["refetchAll"] | false;
  out.refetchedUpTo = doc["refetchedUpTo"] | static_cast<uint64_t>(0);
  out.cardsMoved = doc["cardsMoved"] | false;
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
  if (info.reviewOrder != DeckInfo::REVIEW_DUE) doc["reviewOrder"] = info.reviewOrder;
  if (info.newRandom) doc["newRandom"] = true;
  if (info.newMix != DeckInfo::NEW_AFTER) doc["newMix"] = info.newMix;
  if (info.buryNew) doc["buryNew"] = true;
  if (info.buryReviews) doc["buryReviews"] = true;
  if (std::any_of(info.easyDays, info.easyDays + 7, [](const uint8_t p) { return p != 100; })) {
    JsonArray days = doc["easyDays"].to<JsonArray>();
    for (const uint8_t p : info.easyDays) days.add(p);
  }
  if (info.leechThreshold > 0) {
    doc["leechThreshold"] = info.leechThreshold;
    doc["leechSuspend"] = info.leechSuspend;
  }
  if (info.fetchVersion > 0) doc["fetchVersion"] = info.fetchVersion;
  if (info.typesMod > 0) doc["typesMod"] = info.typesMod;
  if (info.refetchAll) {
    doc["refetchAll"] = true;
    if (info.refetchedUpTo > 0) doc["refetchedUpTo"] = info.refetchedUpTo;
  }
  if (info.cardsMoved) doc["cardsMoved"] = true;
  if (!info.cssHashes.empty()) {
    JsonArray hashes = doc["cssHashes"].to<JsonArray>();
    for (const uint32_t h : info.cssHashes) hashes.add(h);
  }
  return PersistableStoreBase::writeDocToFile(metaPath(info.id, ".json").c_str(), doc);
}

void forgetCardsSynced(const std::string& deckId) {
  DeckInfo info;
  if (!readInfo(deckId, info) || info.cardsSyncedMs == 0) return;
  info.cardsSyncedMs = 0;
  writeInfo(info);
}

bool writeBuried(const std::string& deckId, const int32_t day, const std::vector<uint64_t>& ids) {
  const std::string path = metaPath(deckId, ".buried.bin");
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
  const std::string path = metaPath(deckId, ".buried.bin");
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

bool scanIndex(const std::string& deckId, const std::function<void(const CardRecord&)>& each) {
  const std::string path = metaPath(deckId, ".cards.bin");
  HalFile file;
  // Opened first: the index is nearly always there, and asking first would look it up twice.
  if (!Storage.openFileForRead("ANKI", path, file)) return !Storage.exists(path.c_str());
  const size_t count = std::min<size_t>(file.fileSize() / RECORD_BYTES, MAX_CARDS);
  constexpr size_t CHUNK = 16;
  uint8_t buf[CHUNK * RECORD_BYTES];
  for (size_t i = 0; i < count;) {
    const size_t n = std::min(CHUNK, count - i);
    if (file.read(buf, n * RECORD_BYTES) != static_cast<int>(n * RECORD_BYTES)) return false;
    for (size_t k = 0; k < n; k++) each(decode(buf + k * RECORD_BYTES));
    i += n;
  }
  return true;
}

bool IndexReader::open(const std::string& deckId) {
  count = next = bufStart = bufCount = 0;
  const std::string path = metaPath(deckId, ".cards.bin");
  if (!Storage.openFileForRead("ANKI", path, file)) return !Storage.exists(path.c_str());
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
  path = metaPath(deckId, ".cards.bin");
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
