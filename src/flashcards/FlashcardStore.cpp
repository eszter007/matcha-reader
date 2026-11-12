#include "FlashcardStore.h"

#include <FlashcardCsv.h>
#include <HalStorage.h>
#include <Logging.h>
#include <SdSystemDir.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cstring>

#include "CrossPointSettings.h"
#include "util/SentenceMining.h"

namespace flashcards {
namespace {
constexpr int32_t EPOCH_2020 = 18262;  // days from 1970-01-01 to 2020-01-01
constexpr char MAGIC[4] = {'F', 'C', 'S', '1'};
constexpr size_t HEADER_BYTES = 12;  // magic, day, newToday, count
constexpr size_t RECORD_BYTES = 24;
constexpr size_t READ_CHUNK = 512;
// Today when the clock was never set: progress is kept, nothing new becomes due.
int32_t lastKnownDay = -1;

std::string statePath(const std::string& language) {
  return sdsystem::path(("flashcards-" + language + ".bin").c_str());
}

std::string deckPath(const std::string& language) {
  return std::string(MINING_DIR) + "/sentences-" + language + ".csv";
}

// Streams a deck's rows through `parser`, calling onRow() for each. False when the file is missing.
template <typename OnRow>
bool scanCsv(const std::string& path, CsvRecordParser& parser, OnRow onRow) {
  HalFile file;
  if (!Storage.openFileForRead("FLASH", path, file)) return false;
  char buf[READ_CHUNK];
  int n;
  while ((n = file.read(buf, sizeof(buf))) > 0) {
    for (int i = 0; i < n; i++) {
      if (parser.feed(buf[i]) && !onRow(parser)) return true;
    }
  }
  if (parser.finish()) onRow(parser);
  return true;
}

template <typename T>
void put(uint8_t*& p, const T value) {
  memcpy(p, &value, sizeof(T));
  p += sizeof(T);
}

template <typename T>
T take(const uint8_t*& p) {
  T value;
  memcpy(&value, p, sizeof(T));  // byte copy: records are not aligned (RISC-V faults otherwise)
  p += sizeof(T);
  return value;
}

// `spare` reserves room for cards rated for the first time, so rating never grows the vector.
// std::vector allocates with throwing new (abort on failure), so the block is checked first.
bool loadStates(const std::string& language, std::vector<CardState>& out, uint16_t& day, uint16_t& newToday,
                const size_t spare = 0) {
  out.clear();
  day = 0;
  newToday = 0;
  HalFile file;
  const std::string path = statePath(language);
  if (!Storage.exists(path.c_str())) return true;  // no reviews yet
  if (!Storage.openFileForRead("FLASH", path, file)) return false;
  uint8_t header[HEADER_BYTES];
  // A short read is "not now", not "no progress": failing keeps the next save from overwriting
  // the file with an empty table.
  if (file.read(header, sizeof(header)) != static_cast<int>(sizeof(header))) {
    LOG_ERR("FLASH", "Could not read %s", path.c_str());
    return false;
  }
  if (memcmp(header, MAGIC, 4) != 0) {
    // Not a progress file at all: set it aside, not overwritten, and start the deck afresh.
    file.close();
    const std::string aside = path + ".bad";
    Storage.remove(aside.c_str());
    if (!Storage.rename(path.c_str(), aside.c_str())) return false;
    LOG_ERR("FLASH", "Unrecognised %s kept as %s", path.c_str(), aside.c_str());
    return true;
  }
  const uint8_t* p = header + 4;
  day = take<uint16_t>(p);
  newToday = take<uint16_t>(p);
  const uint32_t count = std::min<uint32_t>(take<uint32_t>(p), Session::MAX_CARDS);
  const size_t bytes = (count + spare) * sizeof(CardState);
  constexpr size_t HEADROOM = 8 * 1024;  // left for the render and SD paths after the table
  if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < bytes + HEADROOM) {
    LOG_ERR("FLASH", "OOM: %u bytes for %s progress", static_cast<unsigned>(bytes), path.c_str());
    return false;
  }
  out.reserve(count + spare);
  uint8_t rec[RECORD_BYTES];
  for (uint32_t i = 0; i < count; i++) {
    if (file.read(rec, sizeof(rec)) != static_cast<int>(sizeof(rec))) {
      LOG_ERR("FLASH", "%s ends early (%u of %u cards)", path.c_str(), static_cast<unsigned>(i),
              static_cast<unsigned>(count));
      out.clear();
      return false;
    }
    const uint8_t* q = rec;
    CardState s;
    s.id = take<uint64_t>(q);
    s.stability = take<float>(q);
    s.difficulty = take<float>(q);
    s.due = take<uint16_t>(q);
    s.lastReview = take<uint16_t>(q);
    s.reps = take<uint16_t>(q);
    s.lapses = take<uint8_t>(q);
    s.flags = take<uint8_t>(q);
    out.push_back(s);
  }
  std::sort(out.begin(), out.end(), [](const CardState& a, const CardState& b) { return a.id < b.id; });
  return true;
}

const CardState* findIn(const std::vector<CardState>& states, const uint64_t id) {
  const auto it = std::lower_bound(states.begin(), states.end(), id,
                                   [](const CardState& s, const uint64_t key) { return s.id < key; });
  return it != states.end() && it->id == id ? &*it : nullptr;
}

uint16_t todayOr(const uint16_t stored) {
  const int32_t t = today();
  return t >= 0 ? static_cast<uint16_t>(t) : stored;
}

bool writeStates(const std::string& language, const std::vector<CardState>& states, const uint16_t day,
                 const uint16_t newToday) {
  const std::string path = statePath(language);
  const std::string tmp = path + ".tmp";
  HalFile file;
  if (!Storage.openFileForWrite("FLASH", tmp, file)) return false;
  uint8_t header[HEADER_BYTES];
  uint8_t* p = header;
  memcpy(p, MAGIC, 4);
  p += 4;
  put<uint16_t>(p, day);
  put<uint16_t>(p, newToday);
  put<uint32_t>(p, static_cast<uint32_t>(states.size()));
  bool ok = file.write(header, sizeof(header)) == sizeof(header);
  // Records go out ten at a time: one SD call (and storage-mutex round trip) per batch.
  constexpr size_t BATCH = 10;
  uint8_t batch[BATCH * RECORD_BYTES];
  size_t filled = 0;
  for (size_t i = 0; ok && i < states.size(); i++) {
    const CardState& s = states[i];
    uint8_t* q = batch + filled * RECORD_BYTES;
    put(q, s.id);
    put(q, s.stability);
    put(q, s.difficulty);
    put(q, s.due);
    put(q, s.lastReview);
    put(q, s.reps);
    put(q, s.lapses);
    put(q, s.flags);
    if (++filled == BATCH || i + 1 == states.size()) {
      ok = file.write(batch, filled * RECORD_BYTES) == filled * RECORD_BYTES;
      filled = 0;
    }
  }
  file.close();  // before replaceFile renames it
  if (!ok || !Storage.replaceFile(tmp.c_str(), path.c_str())) {
    LOG_ERR("FLASH", "Saving %s failed", path.c_str());
    return false;
  }
  return true;
}

std::string vacationPath() { return sdsystem::path("flashcards-vacation.bin"); }

// The deck languages found in the mining folder.
void deckLanguages(std::vector<std::string>& out) {
  out.clear();
  HalFile dir = Storage.open(MINING_DIR);
  if (!dir || !dir.isDirectory()) return;
  char name[64];
  for (HalFile entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    const size_t len = entry.getName(name, sizeof(name));
    if (len == 0 || len >= sizeof(name) || entry.isDirectory()) continue;
    const std::string_view n(name, len);
    if (n.size() > 14 && n.substr(0, 10) == "sentences-" && n.substr(n.size() - 4) == ".csv") {
      out.emplace_back(n.substr(10, n.size() - 14));
    }
  }
  std::sort(out.begin(), out.end());
}
}  // namespace

