#include "FlashcardStore.h"

#include <Arduino.h>
#include <FlashcardCsv.h>
#include <HalStorage.h>
#include <Logging.h>
#include <SdSystemDir.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <iterator>
#include <random>

#include "AnkiDeck.h"

static_assert(ankideck::DeckInfo::MAX_STEPS < flashcards::CardState::LEARN_STEP,
              "a step index plus one must fit in CardState::LEARN_STEP");
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

CardState decodeState(const uint8_t* q) {
  CardState s;
  s.id = take<uint64_t>(q);
  s.stability = take<float>(q);
  s.difficulty = take<float>(q);
  s.due = take<uint16_t>(q);
  s.lastReview = take<uint16_t>(q);
  s.reps = take<uint16_t>(q);
  s.lapses = take<uint8_t>(q);
  s.flags = take<uint8_t>(q);
  return s;
}

// `spare` reserves room for cards rated for the first time, so rating never grows the vector.
// std::vector allocates with throwing new (abort on failure), so the block is checked first.
// Room for the table and the cards rated this session: a full vector would refuse them.
bool reserveStates(std::vector<CardState>& out, const size_t cards, const std::string& path) {
  const size_t bytes = cards * sizeof(CardState);
  constexpr size_t HEADROOM = 8 * 1024;  // left for the render and SD paths after the table
  if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < bytes + HEADROOM) {
    LOG_ERR("FLASH", "OOM: %u bytes for %s progress", static_cast<unsigned>(bytes), path.c_str());
    return false;
  }
  out.reserve(cards);
  return true;
}

