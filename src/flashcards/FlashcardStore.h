#pragma once

#include <Fsrs.h>

#include <cstdint>
#include <string>
#include <vector>

// Flashcards built from the sentence-mining export: one deck per /sentence-mining/sentences-<lang>.csv,
// one card per row. The CSV stays the user's (Anki imports it); review progress lives beside it in
// the system folder, flashcards-<lang>.bin, so clearing the reading cache never loses it.
namespace ankideck {
struct DeckInfo;
}

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
  uint8_t flags = 0;  // LEARN_STEP and LEARN_RELEARN while the card is between learning steps

  // The step the card waits for, plus one (0: not in steps); its time of day is not kept.
  static constexpr uint8_t LEARN_STEP = 0x07;
  static constexpr uint8_t LEARN_RELEARN = 0x08;
};

// One rating, kept until a sync hands it to Anki (flashcards-<deck>.log, 24 bytes each).
struct ReviewLogEntry {
  uint64_t id = 0;                          // the card
  int64_t timeMs = 0;                       // when, ms since 1970; becomes the review's id in Anki
  uint16_t interval = 0;                    // days until it is due again; minutes for a step with MINUTES set
  uint16_t previous = 0;                    // the interval before this review
  uint8_t rating = 0;                       // 1 = Again .. 4 = Easy
  uint8_t kind = 0;                         // 0 learning, 1 review, 2 relearning (Anki's revlog types), | MINUTES
  static constexpr uint8_t MINUTES = 0x80;  // `interval` is a learning step in minutes
};

// The oldest logged reviews of a deck, at most MAX_LOG_ENTRIES; dropReviewLog() removes them once
// Anki has them, and the next read returns the rest.
constexpr size_t MAX_LOG_ENTRIES = 200;
bool readReviewLog(const std::string& deck, std::vector<ReviewLogEntry>& out);
bool dropReviewLog(const std::string& deck, size_t count);

// A review done in Anki, folded into the device's progress by a sync.
struct PulledReview {
  uint64_t id = 0;        // the card
  int64_t timeMs = 0;     // when it was answered
  uint8_t rating = 0;     // 1 = Again .. 4 = Easy
  uint16_t interval = 0;  // days Anki gave it (0 while still learning)
};

// Applies Anki's reviews (oldest first) to a deck's progress: each runs the device's FSRS for the
// memory state, and Anki's own interval sets the due date.
bool applyPulledReviews(const std::string& deck, const std::vector<PulledReview>& reviews);
// The progress of the given cards, for handing back to Anki. Missing cards are left out.
bool readStates(const std::string& deck, const std::vector<uint64_t>& ids, std::vector<CardState>& out,
                uint16_t& today);

struct DeckSummary {
  std::string language;  // the deck key: a mining file's language code ("ja"), or "anki-<id>" for an Anki deck
  std::string label;     // an Anki deck's own name; empty for mining decks
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
  // A card from an Anki deck: its two sides as rendered XHTML files, and the deck's CSS. The
  // fields above are then empty.
  std::string frontPath;
  std::string backPath;
  std::string cssPath;
  bool tooLarge = false;  // Anki's rendering did not fit the device
};

// The user's scheduling settings (Settings > Flashcards), or an Anki deck's own preset where its
// last sync read one.
fsrs::Params schedulingParams(const ankideck::DeckInfo* deck = nullptr);
uint16_t newCardsPerDay(const ankideck::DeckInfo* deck = nullptr);

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

// What a rating button shows: minutes for a learning step, days otherwise.
struct Interval {
  uint16_t value = 0;
  bool minutes = false;
};

class Session {
 public:
  // Most cards a deck may hold: the state table is 24 bytes per card in RAM.
  static constexpr size_t MAX_CARDS = 2000;
  static constexpr size_t MAX_QUEUE = 300;

  // Loads the deck's progress and queues today's cards: due reviews first, then new ones.
  bool open(const std::string& language);
  // Writes progress if anything changed since the last save.
  bool save();

  bool finished() const { return position >= queue.size() && learning.empty(); }
  size_t remaining() const { return (position < queue.size() ? queue.size() - position : 0) + learning.size(); }
  size_t reviewed() const { return reviewedCount; }
  size_t total() const { return queue.size(); }
  const std::string& language() const { return lang; }

  // The card at the front of the queue; false when its row could not be read.
  bool current(CardText& out) const;
  bool currentIsNew() const;
  // What each rating would give: a learning step in minutes, or an interval in days.
  void previews(Interval out[4]) const;
  void rate(fsrs::Rating rating);
  uint16_t ratingCount(fsrs::Rating rating) const { return ratingCounts[static_cast<int>(rating) - 1]; }

 private:
  struct QueueItem {
    uint64_t id;
    uint32_t offset;    // byte offset of the card's CSV row; an Anki card's index flags
    uint64_t note = 0;  // an Anki card's note, for burying its siblings
  };

  // A card between learning steps, shown again this session once its step has passed.
  struct Learning {
    QueueItem item;
    uint32_t dueMs;
    uint8_t step;
    bool relearn;
  };
  // The step after `rating` from step `current` (a new or just-forgotten card is at step 0); -1
  // graduates to a day interval. stepMinutes gives its delay: Hard on the first step waits between
  // the first two steps, as Anki does.
  static int nextStep(fsrs::Rating rating, int current, uint8_t count);
  static uint16_t stepMinutes(const uint16_t* steps, uint8_t count, fsrs::Rating rating, int current, int next);
  const QueueItem& active() const { return activeLearn >= 0 ? learning[activeLearn].item : queue[position]; }
  void pickNext();

  CardState* find(uint64_t id);
  const CardState* find(uint64_t id) const;
  bool load();

  std::string lang;
  std::string csvPath;
  bool upgraded = false;                    // the file has the Furigana column
  bool anki = false;                        // an Anki deck: cards come from its index, sides from XHTML files
  mutable bool cardsSyncForgotten = false;  // a missing card file was reported once this session
  std::vector<CardState> states;            // sorted by id
  std::vector<QueueItem> queue;
  size_t position = 0;
  size_t reviewedCount = 0;
  uint16_t ratingCounts[4] = {0, 0, 0, 0};
  uint16_t day = 0;
  uint16_t newToday = 0;
  fsrs::Params params;  // this deck's scheduling, set by open()
  uint16_t perDay = 0;
  uint16_t learnSteps[4] = {};
  uint8_t learnCount = 0;
  uint16_t relearnSteps[4] = {};
  uint8_t relearnCount = 0;
  std::vector<Learning> learning;
  int activeLearn = -1;  // the card shown comes from `learning` at this index, else from the queue
  // From the Anki deck's preset (see ankideck::DeckInfo); off for the device's own decks.
  bool buryNew = false;
  bool buryReviews = false;
  uint8_t easyDays[7] = {100, 100, 100, 100, 100, 100, 100};
  uint8_t leechThreshold = 0;
  bool leechSuspend = true;
  std::vector<uint64_t> notesDone;  // notes reviewed today, sorted: their other cards wait (burying)
  bool buried(const QueueItem& item) const;
  uint16_t easyDay(uint16_t interval) const;
  void markLeech(const QueueItem& item, const CardState& s);
  std::vector<uint64_t> leeches;  // suspended leeches not yet sent to Anki, sorted: not queued
  uint8_t unsaved = 0;
  std::vector<ReviewLogEntry> pendingLog;  // ratings not yet appended to the review log
};

}  // namespace flashcards