fsrs::Params schedulingParams() {
  static constexpr float RETENTION[] = {0.80f, 0.85f, 0.90f, 0.95f};
  static constexpr uint16_t MAX_DAYS[] = {30, 180, 365, 36500};
  fsrs::Params p;
  p.retention = RETENTION[std::min<uint8_t>(SETTINGS.flashcardRetention, 3)];
  p.maxInterval = MAX_DAYS[std::min<uint8_t>(SETTINGS.flashcardMaxInterval, 3)];
  return p;
}

uint16_t newCardsPerDay() {
  static constexpr uint16_t PER_DAY[] = {5, 10, 20, 30, 50};
  return PER_DAY[std::min<uint8_t>(SETTINGS.flashcardNewPerDay, 4)];
}

int32_t today() {
  const int32_t d = dayNumber(sentencemining::today());
  if (d >= EPOCH_2020) lastKnownDay = d - EPOCH_2020;
  return lastKnownDay;
}

void listDecks(std::vector<DeckSummary>& out) {
  out.clear();
  syncVacation();
  std::vector<std::string> languages;
  deckLanguages(languages);
  const bool paused = onVacation();
  out.reserve(languages.size());
  for (const auto& language : languages) {
    DeckSummary deck;
    deck.language = language;
    std::vector<CardState> states;
    uint16_t day = 0, newToday = 0;
    // Without its progress a deck's counts would show every card as new: list it with none due.
    if (!loadStates(language, states, day, newToday)) {
      out.push_back(deck);
      continue;
    }
    const uint16_t now = todayOr(day);
    if (now != day) newToday = 0;
    const uint16_t perDay = newCardsPerDay();
    uint16_t fresh = 0;
    CsvRecordParser parser(1u << GUID, 32);
    scanCsv(deckPath(language), parser, [&](const CsvRecordParser& row) {
      const uint64_t id = guidToId(row.field(GUID));
      if (id == 0) return true;
      deck.total++;
      if (const CardState* s = findIn(states, id)) {
        if (s->due <= now) deck.due++;
      } else {
        fresh++;
      }
      return true;
    });
    deck.fresh = std::min<uint16_t>(fresh, perDay > newToday ? perDay - newToday : 0);
    if (paused) deck.due = deck.fresh = 0;
    out.push_back(std::move(deck));
  }
}

