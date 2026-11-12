#pragma once

#include <Fsrs.h>

#include <cstdint>
#include <string>
#include <vector>

// Flashcards built from the sentence-mining export: one deck per /sentence-mining/sentences-<lang>.csv,
// one card per row. The CSV stays the user's (Anki imports it); review progress lives beside it in
// the system folder, flashcards-<lang>.bin, so clearing the reading cache never loses it.
namespace flashcards {

constexpr const char* MINING_DIR = "/sentence-mining";

// Review state for one card, keyed by its CSV GUID. Days count from 2020-01-01 so they fit 16 bits.
struct CardState {
  uint64_t id = 0;
  float stability = 0;
  float difficulty = 0;
  uint16_t due = 0;
  uint16_t lastReview = 0;
  uint16_t reps = 0;
  uint8_t lapses = 0;
  uint8_t flags = 0;
};

struct DeckSummary {
  std::string language;  // file's language code, e.g. "ja"
  uint16_t total = 0;
  uint16_t due = 0;
  uint16_t fresh = 0;  // new cards available today, after the daily limit
};

// What a card shows, read from its CSV row. Sentence and definition keep the export's HTML.
struct CardText {
  std::string word;
  std::string reading;
  std::string sentence;
  std::string definition;
  std::string book;
  std::string furigana;  // Anki furigana syntax; empty when unknown
};

// The user's scheduling settings (Settings > Flashcards).
fsrs::Params schedulingParams();
uint16_t newCardsPerDay();

// Today as a day number since 2020-01-01; -1 when the clock was never set.
int32_t today();

// One summary per deck file, in name order.
void listDecks(std::vector<DeckSummary>& out);

// Vacation mode (Settings > Flashcards): while on, nothing comes due and no new cards are added.
bool onVacation();
// Brings the vacation record in line with the setting, however the setting changed (device
// settings, web settings, a restored file). Its start day is recorded when it turns on; when it
// turns off, every card's due date and last review move forward by the days away, so reviews
// resume where they stopped instead of piling up. Runs when the setting changes and whenever
// Flashcards opens a deck or lists them.
void syncVacation();

class Session {
 public:
  // Most cards a deck may hold: the state table is 24 bytes per card in RAM.
  static constexpr size_t MAX_CARDS = 2000;
  static constexpr size_t MAX_QUEUE = 300;

  // Loads the deck's progress and queues today's cards: due reviews first, then new ones.
  bool open(const std::string& language);
  // Writes progress if anything changed since the last save.
  bool save();

  bool finished() const { return position >= queue.size(); }
  size_t remaining() const { return finished() ? 0 : queue.size() - position; }
  size_t reviewed() const { return reviewedCount; }
  size_t total() const { return queue.size(); }
  const std::string& language() const { return lang; }

  // The card at the front of the queue; false when its row could not be read.
  bool current(CardText& out) const;
  bool currentIsNew() const;
  // Interval each rating would give (days; Again = 0, shown again this session).
  void previews(uint16_t out[4]) const;
  void rate(fsrs::Rating rating);
  uint16_t ratingCount(fsrs::Rating rating) const { return ratingCounts[static_cast<int>(rating) - 1]; }

 private:
  struct QueueItem {
    uint64_t id;
    uint32_t offset;  // byte offset of the card's CSV row
  };

  CardState* find(uint64_t id);
  const CardState* find(uint64_t id) const;
  bool load();

  std::string lang;
  std::string csvPath;
  bool upgraded = false;          // the file has the Furigana column
  std::vector<CardState> states;  // sorted by id
  std::vector<QueueItem> queue;
  size_t position = 0;
  size_t reviewedCount = 0;
  uint16_t ratingCounts[4] = {0, 0, 0, 0};
  uint16_t day = 0;
  uint16_t newToday = 0;
  uint8_t unsaved = 0;
};

}  // namespace flashcards