bool loadStates(const std::string& language, std::vector<CardState>& out, uint16_t& day, uint16_t& newToday,
                const size_t spare = 0) {
  out.clear();
  day = 0;
  newToday = 0;
  HalFile file;
  const std::string path = statePath(language);
  if (!Storage.exists(path.c_str())) return reserveStates(out, spare, path);  // no reviews yet
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
    return reserveStates(out, spare, path);
  }
  const uint8_t* p = header + 4;
  day = take<uint16_t>(p);
  newToday = take<uint16_t>(p);
  const uint32_t count = std::min<uint32_t>(take<uint32_t>(p), Session::MAX_CARDS);
  if (!reserveStates(out, count + spare, path)) return false;
  uint8_t rec[RECORD_BYTES];
  for (uint32_t i = 0; i < count; i++) {
    if (file.read(rec, sizeof(rec)) != static_cast<int>(sizeof(rec))) {
      LOG_ERR("FLASH", "%s ends early (%u of %u cards)", path.c_str(), static_cast<unsigned>(i),
              static_cast<unsigned>(count));
      out.clear();
      return false;
    }
    out.push_back(decodeState(rec));
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

std::string logPath(const std::string& deck) { return sdsystem::path(("flashcards-" + deck + ".log").c_str()); }

// When each card between learning steps is due, by the wall clock: 16 bytes per card (id, due
// seconds since 1970). Written with the progress; a card without an entry is due at once.
std::string stepsPath(const std::string& deck) { return sdsystem::path(("flashcards-" + deck + ".steps").c_str()); }

bool clockValid(const time_t now) { return now > 1577836800; }  // 2020-01-01

constexpr size_t LOG_BYTES = 24;

void encodeLogEntry(uint8_t* rec, const ReviewLogEntry& e) {
  uint8_t* q = rec;
  put(q, e.id);
  put(q, e.timeMs);
  put(q, e.interval);
  put(q, e.previous);
  put(q, e.rating);
  put(q, e.kind);
  put<uint16_t>(q, 0);
}

// Appends the ratings since the last save. Only a successful save drops them from RAM.
bool appendLog(const std::string& deck, const std::vector<ReviewLogEntry>& entries) {
  if (entries.empty()) return true;
  HalFile file;
  if (!Storage.openFileForAppend("FLASH", logPath(deck).c_str(), file)) return false;
  const uint64_t start = file.fileSize64();
  uint8_t rec[LOG_BYTES];
  for (const auto& e : entries) {
    encodeLogEntry(rec, e);
    if (file.write(rec, sizeof(rec)) != sizeof(rec)) {
      // A partial record would shift every later one: drop the whole batch, retried at the next save.
      file.truncate(start);
      return false;
    }
  }
  return true;
}

// Mined cards handed over to Anki: their CSV ids, 8 bytes each, sorted.
std::string handedPath(const std::string& language) {
  return sdsystem::path(("flashcards-" + language + ".anki").c_str());
}

bool loadHanded(const std::string& language, std::vector<uint64_t>& out) {
  out.clear();
  const std::string path = handedPath(language);
  if (!Storage.exists(path.c_str())) return true;
  HalFile file;
  if (!Storage.openFileForRead("FLASH", path, file)) return false;
  const size_t count = file.fileSize() / sizeof(uint64_t);
  if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < count * sizeof(uint64_t) + 8 * 1024) {
    LOG_ERR("FLASH", "OOM: %u handed-over ids for %s", static_cast<unsigned>(count), language.c_str());
    return false;
  }
  out.resize(count);
  if (count > 0 && file.read(reinterpret_cast<uint8_t*>(out.data()), count * sizeof(uint64_t)) !=
                       static_cast<int>(count * sizeof(uint64_t))) {
    out.clear();
    return false;
  }
  return true;
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

fsrs::Params schedulingParams(const ankideck::DeckInfo* deck) {
  static constexpr float RETENTION[] = {0.80f, 0.85f, 0.90f, 0.95f};
  static constexpr uint16_t MAX_DAYS[] = {30, 180, 365, 36500};
  fsrs::Params p;
  p.retention = RETENTION[std::min<uint8_t>(SETTINGS.flashcardRetention, 3)];
  p.maxInterval = MAX_DAYS[std::min<uint8_t>(SETTINGS.flashcardMaxInterval, 3)];
  if (deck && deck->retention > 0) p.retention = std::clamp(deck->retention, 0.70f, 0.99f);
  if (deck && deck->maxInterval > 0) p.maxInterval = deck->maxInterval;
  return p;
}

uint16_t newCardsPerDay(const ankideck::DeckInfo* deck) {
  static constexpr uint16_t PER_DAY[] = {5, 10, 20, 30, 50};
  if (deck && deck->newPerDay >= 0) return static_cast<uint16_t>(std::min<int32_t>(deck->newPerDay, 9999));
  return PER_DAY[std::min<uint8_t>(SETTINGS.flashcardNewPerDay, 4)];
}

int32_t today() {
  const int32_t d = dayNumber(sentencemining::today());
  if (d >= EPOCH_2020) lastKnownDay = d - EPOCH_2020;
  return lastKnownDay;
}

namespace {
// Card ids in a deck's list of leeches not yet sent to Anki, sorted; at most 512.
void readLeeches(const std::string& deckId, std::vector<uint64_t>& out) {
  out.clear();
  const std::string path = ankideck::leechPath(deckId);
  if (!Storage.exists(path.c_str())) return;
  HalFile file;
  if (!Storage.openFileForRead("FLASH", path, file)) return;
  const size_t count = std::min<size_t>(file.fileSize() / 16, 512);
  if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < count * sizeof(uint64_t) + 8 * 1024) return;
  out.reserve(count);
  uint8_t rec[16];
  while (out.size() < count && file.read(rec, sizeof(rec)) == static_cast<int>(sizeof(rec))) {
    uint64_t id = 0;
    memcpy(&id, rec, 8);
    out.push_back(id);
  }
  std::sort(out.begin(), out.end());
}

// Reviews of known cards done today: cards last rated today, less those first learned today.
uint16_t reviewsDoneToday(const std::vector<CardState>& states, const uint16_t day, const uint16_t newToday) {
  const auto rated = static_cast<uint16_t>(std::count_if(
      states.begin(), states.end(), [day](const CardState& c) { return c.reps > 0 && c.lastReview == day; }));
  return rated > newToday ? rated - newToday : 0;
}

// The same, from the number of cards rated today.
uint16_t reviewsLeftAfter(const ankideck::DeckInfo* deck, const size_t ratedToday, const uint16_t newToday) {
  if (!deck || deck->reviewsPerDay < 0) return UINT16_MAX;
  const uint16_t done =
      ratedToday > newToday ? static_cast<uint16_t>(std::min<size_t>(ratedToday - newToday, UINT16_MAX)) : 0;
  return deck->reviewsPerDay > done ? static_cast<uint16_t>(std::min<int32_t>(deck->reviewsPerDay - done, UINT16_MAX))
                                    : 0;
}

uint16_t reviewsLeft(const ankideck::DeckInfo* deck, const std::vector<CardState>& states, const uint16_t day,
                     const uint16_t newToday) {
  if (!deck || deck->reviewsPerDay < 0) return UINT16_MAX;
  const uint16_t done = reviewsDoneToday(states, day, newToday);
  return deck->reviewsPerDay > done ? static_cast<uint16_t>(std::min<int32_t>(deck->reviewsPerDay - done, UINT16_MAX))
                                    : 0;
}
// A deck's progress file read in order, a few records at a time. Files are written sorted by card
// id; one found out of order fails, and the caller loads the whole table instead.
class StateStream {
 public:
  // Back to the first card. False when the file cannot be read or is not a progress file; a deck
  // with no file yet has no cards to give.
  bool open(const std::string& language) {
    if (file.isOpen()) file.close();
    day = newToday = 0;
    left = 0;
    have = pos = 0;
    seen = error = false;
    if (absent) return true;
    const std::string path = statePath(language);
    // A deck with no reviews yet has no file. Asked once: its passes do not look again.
    if (!Storage.exists(path.c_str())) {
      absent = true;
      return true;
    }
    if (!Storage.openFileForRead("FLASH", path, file)) return false;
    uint8_t header[HEADER_BYTES];
    if (file.read(header, sizeof(header)) != static_cast<int>(sizeof(header)) || memcmp(header, MAGIC, 4) != 0) {
      return false;
    }
    const uint8_t* p = header + 4;
    day = take<uint16_t>(p);
    newToday = take<uint16_t>(p);
    left = std::min<uint32_t>(take<uint32_t>(p), Session::MAX_CARDS);
    return true;
  }
  // The next card, ids rising. False at the end, and on a short read or an id out of order, which
  // failed() then reports.
  bool next(CardState& out) {
    if (pos == have) {
      if (left == 0) return false;
      const size_t n = std::min<size_t>(CHUNK, left);
      if (file.read(buf, n * RECORD_BYTES) != static_cast<int>(n * RECORD_BYTES)) return fail();
      have = n;
      pos = 0;
      left -= n;
    }
    out = decodeState(buf + pos++ * RECORD_BYTES);
    if (seen && out.id <= lastId) return fail();
    seen = true;
    lastId = out.id;
    return true;
  }
  bool failed() const { return error; }
  uint16_t day = 0;
  uint16_t newToday = 0;

 private:
  bool fail() {
    error = true;
    left = 0;
    have = pos = 0;
    return false;
  }
  static constexpr size_t CHUNK = 8;
  HalFile file;
  uint32_t left = 0;
  size_t have = 0;
  size_t pos = 0;
  uint64_t lastId = 0;
  bool seen = false;
  bool error = false;
  bool absent = false;  // no progress file: every open after the first gives no cards
  uint8_t buf[CHUNK * RECORD_BYTES] = {};
};

// A deck's progress for countAnkiDeck(), from the table in memory.
struct TableStates {
  const std::vector<CardState>& states;
  bool rewind() { return true; }
  const CardState* find(const uint64_t id) { return findIn(states, id); }
  template <typename Each>
  bool each(Each fn) {
    for (const auto& s : states) fn(s);
    return true;
  }
  bool failed() const { return false; }
};

// The same from the file, without the table: a 2000-card table needs a 48 KB block, which a
// fragmented heap often lacks. find() takes rising ids within a pass; rewind() starts the next.
struct StreamedStates {
  const std::string& language;
  StateStream stream;
  CardState current;
  bool has = false;
  bool atStart = false;  // rewound and nothing asked yet: each() need not open the file again
  bool rewind() {
    has = false;
    atStart = false;
    if (!stream.open(language)) return false;
    has = stream.next(current);
    atStart = !stream.failed();
    return atStart;
  }
  const CardState* find(const uint64_t id) {
    atStart = false;
    while (has && current.id < id) has = stream.next(current);
    return has && current.id == id ? &current : nullptr;
  }
  template <typename Each>
  bool each(Each fn) {
    if (!atStart && !rewind()) return false;
    atStart = false;
    for (; has; has = stream.next(current)) fn(current);
    return !stream.failed();
  }
  bool failed() const { return stream.failed(); }
};

// An Anki deck's counts into `deck`, its progress from `states` (TableStates or StreamedStates).
// False when the progress or the card index cannot be read.
template <typename States>
bool countAnkiDeck(const ankideck::DeckInfo& info, const uint16_t now, const uint16_t newToday, States& states,
                   DeckSummary& deck) {
  size_t reviewedToday = 0;
  if (!states.each([&](const CardState& c) {
        if (c.reps > 0 && c.lastReview == now) reviewedToday++;
      })) {
    return false;
  }
  const uint16_t perDay = newCardsPerDay(&info);
  uint16_t fresh = 0;
  uint16_t stepping = 0;  // between learning steps: outside the review limit, as in a session
  std::vector<uint64_t> buried, leeches, notesDone;
  ankideck::readBuried(info.id, now, buried);
  if (info.leechThreshold > 0 && info.leechSuspend) readLeeches(info.id, leeches);
  const bool bury = (info.buryNew || info.buryReviews) && reviewedToday > 0 &&
                    heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) >= reviewedToday * sizeof(uint64_t) + 8 * 1024;
  // A first pass over the index finds the notes reviewed today, whose siblings the second skips.
  if (bury) {
    notesDone.reserve(reviewedToday);
    if (!states.rewind() || !ankideck::scanIndex(info.id, [&](const ankideck::CardRecord& c) {
          const CardState* s = states.find(c.cardId);
          if (s && s->reps > 0 && s->lastReview == now && notesDone.size() < reviewedToday) {
            notesDone.push_back(c.noteId);
          }
        })) {
      return false;
    }
    if (states.failed()) return false;
    std::sort(notesDone.begin(), notesDone.end());
  }
  if (!states.rewind() || !ankideck::scanIndex(info.id, [&](const ankideck::CardRecord& c) {
        deck.total++;
        if (std::binary_search(buried.begin(), buried.end(), c.cardId)) return;
        if (std::binary_search(leeches.begin(), leeches.end(), c.cardId)) return;
        const CardState* known = states.find(c.cardId);
        // A sibling of a note reviewed today waits for tomorrow, as in a session.
        if (std::binary_search(notesDone.begin(), notesDone.end(), c.noteId) &&
            !(known && known->reps > 0 && known->lastReview == now) && (known ? info.buryReviews : info.buryNew)) {
          return;
        }
        if (known) {
          if (known->due > now) return;
          if ((known->flags & CardState::LEARN_STEP) != 0) {
            stepping++;
          } else {
            deck.due++;
          }
        } else {
          fresh++;
        }
      })) {
    return false;
  }
  if (states.failed()) return false;
  deck.fresh = std::min<uint16_t>(fresh, perDay > newToday ? perDay - newToday : 0);
  deck.due = std::min(deck.due, reviewsLeftAfter(&info, reviewedToday, newToday)) + stepping;
  return true;
}
}  // namespace

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
    std::vector<uint64_t> handed;
    loadHanded(language, handed);  // unreadable: every card counts, as before a hand-over
    CsvRecordParser parser(1u << GUID, 32);
    scanCsv(deckPath(language), parser, [&](const CsvRecordParser& row) {
      const uint64_t id = guidToId(row.field(GUID));
      if (id == 0 || std::binary_search(handed.begin(), handed.end(), id)) return true;
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

  // Decks brought from Anki: counted from their card index instead of a CSV.
  std::vector<ankideck::DeckInfo> anki;
  ankideck::list(anki);
  out.reserve(out.size() + anki.size());
  for (const auto& info : anki) {
    const std::string key = ankideck::keyFor(info.id);
    const auto fresh = [&] {
      DeckSummary d;
      d.language = key;
      d.label = info.name;
      return d;
    };
    DeckSummary deck = fresh();
    StreamedStates streamed{key};
    bool counted = false;
    if (streamed.rewind()) {
      const uint16_t now = todayOr(streamed.stream.day);
      counted = countAnkiDeck(info, now, now != streamed.stream.day ? 0 : streamed.stream.newToday, streamed, deck);
    }
    if (!counted) {
      // Not readable in order (or not a progress file, which loadStates sets aside): the table, as
      // the counts were taken before. Without it, or without the index, the deck shows none due.
      LOG_DBG("FLASH", "%s: counting from the table", key.c_str());
      deck = fresh();
      std::vector<CardState> states;
      uint16_t day = 0, newToday = 0;
      if (loadStates(key, states, day, newToday)) {
        const uint16_t now = todayOr(day);
        TableStates table{states};
        counted = countAnkiDeck(info, now, now != day ? 0 : newToday, table, deck);
      }
      if (!counted) deck = fresh();
    }
    if (paused) deck.due = deck.fresh = 0;
    out.push_back(std::move(deck));
  }
}

CardState* Session::find(const uint64_t id) { return const_cast<CardState*>(findIn(states, id)); }

const CardState* Session::find(const uint64_t id) const { return findIn(states, id); }

bool Session::open(const std::string& language) {
  lang = language;
  anki = ankideck::isAnkiKey(language);
  csvPath = anki ? std::string() : deckPath(language);
  // Record offsets are taken after this, so they point into the upgraded file.
  upgraded = !anki && sentencemining::upgradeExport(csvPath);
  queue.clear();
  learning.clear();
  activeLearn = -1;
  position = 0;
  reviewedCount = 0;
  std::fill(std::begin(ratingCounts), std::end(ratingCounts), 0);
  syncVacation();
  ankideck::DeckInfo info;
  const bool preset = anki && ankideck::readInfo(ankideck::idOf(language), info);
  params = schedulingParams(preset ? &info : nullptr);
  perDay = newCardsPerDay(preset ? &info : nullptr);
  // Anki's steps where the deck's preset gave them; a single ten-minute step otherwise.
  learnCount = relearnCount = 1;
  learnSteps[0] = relearnSteps[0] = 10;
  if (preset && info.learnCount != 0xFF) {
    learnCount = info.learnCount;
    std::copy(info.learnSteps, info.learnSteps + learnCount, learnSteps);
  }
  if (preset && info.relearnCount != 0xFF) {
    relearnCount = info.relearnCount;
    std::copy(info.relearnSteps, info.relearnSteps + relearnCount, relearnSteps);
  }
  buryNew = preset && info.buryNew;
  buryReviews = preset && info.buryReviews;
  std::fill(std::begin(easyDays), std::end(easyDays), static_cast<uint8_t>(100));
  if (preset) std::copy(info.easyDays, info.easyDays + 7, easyDays);
  leechThreshold = preset ? info.leechThreshold : 0;
  leechSuspend = !preset || info.leechSuspend;
  notesDone.clear();
  leeches.clear();
  if (preset && info.leechThreshold > 0 && info.leechSuspend) readLeeches(ankideck::idOf(language), leeches);
  if (!loadStates(language, states, day, newToday, MAX_QUEUE)) return false;
  const uint16_t now = todayOr(day);
  if (now != day) newToday = 0;
  day = now;
  if (onVacation()) return true;  // progress loaded, nothing queued
  {
    // Notes of the cards reviewed today, and room for this session's.
    const auto reviewedToday = static_cast<size_t>(std::count_if(
        states.begin(), states.end(), [this](const CardState& c) { return c.reps > 0 && c.lastReview == day; }));
    // std::vector reserves with throwing new: without room, burying is skipped this session.
    if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) >= (reviewedToday + 64) * sizeof(uint64_t) + 8 * 1024) {
      notesDone.reserve(reviewedToday + 64);
    } else {
      buryNew = buryReviews = false;
    }
  }

  std::vector<QueueItem> fresh;
  const size_t newLimit = perDay > newToday ? perDay - newToday : 0;
  queue.reserve(64);
  pendingLog.reserve(8);  // ratings between two saves
  fresh.reserve(std::min<size_t>(newLimit, MAX_QUEUE));
  learning.reserve(32);  // cards between steps: at most 64, 24 bytes each
  // When the saved steps fall due, by the wall clock (see stepsPath).
  std::vector<std::pair<uint64_t, int64_t>> stepDue;
  {
    HalFile file;
    if (Storage.exists(stepsPath(language).c_str()) && Storage.openFileForRead("FLASH", stepsPath(language), file)) {
      stepDue.reserve(64);
      uint8_t rec[16];
      while (stepDue.size() < 64 && file.read(rec, sizeof(rec)) == static_cast<int>(sizeof(rec))) {
        uint64_t id = 0;
        int64_t due = 0;
        memcpy(&id, rec, 8);
        memcpy(&due, rec + 8, 8);
        stepDue.emplace_back(id, due);
      }
    }
  }
  const time_t wallNow = time(nullptr);
  const auto stepDueMs = [&](const uint64_t id) {
    const uint32_t ms = millis();
    if (!clockValid(wallNow)) return ms;
    for (const auto& d : stepDue) {
      if (d.first != id) continue;
      const int64_t wait = d.second - static_cast<int64_t>(wallNow);
      return wait > 0 ? static_cast<uint32_t>(ms + std::min<int64_t>(wait, 86400) * 1000) : ms;
    }
    return ms;
  };
  const auto consider = [&](const uint64_t id, const uint32_t offset, const uint64_t note = 0) {
    // A leech the next sync suspends in Anki is not shown meanwhile.
    if (std::binary_search(leeches.begin(), leeches.end(), id)) return;
    if (const CardState* s = find(id)) {
      // Between learning steps when the last session ended: it goes on from its saved step, due
      // now (the time of day is not kept), outside the review limit.
      const uint8_t saved = s->flags & CardState::LEARN_STEP;
      const bool relearn = (s->flags & CardState::LEARN_RELEARN) != 0;
      const uint8_t count = relearn ? relearnCount : learnCount;  // the preset may have changed since
      if (saved > 0 && count > 0 && s->due <= day && learning.size() < 64) {
        const uint8_t step = std::min<uint8_t>(saved - 1, count - 1);
        learning.push_back({{id, offset, note}, stepDueMs(id), step, relearn});
        return;
      }
      if (s->due <= day && queue.size() < MAX_QUEUE) queue.push_back({id, offset, note});
    } else if (fresh.size() < newLimit && fresh.size() < MAX_QUEUE) {
      fresh.push_back({id, offset, note});
    }
  };
  if (anki) {
    // An Anki deck's cards come from its index, in Anki's order; offset holds the record's flags.
    std::vector<uint64_t> buriedIds;
    ankideck::readBuried(ankideck::idOf(language), day, buriedIds);
    const bool read = ankideck::scanIndex(ankideck::idOf(language), [&](const ankideck::CardRecord& c) {
      if (std::binary_search(buriedIds.begin(), buriedIds.end(), c.cardId)) return;
      // Notes already reviewed today bury their siblings for the rest of the day.
      const CardState* s = find(c.cardId);
      if ((buryNew || buryReviews) && s && s->reps > 0 && s->lastReview == day) notesDone.push_back(c.noteId);
      consider(c.cardId, c.flags, c.noteId);
    });
    if (!read) {
      queue.clear();  // as before: a session that did not open has nothing to show
      learning.clear();
      return false;
    }
    std::sort(notesDone.begin(), notesDone.end());
  } else {
    // Cards handed over to Anki are reviewed in its deck now.
    std::vector<uint64_t> handed;
    loadHanded(lang, handed);
    CsvRecordParser parser(1u << GUID, 32);
    const bool found = scanCsv(csvPath, parser, [&](const CsvRecordParser& row) {
      const uint64_t id = guidToId(row.field(GUID));
      if (id != 0 && !std::binary_search(handed.begin(), handed.end(), id)) consider(id, row.recordStart());
      return true;
    });
    if (!found) return false;
  }
  // Most overdue first; Anki's daily review limit keeps the most overdue.
  std::stable_sort(queue.begin(), queue.end(),
                   [this](const QueueItem& a, const QueueItem& b) { return find(a.id)->due < find(b.id)->due; });
  const uint16_t allowed = reviewsLeft(preset ? &info : nullptr, states, day, newToday);
  if (queue.size() > allowed) queue.resize(allowed);
  // Then the preset's order: of the reviews kept, and of the new cards (as added, or random).
  const uint8_t order = preset ? info.reviewOrder : ankideck::DeckInfo::REVIEW_DUE;
  std::minstd_rand shuffle(static_cast<uint32_t>(millis()) ^ static_cast<uint32_t>(time(nullptr)));
  if (order == ankideck::DeckInfo::REVIEW_RANDOM) {
    std::shuffle(queue.begin(), queue.end(), shuffle);
  } else if (order == ankideck::DeckInfo::REVIEW_INTERVAL_ASC || order == ankideck::DeckInfo::REVIEW_INTERVAL_DESC) {
    const bool asc = order == ankideck::DeckInfo::REVIEW_INTERVAL_ASC;
    std::stable_sort(queue.begin(), queue.end(), [this, asc](const QueueItem& a, const QueueItem& b) {
      const int ia = find(a.id)->due - find(a.id)->lastReview;
      const int ib = find(b.id)->due - find(b.id)->lastReview;
      return asc ? ia < ib : ia > ib;
    });
  }
  if (preset && info.newRandom) std::shuffle(fresh.begin(), fresh.end(), shuffle);
  const uint8_t mix = preset ? info.newMix : ankideck::DeckInfo::NEW_AFTER;
  // The queue holds MAX_QUEUE at most: the side placed last is cut first, as Anki does.
  if (mix == ankideck::DeckInfo::NEW_BEFORE) {
    if (fresh.size() > MAX_QUEUE) fresh.resize(MAX_QUEUE);
    if (queue.size() + fresh.size() > MAX_QUEUE) queue.resize(MAX_QUEUE - fresh.size());
    queue.insert(queue.begin(), fresh.begin(), fresh.end());
  } else if (mix == ankideck::DeckInfo::NEW_MIXED && !queue.empty() && !fresh.empty()) {
    // Spread the new cards evenly among the reviews.
    if (queue.size() + fresh.size() > MAX_QUEUE) fresh.resize(MAX_QUEUE > queue.size() ? MAX_QUEUE - queue.size() : 0);
    std::vector<QueueItem> merged;
    merged.reserve(queue.size() + fresh.size());
    const size_t every = fresh.empty() ? queue.size() : std::max<size_t>(1, queue.size() / fresh.size());
    size_t r = 0;
    for (const auto& item : fresh) {
      for (size_t k = 0; k < every && r < queue.size(); k++) merged.push_back(queue[r++]);
      merged.push_back(item);
    }
    while (r < queue.size()) merged.push_back(queue[r++]);
    queue.swap(merged);
  } else {
    if (queue.size() + fresh.size() > MAX_QUEUE) fresh.resize(MAX_QUEUE > queue.size() ? MAX_QUEUE - queue.size() : 0);
    queue.insert(queue.end(), fresh.begin(), fresh.end());
  }
  pickNext();
  LOG_INF("FLASH", "Deck %s: %u queued, %u states", language.c_str(), static_cast<unsigned>(queue.size()),
          static_cast<unsigned>(states.size()));
  return true;
}