CardState* Session::find(const uint64_t id) { return const_cast<CardState*>(findIn(states, id)); }

const CardState* Session::find(const uint64_t id) const { return findIn(states, id); }

bool Session::open(const std::string& language) {
  lang = language;
  csvPath = deckPath(language);
  // Record offsets are taken after this, so they point into the upgraded file.
  upgraded = sentencemining::upgradeExport(csvPath);
  queue.clear();
  position = 0;
  reviewedCount = 0;
  std::fill(std::begin(ratingCounts), std::end(ratingCounts), 0);
  syncVacation();
  if (!loadStates(language, states, day, newToday, MAX_QUEUE)) return false;
  const uint16_t now = todayOr(day);
  if (now != day) newToday = 0;
  day = now;
  if (onVacation()) return true;  // progress loaded, nothing queued

  std::vector<QueueItem> fresh;
  const uint16_t perDay = newCardsPerDay();
  const size_t newLimit = perDay > newToday ? perDay - newToday : 0;
  queue.reserve(64);
  fresh.reserve(std::min<size_t>(newLimit, MAX_QUEUE));
  CsvRecordParser parser(1u << GUID, 32);
  const bool found = scanCsv(csvPath, parser, [&](const CsvRecordParser& row) {
    const uint64_t id = guidToId(row.field(GUID));
    if (id == 0) return true;
    if (const CardState* s = find(id)) {
      if (s->due <= day && queue.size() < MAX_QUEUE) queue.push_back({id, row.recordStart()});
    } else if (fresh.size() < newLimit && fresh.size() < MAX_QUEUE) {
      fresh.push_back({id, row.recordStart()});
    }
    return true;
  });
  if (!found) return false;
  // Most overdue first, then new cards in the order they were saved.
  std::stable_sort(queue.begin(), queue.end(),
                   [this](const QueueItem& a, const QueueItem& b) { return find(a.id)->due < find(b.id)->due; });
  for (const auto& item : fresh) {
    if (queue.size() >= MAX_QUEUE) break;
    queue.push_back(item);
  }
  LOG_INF("FLASH", "Deck %s: %u queued, %u states", language.c_str(), static_cast<unsigned>(queue.size()),
          static_cast<unsigned>(states.size()));
  return true;
}

bool Session::current(CardText& out) const {
  if (finished()) return false;
  HalFile file;
  if (!Storage.openFileForRead("FLASH", csvPath, file) || !file.seekSet(queue[position].offset)) return false;
  CsvRecordParser parser(
      (1u << WORD) | (1u << READING) | (1u << SENTENCE) | (1u << DEFINITION) | (1u << BOOK) | (1u << FURIGANA), 2048);
  char buf[256];
  int n;
  bool done = false;
  while (!done && (n = file.read(buf, sizeof(buf))) > 0) {
    for (int i = 0; i < n && !done; i++) done = parser.feed(buf[i]);
  }
  if (!done) done = parser.finish();
  if (!done) return false;
  out.word = parser.field(WORD);
  out.reading = parser.field(READING);
  out.sentence = parser.field(SENTENCE);
  out.definition = parser.field(DEFINITION);
  out.book = parser.field(BOOK);
  // A file that could not be upgraded still has Tags here.
  out.furigana = upgraded ? parser.field(FURIGANA) : std::string();
  return true;
}

bool Session::currentIsNew() const { return !finished() && find(queue[position].id) == nullptr; }

void Session::previews(uint16_t out[4]) const {
  const CardState* s = finished() ? nullptr : find(queue[position].id);
  const fsrs::Memory memory = s ? fsrs::Memory{s->stability, s->difficulty} : fsrs::Memory{};
  const float elapsed = s ? static_cast<float>(day - s->lastReview) : 0;
  fsrs::previewIntervals(memory, s == nullptr, elapsed, schedulingParams(), out);
}

