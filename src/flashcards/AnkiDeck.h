#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// A deck brought from Anki onto the device, kept in the system folder (never in the Library):
//   anki/<deckId>.json           name, last Anki review seen
//   anki/<deckId>.cards.bin      the card index, CardRecord per card, sorted by card id
//   anki/<deckId>.buried.bin     cards buried until tomorrow; anki/<deckId>.leeches.bin, leeches
//   anki/<deckId>.css            the note types' CSS
//   anki-cache/<deckId>/         the card images' decoded pixels (.pxc)
//   anki/<deckId>/<cardId>q.xhtml, <cardId>a.xhtml   each side as Anki renders it, as XHTML
//   anki/<deckId>/<media>        the images the cards use
// Its review progress uses the flashcard store under the deck key "anki-<deckId>".
namespace ankideck {

struct CardRecord {
  uint64_t cardId = 0;
  uint64_t noteId = 0;
  uint32_t noteMod = 0;  // the note's last change, seconds since 1970: re-rendered when it moves
  uint32_t flags = 0;    // TOO_LARGE and friends
};
constexpr uint32_t TOO_LARGE = 1;      // Anki's rendering did not fit the device; shown as a notice
constexpr uint32_t MEDIA_MISSING = 2;  // an image of the card failed; tried again at each sync

struct DeckInfo {
  std::string id;             // Anki deck id
  std::string name;           // Anki deck name
  uint64_t lastReviewId = 0;  // newest Anki review folded into the device's progress
  int64_t cardsSyncedMs = 0;  // start of the last complete card refresh, from the network clock
  // From the deck's preset in Anki, when the last sync could read it; unset uses the device settings.
  int32_t newPerDay = -1;
  float retention = 0;
  uint16_t maxInterval = 0;
  int32_t reviewsPerDay = -1;
  // Learning and relearning steps in minutes; count 0xFF while unknown.
  static constexpr uint8_t MAX_STEPS = 4;
  uint16_t learnSteps[MAX_STEPS] = {};
  uint8_t learnCount = 0xFF;
  uint16_t relearnSteps[MAX_STEPS] = {};
  uint8_t relearnCount = 0xFF;
  // Order and burying, from the preset; 0 is the device's own behaviour.
  enum ReviewOrder : uint8_t { REVIEW_DUE = 0, REVIEW_RANDOM, REVIEW_INTERVAL_ASC, REVIEW_INTERVAL_DESC };
  enum NewMix : uint8_t { NEW_AFTER = 0, NEW_BEFORE, NEW_MIXED };
  uint8_t reviewOrder = REVIEW_DUE;
  bool newRandom = false;
  uint8_t newMix = NEW_AFTER;
  bool buryNew = false;
  bool buryReviews = false;
  // Easy Days: how much of a normal day's reviews each weekday takes, Monday first, in percent.
  uint8_t easyDays[7] = {100, 100, 100, 100, 100, 100, 100};
  // A card failing this many times is a leech (0: never); suspended in Anki or only tagged.
  uint8_t leechThreshold = 0;
  bool leechSuspend = true;
  uint8_t fetchVersion = 0;  // how its cards were last fetched; see FETCH_VERSION in AnkiSync.cpp
  // Style blocks already in style.css: FNV-1a of a note type's CSS as the reply carried it
  // (JsonValueSkipper::hash), or of a template's own <style> text.
  std::vector<uint32_t> cssHashes;
  // Newest change time (Anki's clock, seconds) of any note type at the last complete refresh; a
  // note type changed later has its cards in this deck fetched again. 0 until first checked.
  uint32_t typesMod = 0;
  // A note type used here changed: every card is fetched again (templates and styles with them),
  // until a refresh completes. Kept across failed or cancelled syncs.
  bool refetchAll = false;
  uint64_t refetchedUpTo = 0;  // cards up to this id were fetched again already (index saved)
};
constexpr size_t MAX_CSS_HASHES = 16;

constexpr const char* KEY_PREFIX = "anki-";
inline bool isAnkiKey(const std::string& key) { return key.rfind(KEY_PREFIX, 0) == 0; }
// Device deck ids of Migaku decks start with this; Anki's are its own ids, digits only.
constexpr const char* MIGAKU_PREFIX = "m";
inline bool isMigakuKey(const std::string& key) { return key.rfind(std::string(KEY_PREFIX) + MIGAKU_PREFIX, 0) == 0; }
inline std::string keyFor(const std::string& deckId) { return KEY_PREFIX + deckId; }
std::string dirFor(const std::string& deckId);  // the deck's card folder, with a trailing '/'
// Where the deck's card images keep their decoded pixels (the reader's .pxc files), with a trailing
// '/': not the card folder, where a name costs a quarter second to find. Made when first needed.
std::string imageCacheDir(const std::string& deckId);
// One of the deck's small files beside that folder (suffix ".json", ".cards.bin"...): a lookup in a
// folder of thousands of cards costs a quarter second. Moves an older layout's files on first use.
std::string metaPath(const std::string& deckId, const char* suffix);
// Removes those small files: a deck whose folder was deleted starts again from nothing, as it did
// when they lived inside the folder.
void forgetDeckFiles(const std::string& deckId);
std::string idOf(const std::string& key);

bool readInfo(const std::string& deckId, DeckInfo& out);
bool writeInfo(const DeckInfo& info);
// A card file the index lists is missing: the next sync checks every card again.
void forgetCardsSynced(const std::string& deckId);
// Each card of the index in turn, read a few records at a time: a 2000-card index whole needs a
// 48 KB block, which a fragmented heap often does not have. True with no calls when the deck has no
// index yet; false when unreadable, possibly after some calls.
bool scanIndex(const std::string& deckId, const std::function<void(const CardRecord&)>& each);

constexpr size_t INDEX_RECORD_BYTES = 24;  // one CardRecord in cards.bin

// The index read from the card a record at a time: a sync keeps the old index on the card
// rather than in RAM beside the new one. Records are sorted by card id.
class IndexReader {
 public:
  bool open(const std::string& deckId);  // true with size() 0 when the deck has no index yet
  size_t size() const { return count; }
  bool at(size_t i, CardRecord& out);
  // Forward only: ids asked in rising order, as the sync walks its sorted list. Reads the file
  // once, a few records at a time. False when absent or unreadable.
  bool findNext(uint64_t cardId, CardRecord& out);
  size_t position() const { return next; }  // records before it hold smaller ids than asked
  void skipTo(size_t i) { next = i; }
  // Appends records [first, size()) to `out` as they are stored, through the caller's buffer.
  bool copyTail(size_t first, HalFile& out, uint8_t* chunk, size_t chunkBytes);
  void close() {
    if (file.isOpen()) file.close();  // a deck without an index never opened it
  }