// A learning card whose step has passed comes first. When nothing else is left, the next one is
// shown early rather than leaving the session waiting.
void Session::pickNext() {
  activeLearn = -1;
  if (!leeches.empty()) {
    learning.erase(std::remove_if(learning.begin(), learning.end(),
                                  [this](const Learning& l) {
                                    return std::binary_search(leeches.begin(), leeches.end(), l.item.id);
                                  }),
                   learning.end());
  }
  while (position < queue.size() &&
         (buried(queue[position]) || std::binary_search(leeches.begin(), leeches.end(), queue[position].id))) {
    position++;
  }
  if (learning.empty()) return;
  int earliest = 0;
  for (size_t i = 1; i < learning.size(); i++) {
    if (static_cast<int32_t>(learning[i].dueMs - learning[earliest].dueMs) < 0) earliest = static_cast<int>(i);
  }
  const bool due = static_cast<int32_t>(millis() - learning[earliest].dueMs) >= 0;
  if (due || position >= queue.size()) activeLearn = earliest;
}

// A sibling of a note reviewed today, held back until tomorrow as the preset asks.
bool Session::buried(const QueueItem& item) const {
  if (item.note == 0 || notesDone.empty()) return false;
  if (!std::binary_search(notesDone.begin(), notesDone.end(), item.note)) return false;
  const CardState* s = find(item.id);
  if (s && s->reps > 0 && s->lastReview == day) return false;  // this very card, again
  return s == nullptr ? buryNew : buryReviews;
}