void Session::rate(const fsrs::Rating rating) {
  if (finished()) return;
  const QueueItem item = queue[position++];
  CardState* s = find(item.id);
  if (!s) {
    // Room for this session's new cards was reserved at open: never grow the table here.
    if (states.size() >= MAX_CARDS || states.size() >= states.capacity()) {
      LOG_ERR("FLASH", "No room for another card's progress; not kept");
      return;
    }
    CardState fresh;
    fresh.id = item.id;
    const auto at = std::lower_bound(states.begin(), states.end(), item.id,
                                     [](const CardState& c, const uint64_t key) { return c.id < key; });
    s = &*states.insert(at, fresh);
    newToday++;
  }
  const bool isNew = s->reps == 0;
  const fsrs::Memory before{s->stability, s->difficulty};
  const fsrs::Memory after =
      isNew ? fsrs::initial(rating) : fsrs::next(before, static_cast<float>(day - s->lastReview), rating);
  s->stability = after.stability;
  s->difficulty = after.difficulty;
  s->lastReview = day;
  s->reps++;
  if (rating == fsrs::Rating::Again) {
    if (!isNew) s->lapses++;
    s->due = day;
    // Seen again a few cards later this session.
    const size_t at = std::min(queue.size(), position + 4);
    if (queue.size() < MAX_QUEUE + 50) queue.insert(queue.begin() + static_cast<long>(at), item);
  } else {
    s->due = static_cast<uint16_t>(day + fsrs::intervalDays(after.stability, schedulingParams()));
  }
  ratingCounts[static_cast<int>(rating) - 1]++;
  reviewedCount++;
  // Saved every few ratings rather than each one: an SD write per card would be the slowest
  // part of reviewing. The session saves again when it ends.
  if (++unsaved >= 5) save();
}

bool Session::save() {
  if (unsaved == 0) return true;
  if (!writeStates(lang, states, day, newToday)) return false;
  unsaved = 0;
  return true;
}

bool onVacation() { return SETTINGS.flashcardVacation != 0; }

void syncVacation() {
  const std::string path = vacationPath();
  const bool recorded = Storage.exists(path.c_str());
  const int32_t now = today();
  if (onVacation()) {
    if (recorded) return;
    // The start day, so the end can move every due date forward by the days away.
    HalFile file;
    if (now < 0 || !Storage.openFileForWrite("FLASH", path, file)) {
      LOG_ERR("FLASH", "Vacation start not recorded yet (clock %ld)", static_cast<long>(now));
      return;
    }
    const uint16_t start = static_cast<uint16_t>(now);
    if (file.write(reinterpret_cast<const uint8_t*>(&start), sizeof(start)) != sizeof(start)) {
      file.close();
      Storage.remove(path.c_str());  // an empty record would end the vacation with no shift
      LOG_ERR("FLASH", "Vacation start not recorded");
    }
    return;
  }
  if (!recorded) return;
  uint16_t start = 0;
  {
    HalFile file;
    if (!Storage.openFileForRead("FLASH", path, file)) return;
    if (file.read(reinterpret_cast<uint8_t*>(&start), sizeof(start)) != static_cast<int>(sizeof(start))) start = 0;
  }
  // Without a clock the length of the trip is unknown: keep the record and try again later.
  if (now < 0) return;
  const int shift = now - static_cast<int>(start);
  if (start != 0 && shift > 0) {
    std::vector<std::string> languages;
    deckLanguages(languages);
    std::vector<CardState> states;
    uint16_t day = 0, newToday = 0;
    // Every deck must load before any is written: a deck skipped now would never be shifted,
    // and a retry after a partial shift would move the others twice.
    for (const auto& language : languages) {
      if (!loadStates(language, states, day, newToday)) {
        LOG_ERR("FLASH", "Vacation end postponed: %s progress could not be read", language.c_str());
        return;
      }
    }
    for (const auto& language : languages) {
      if (!loadStates(language, states, day, newToday) || states.empty()) continue;
      for (auto& s : states) {
        s.due = static_cast<uint16_t>(std::min<int>(s.due + shift, UINT16_MAX));
        s.lastReview = static_cast<uint16_t>(std::min<int>(s.lastReview + shift, UINT16_MAX));
      }
      if (!writeStates(language, states, day, newToday))
        LOG_ERR("FLASH", "Vacation shift lost for %s", language.c_str());
    }
    LOG_INF("FLASH", "Vacation over: %d days added to every due date", shift);
  }
  Storage.remove(path.c_str());
}

}  // namespace flashcards