 private:
  static constexpr size_t BUFFERED = 4;
  HalFile file;
  size_t count = 0;
  size_t next = 0;      // first record findNext has not passed
  size_t bufStart = 0;  // records [bufStart, bufStart + bufCount) are in buf
  size_t bufCount = 0;
  uint8_t buf[BUFFERED * INDEX_RECORD_BYTES] = {};
};

// The new index written a record at a time, in card id order; commit() replaces the old one.
class IndexWriter {
 public:
  bool open(const std::string& deckId);
  bool add(const CardRecord& record);
  bool commit();
  // Saves the records written so far as the deck's index, followed by the old index's records
  // after lastDone: a sync cut off later resumes after lastDone. Reopens `known` on the result,
  // positioned after the saved records, whose count is `written`.
  bool checkpoint(IndexReader& known, uint64_t lastDone, size_t& written);

 private:
  HalFile file;
  std::string deckId;
  std::string path;
  std::string tmp;
};
// Every deck on the card, by name.
void list(std::vector<DeckInfo>& out);

std::string frontPath(const std::string& deckId, uint64_t cardId);
std::string backPath(const std::string& deckId, uint64_t cardId);
std::string cssPath(const std::string& deckId);
// Leeches found on the device, for the next sync to suspend or tag in Anki: 16 bytes each.
std::string leechPath(const std::string& deckId);

// Cards buried in Anki (until its next day), as the last sync saw them: `day` is the device day
// of that sync (flashcards::today()), so a later day reads as none buried.
constexpr size_t MAX_BURIED = 512;
bool writeBuried(const std::string& deckId, int32_t day, const std::vector<uint64_t>& ids);
// Sorted ids buried on `day`; empty for any other day or without a file.
void readBuried(const std::string& deckId, int32_t day, std::vector<uint64_t>& out);

}  // namespace ankideck