// Easy Days: within a few percent of the interval, the weekday with the most room is chosen.
uint16_t Session::easyDay(const uint16_t interval) const {
  if (interval < 3 || std::all_of(std::begin(easyDays), std::end(easyDays), [](uint8_t p) { return p == 100; })) {
    return interval;
  }
  const int spread = std::max(1, interval * 5 / 100);
  uint16_t best = interval;
  int bestRoom = -1;
  for (int d = -spread; d <= spread; d++) {
    const int candidate = interval + d;
    if (candidate < 1 || candidate > params.maxInterval) continue;
    // Day 0 is 2020-01-01, a Wednesday; easyDays starts on Monday.
    const int room = easyDays[(day + candidate + 2) % 7];
    if (room > bestRoom || (room == bestRoom && std::abs(d) < std::abs(best - static_cast<int>(interval)))) {
      bestRoom = room;
      best = static_cast<uint16_t>(candidate);
    }
  }
  return best;
}

// A leech: noted for the next sync, which tags it (and suspends it, as the preset says) in Anki.
void Session::markLeech(const QueueItem& item, const CardState& s) {
  if (!anki) return;
  LOG_INF("FLASH", "Leech %llu after %u lapses", static_cast<unsigned long long>(item.id), s.lapses);
  HalFile file;
  if (!Storage.openFileForAppend("FLASH", ankideck::leechPath(ankideck::idOf(lang)).c_str(), file)) return;
  if (leechSuspend && leeches.size() < 512)
    leeches.insert(std::upper_bound(leeches.begin(), leeches.end(), item.id), item.id);
  uint8_t rec[16];
  memcpy(rec, &item.id, 8);
  memcpy(rec + 8, &item.note, 8);
  if (file.write(rec, sizeof(rec)) != sizeof(rec)) LOG_ERR("FLASH", "Leech not noted");
}

int Session::nextStep(const fsrs::Rating rating, const int current, const uint8_t count) {
  int next = -1;
  switch (rating) {
    case fsrs::Rating::Again:
      next = 0;
      break;
    case fsrs::Rating::Hard:
      next = current;
      break;
    case fsrs::Rating::Good:
      next = current + 1;
      break;
    case fsrs::Rating::Easy:
      return -1;
  }
  return next < static_cast<int>(count) ? next : -1;
}

uint16_t Session::stepMinutes(const uint16_t* steps, const uint8_t count, const fsrs::Rating rating, const int current,
                              const int next) {
  if (rating == fsrs::Rating::Hard && current == 0) {
    if (count >= 2) return static_cast<uint16_t>((steps[0] + steps[1]) / 2);
    return static_cast<uint16_t>(std::min<uint32_t>(steps[0] * 3u / 2u, steps[0] + 1440u));
  }
  return steps[next];
}

bool Session::current(CardText& out) const {
  if (finished()) return false;
  if (anki) {
    const std::string deckId = ankideck::idOf(lang);
    const QueueItem& item = active();
    out = CardText{};
    out.frontPath = ankideck::frontPath(deckId, item.id);
    out.backPath = ankideck::backPath(deckId, item.id);
    out.cssPath = ankideck::cssPath(deckId);
    out.tooLarge = (item.offset & ankideck::TOO_LARGE) != 0;
    if (out.tooLarge || Storage.exists(out.frontPath.c_str())) return true;
    // The sync trusted its index for this card: the next one checks every card again.
    if (!cardsSyncForgotten) {
      ankideck::forgetCardsSynced(deckId);
      cardsSyncForgotten = true;
    }
    return false;
  }
  HalFile file;
  if (!Storage.openFileForRead("FLASH", csvPath, file) || !file.seekSet(active().offset)) return false;
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

bool Session::currentIsNew() const { return !finished() && find(active().id) == nullptr; }

void Session::previews(Interval out[4]) const {
  for (int i = 0; i < 4; i++) out[i] = Interval{};
  if (finished()) return;
  const CardState* s = find(active().id);
  const fsrs::Memory memory = s ? fsrs::Memory{s->stability, s->difficulty} : fsrs::Memory{};
  const float elapsed = s ? static_cast<float>(day - s->lastReview) : 0;
  uint16_t days[4];
  fsrs::previewIntervals(memory, s == nullptr, elapsed, params, days);
  const bool inLearning = activeLearn >= 0;
  const bool relearn = inLearning && learning[activeLearn].relearn;
  const int currentStep = inLearning ? learning[activeLearn].step : 0;
  for (int i = 0; i < 4; i++) {
    const auto rating = static_cast<fsrs::Rating>(i + 1);
    out[i].value = easyDay(days[i]);
    // A review card only enters steps when it is forgotten.
    const bool steps = s == nullptr || inLearning || rating == fsrs::Rating::Again;
    if (!steps) continue;
    const bool re = inLearning ? relearn : s != nullptr;
    const uint8_t count = re ? relearnCount : learnCount;
    const int next = nextStep(rating, currentStep, count);
    if (next < 0) {
      if (rating == fsrs::Rating::Again) {
        const fsrs::Memory m = s ? fsrs::next(memory, elapsed, rating) : fsrs::initial(rating);
        out[i].value = easyDay(fsrs::intervalDays(m.stability, params));
      }
      continue;
    }
    const uint16_t minutes = stepMinutes(re ? relearnSteps : learnSteps, count, rating, currentStep, next);
    out[i] = minutes >= 1440 ? Interval{static_cast<uint16_t>(minutes / 1440), false} : Interval{minutes, true};
  }
}

void Session::rate(const fsrs::Rating rating) {
  if (finished()) return;
  const bool fromLearning = activeLearn >= 0;
  Learning learnt{};
  if (fromLearning) {
    learnt = learning[activeLearn];
    learning.erase(learning.begin() + activeLearn);
    activeLearn = -1;
  } else {
    position++;
  }
  const QueueItem item = fromLearning ? learnt.item : queue[position - 1];
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
  const uint16_t previous = isNew ? 0 : static_cast<uint16_t>(std::max(0, s->due - s->lastReview));
  const fsrs::Memory before{s->stability, s->difficulty};
  const fsrs::Memory after =
      isNew ? fsrs::initial(rating) : fsrs::next(before, static_cast<float>(day - s->lastReview), rating);
  s->stability = after.stability;
  s->difficulty = after.difficulty;
  s->lastReview = day;
  s->reps++;
  // New and forgotten cards go through the learning (or relearning) steps before a day interval.
  const bool relearn = fromLearning ? learnt.relearn : (!isNew && rating == fsrs::Rating::Again);
  const bool stepping = fromLearning || isNew || rating == fsrs::Rating::Again;
  if (rating == fsrs::Rating::Again && !isNew && !fromLearning) {
    if (s->lapses < 255) s->lapses++;
    // Anki's rule: a leech at the threshold, and again every half threshold after it.
    const uint8_t t = leechThreshold;
    if (t > 0 && s->lapses >= t && (s->lapses - t) % std::max(1, t / 2) == 0) markLeech(item, *s);
  }
  if (item.note != 0 && (buryNew || buryReviews) &&
      !std::binary_search(notesDone.begin(), notesDone.end(), item.note)) {
    notesDone.insert(std::upper_bound(notesDone.begin(), notesDone.end(), item.note), item.note);
  }
  const int currentStep = fromLearning ? learnt.step : 0;
  const uint8_t count = relearn ? relearnCount : learnCount;
  const int next = stepping ? nextStep(rating, currentStep, count) : -1;
  uint16_t stepMinutesLogged = 0;  // set while the card stays in a minute step
  s->flags &= static_cast<uint8_t>(~(CardState::LEARN_STEP | CardState::LEARN_RELEARN));
  if (next >= 0) {
    const uint16_t minutes = stepMinutes(relearn ? relearnSteps : learnSteps, count, rating, currentStep, next);
    if (minutes < 1440) {
      s->flags |= static_cast<uint8_t>((next + 1) & CardState::LEARN_STEP);
      if (relearn) s->flags |= CardState::LEARN_RELEARN;
    }
    if (minutes >= 1440) {
      s->due = static_cast<uint16_t>(day + minutes / 1440);  // a step of days ends this session for the card
    } else {
      s->due = day;
      stepMinutesLogged = minutes;
      if (learning.size() < 64) {
        learning.push_back(
            {item, static_cast<uint32_t>(millis() + minutes * 60000UL), static_cast<uint8_t>(next), relearn});
      } else if (queue.size() < MAX_QUEUE + 50) {
        queue.insert(queue.begin() + static_cast<long>(std::min(queue.size(), position + 4)), item);
      }
    }
  } else {
    s->due = static_cast<uint16_t>(day + easyDay(fsrs::intervalDays(after.stability, params)));
  }
  // Logged for Anki only with a real clock: a review's time is its identity there.
  const time_t now = time(nullptr);
  if (now > 1577836800) {  // 2020-01-01
    ReviewLogEntry e;
    e.id = item.id;
    e.timeMs = static_cast<int64_t>(now) * 1000 + static_cast<int64_t>(millis() % 1000);
    e.interval = stepMinutesLogged > 0 ? stepMinutesLogged : static_cast<uint16_t>(s->due - day);
    e.previous = previous;
    e.rating = static_cast<uint8_t>(rating);
    e.kind = relearn ? 2 : (stepping ? 0 : 1);
    if (stepMinutesLogged > 0) e.kind |= ReviewLogEntry::MINUTES;
    pendingLog.push_back(e);
  }
  ratingCounts[static_cast<int>(rating) - 1]++;
  reviewedCount++;
  // Saved every few ratings rather than each one: an SD write per card would be the slowest
  // part of reviewing. The session saves again when it ends.
  if (++unsaved >= 5) save();
  pickNext();
}

bool Session::save() {
  if (unsaved == 0) return true;
  if (!writeStates(lang, states, day, newToday)) return false;
  saveStepTimes();
  // After the states: a rating logged without its new state would be sent to Anki twice over.
  if (appendLog(lang, pendingLog)) {
    pendingLog.clear();
  } else {
    LOG_ERR("FLASH", "Review log for %s not written; kept for the next save", lang.c_str());
  }
  unsaved = 0;
  return true;
}

void Session::saveStepTimes() const {
  const std::string path = stepsPath(lang);
  const time_t now = time(nullptr);
  if (!clockValid(now)) return;  // the saved times stay until a clock can replace them
  if (learning.empty()) {
    if (Storage.exists(path.c_str())) Storage.remove(path.c_str());
    return;
  }
  HalFile file;
  if (!Storage.openFileForWrite("FLASH", path, file)) return;
  const uint32_t ms = millis();
  for (const auto& l : learning) {
    // Signed difference: a step already passed is due now.
    const int64_t due = static_cast<int64_t>(now) + std::max<int32_t>(0, static_cast<int32_t>(l.dueMs - ms)) / 1000;
    uint8_t rec[16];
    memcpy(rec, &l.item.id, 8);
    memcpy(rec + 8, &due, 8);
    if (file.write(rec, sizeof(rec)) != sizeof(rec)) {
      LOG_ERR("FLASH", "Step times not saved");
      return;
    }
  }
}

bool applyPulledReviews(const std::string& deck, const std::vector<PulledReview>& reviews) {
  if (reviews.empty()) return true;
  std::vector<CardState> states;
  uint16_t day = 0, newToday = 0;
  if (!loadStates(deck, states, day, newToday, std::min<size_t>(reviews.size(), Session::MAX_QUEUE))) return false;
  for (const auto& r : reviews) {
    if (r.rating < 1 || r.rating > 4 || r.timeMs <= 0) continue;
    const int32_t reviewDay = static_cast<int32_t>(r.timeMs / 86400000) - EPOCH_2020;
    if (reviewDay < 0) continue;
    auto it = std::lower_bound(states.begin(), states.end(), r.id,
                               [](const CardState& c, const uint64_t key) { return c.id < key; });
    if (it == states.end() || it->id != r.id) {
      if (states.size() >= Session::MAX_CARDS || states.size() >= states.capacity()) continue;
      CardState fresh;
      fresh.id = r.id;
      it = states.insert(it, fresh);
    }
    CardState& s = *it;
    // Older than what the device already counted (rated here since): its schedule stands.
    if (s.reps > 0 && reviewDay < s.lastReview) continue;
    const auto rating = static_cast<fsrs::Rating>(r.rating);
    const fsrs::Memory after =
        s.reps == 0 ? fsrs::initial(rating)
                    : fsrs::next({s.stability, s.difficulty}, static_cast<float>(reviewDay - s.lastReview), rating);
    s.stability = after.stability;
    s.difficulty = after.difficulty;
    s.lastReview = static_cast<uint16_t>(reviewDay);
    s.due = static_cast<uint16_t>(reviewDay + r.interval);
    s.flags &= static_cast<uint8_t>(~(CardState::LEARN_STEP | CardState::LEARN_RELEARN));
    if (rating == fsrs::Rating::Again && s.reps > 0) s.lapses++;
    s.reps++;
  }
  return writeStates(deck, states, day, newToday);
}

bool readStates(const std::string& deck, const std::vector<uint64_t>& ids, std::vector<CardState>& out,
                uint16_t& today) {
  out.clear();
  std::vector<CardState> states;
  uint16_t day = 0, newToday = 0;
  if (!loadStates(deck, states, day, newToday)) return false;
  today = todayOr(day);
  out.reserve(ids.size());
  for (const uint64_t id : ids) {
    if (const CardState* s = findIn(states, id)) out.push_back(*s);
  }
  return true;
}

bool readReviewLog(const std::string& deck, std::vector<ReviewLogEntry>& out) {
  out.clear();
  const std::string path = logPath(deck);
  if (!Storage.exists(path.c_str())) return true;
  HalFile file;
  if (!Storage.openFileForRead("FLASH", path, file)) return false;
  // As many as the heap holds, down to a few: the caller sends them and asks again for the rest.
  constexpr size_t HEADROOM = 8 * 1024;
  constexpr size_t MIN_ENTRIES = 8;
  const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  const size_t fits = largest > HEADROOM ? (largest - HEADROOM) / sizeof(ReviewLogEntry) : 0;
  const size_t count = std::min({file.fileSize() / LOG_BYTES, MAX_LOG_ENTRIES, fits});
  if (count < std::min<size_t>(MIN_ENTRIES, file.fileSize() / LOG_BYTES)) {
    LOG_ERR("FLASH", "OOM: %u bytes free for %s", static_cast<unsigned>(largest), path.c_str());
    return false;
  }
  out.reserve(count);
  uint8_t rec[LOG_BYTES];
  for (size_t i = 0; i < count; i++) {
    if (file.read(rec, sizeof(rec)) != static_cast<int>(sizeof(rec))) return false;
    const uint8_t* p = rec;
    ReviewLogEntry e;
    e.id = take<uint64_t>(p);
    e.timeMs = take<int64_t>(p);
    e.interval = take<uint16_t>(p);
    e.previous = take<uint16_t>(p);
    e.rating = take<uint8_t>(p);
    e.kind = take<uint8_t>(p);
    out.push_back(e);
  }
  return true;
}

bool rewriteReviewLog(const std::string& deck, const std::vector<ReviewLogEntry>& oldest) {
  const std::string path = logPath(deck);
  const std::string tmp = path + ".tmp";
  {
    HalFile in, out;
    if (!Storage.openFileForRead("FLASH", path, in) || !Storage.openFileForWrite("FLASH", tmp, out)) return false;
    uint8_t rec[LOG_BYTES];
    for (size_t i = 0; in.read(rec, sizeof(rec)) == static_cast<int>(sizeof(rec)); i++) {
      if (i < oldest.size()) {
        const uint8_t* p = rec;
        if (take<uint64_t>(p) != oldest[i].id) return false;  // not the entries that were read
        encodeLogEntry(rec, oldest[i]);
      }
      if (out.write(rec, sizeof(rec)) != sizeof(rec)) return false;
    }
    // A read error ends the loop early too: the copy would lose the rest of the log.
    if (in.position() != in.fileSize()) return false;
  }
  return Storage.replaceFile(tmp.c_str(), path.c_str());
}

bool dropReviewLog(const std::string& deck, const size_t count) {
  const std::string path = logPath(deck);
  if (!Storage.exists(path.c_str())) return true;
  const std::string tmp = path + ".tmp";
  {
    HalFile in;
    if (!Storage.openFileForRead("FLASH", path, in)) return false;
    const uint64_t skip = static_cast<uint64_t>(count) * LOG_BYTES;
    if (in.fileSize64() <= skip) {
      in.close();
      return Storage.remove(path.c_str());
    }
    HalFile out;
    if (!in.seek64(skip) || !Storage.openFileForWrite("FLASH", tmp, out)) return false;
    uint8_t rec[LOG_BYTES];
    while (in.read(rec, sizeof(rec)) == static_cast<int>(sizeof(rec))) {
      if (out.write(rec, sizeof(rec)) != sizeof(rec)) return false;
    }
    if (in.position() != in.fileSize()) return false;  // a read error, not the end of the log
  }
  return Storage.replaceFile(tmp.c_str(), path.c_str());
}

const char* autonym(const std::string& code) {
  // Korean and Cantonese are left out: the UI font has no Hangul and no 粵.
  struct Autonym {
    const char* code;
    const char* name;
  };
  static constexpr Autonym AUTONYMS[] = {
      {"ja", "日本語"},  {"zh", "中文"},     {"en", "English"},    {"de", "Deutsch"},    {"fr", "Français"},
      {"es", "Español"}, {"it", "Italiano"}, {"pt", "Português"},  {"nl", "Nederlands"}, {"hu", "Magyar"},
      {"pl", "Polski"},  {"ru", "Русский"},  {"uk", "Українська"},
  };
  const auto* const found =
      std::find_if(std::begin(AUTONYMS), std::end(AUTONYMS), [&code](const Autonym& a) { return code == a.code; });
  return found == std::end(AUTONYMS) ? nullptr : found->name;
}

void miningLanguages(std::vector<std::string>& out) { deckLanguages(out); }

size_t newMinedRows(const std::string& language, const std::vector<uint64_t>& skip, const size_t max,
                    std::vector<MinedRow>& out) {
  out.clear();
  out.reserve(max);
  std::vector<uint64_t> handed;
  if (!loadHanded(language, handed)) return 0;
  const std::string path = deckPath(language);
  sentencemining::upgradeExport(path);
  CsvRecordParser parser(static_cast<uint16_t>((1u << COLUMNS) - 1));
  scanCsv(path, parser, [&](const CsvRecordParser& row) {
    const uint64_t id = guidToId(row.field(GUID));
    if (id == 0 || std::binary_search(handed.begin(), handed.end(), id) ||
        std::find(skip.begin(), skip.end(), id) != skip.end()) {
      return true;
    }
    out.emplace_back();  // filled in place: a MinedRow on the stack would pass 256 bytes
    out.back().id = id;
    for (uint8_t c = WORD; c < COLUMNS; c++) out.back().fields[c] = row.field(static_cast<Column>(c));
    return out.size() < max;
  });
  return out.size();
}

// The Anki deck a language's mined words go to: its id, as text (flashcards-<lang>.deck).
std::string minedDeckId(const std::string& language) {
  const std::string path = sdsystem::path(("flashcards-" + language + ".deck").c_str());
  if (!Storage.exists(path.c_str())) return {};
  HalFile file;
  if (!Storage.openFileForRead("FLASH", path, file)) return {};
  char id[24] = {};
  const int n = file.read(reinterpret_cast<uint8_t*>(id), sizeof(id) - 1);
  return n > 0 ? std::string(id, static_cast<size_t>(n)) : std::string();
}

bool setMinedDeckId(const std::string& language, const std::string& deckId) {
  if (minedDeckId(language) == deckId) return true;
  const std::string path = sdsystem::path(("flashcards-" + language + ".deck").c_str());
  const std::string tmp = path + ".tmp";
  {
    HalFile file;
    if (!Storage.openFileForWrite("FLASH", tmp, file) ||
        file.write(reinterpret_cast<const uint8_t*>(deckId.data()), deckId.size()) != deckId.size()) {
      return false;
    }
  }
  return Storage.replaceFile(tmp.c_str(), path.c_str());
}

bool handOver(const std::string& language, const std::string& ankiKey, const std::vector<HandedCard>& cards) {
  if (cards.empty()) return true;
  // 1. Each card's progress, under its Anki card id in the Anki deck's state table.
  std::vector<CardState> from, to;
  uint16_t fromDay = 0, fromNew = 0, toDay = 0, toNew = 0;
  if (!loadStates(language, from, fromDay, fromNew) || !loadStates(ankiKey, to, toDay, toNew, cards.size())) {
    return false;
  }
  std::vector<uint64_t> withProgress;  // Anki card ids whose schedule must reach Anki
  withProgress.reserve(cards.size());
  for (const auto& c : cards) {
    const CardState* s = c.adopted ? nullptr : findIn(from, c.csvId);
    if (!s) continue;  // never reviewed (a new card in Anki too), or Anki's schedule is kept
    withProgress.push_back(c.cardId);
    CardState moved = *s;
    moved.id = c.cardId;
    const auto it = std::lower_bound(to.begin(), to.end(), c.cardId,
                                     [](const CardState& a, const uint64_t key) { return a.id < key; });
    if (it != to.end() && it->id == c.cardId) {
      *it = moved;
    } else {
      to.insert(it, moved);
    }
  }
  if (!writeStates(ankiKey, to, todayOr(toDay), toNew)) return false;
  std::vector<CardState>().swap(from);
  std::vector<CardState>().swap(to);

  // 2. Their review history, into the Anki deck's log: the deck's sync sends it to Anki. A
  // repeat after an interruption sends a review twice, which Anki refuses as a conflict.
  const std::string fromLog = logPath(language);
  if (Storage.exists(fromLog.c_str())) {
    HalFile file;
    if (!Storage.openFileForRead("FLASH", fromLog, file)) return false;
    std::vector<ReviewLogEntry> batch;
    batch.reserve(32);
    uint8_t rec[LOG_BYTES];
    while (file.read(rec, sizeof(rec)) == static_cast<int>(sizeof(rec))) {
      const uint8_t* p = rec;
      ReviewLogEntry e;
      e.id = take<uint64_t>(p);
      e.timeMs = take<int64_t>(p);
      e.interval = take<uint16_t>(p);
      e.previous = take<uint16_t>(p);
      e.rating = take<uint8_t>(p);
      e.kind = take<uint8_t>(p);
      const auto c = std::find_if(cards.begin(), cards.end(), [&e](const HandedCard& h) { return h.csvId == e.id; });
      if (c == cards.end()) continue;
      e.id = c->cardId;
      batch.push_back(e);
      if (batch.size() == 32) {
        if (!appendLog(ankiKey, batch)) return false;
        batch.clear();
      }
    }
    if (!appendLog(ankiKey, batch)) return false;
  }
  // A card reviewed before ratings were logged has progress but no history: its schedule still
  // goes to Anki, through an entry without a rating.
  {
    std::vector<ReviewLogEntry> scheduleOnly;
    scheduleOnly.reserve(withProgress.size());
    for (const uint64_t id : withProgress) {
      ReviewLogEntry e;
      e.id = id;
      e.timeMs = static_cast<int64_t>(time(nullptr)) * 1000;
      scheduleOnly.push_back(e);
    }
    if (!appendLog(ankiKey, scheduleOnly)) return false;
  }

  // 3. Last: from now on the local deck leaves these cards out.
  std::vector<uint64_t> handed;
  if (!loadHanded(language, handed)) return false;
  handed.reserve(handed.size() + cards.size());
  std::transform(cards.begin(), cards.end(), std::back_inserter(handed), [](const HandedCard& c) { return c.csvId; });
  std::sort(handed.begin(), handed.end());
  handed.erase(std::unique(handed.begin(), handed.end()), handed.end());
  const std::string path = handedPath(language);
  const std::string tmp = path + ".tmp";
  {
    HalFile file;
    if (!Storage.openFileForWrite("FLASH", tmp, file)) return false;
    const size_t bytes = handed.size() * sizeof(uint64_t);
    if (file.write(reinterpret_cast<const uint8_t*>(handed.data()), bytes) != bytes) return false;
  }
  return Storage.replaceFile(tmp.c_str(), path.c_str());
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
    const auto unreadable = std::find_if(languages.begin(), languages.end(), [&](const std::string& language) {
      return !loadStates(language, states, day, newToday);
    });
    if (unreadable != languages.end()) {
      LOG_ERR("FLASH", "Vacation end postponed: %s progress could not be read", unreadable->c_str());
      return;
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
