#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <string>
#include <vector>

// A deck brought from Anki onto the device, kept in the system folder (never in the Library):
//   anki/<deckId>/deck.json      name, last Anki review seen
//   anki/<deckId>/cards.bin      the card index, CardRecord per card, sorted by card id
//   anki/<deckId>/style.css      the note types' CSS
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
constexpr uint32_t TOO_LARGE = 1;  // Anki's rendering did not fit the device; shown as a notice

struct DeckInfo {
  std::string id;    // Anki deck id
  std::string name;  // Anki deck name
  uint64_t lastReviewId = 0;  // newest Anki review folded into the device's progress
};

constexpr const char* KEY_PREFIX = "anki-";
inline bool isAnkiKey(const std::string& key) { return key.rfind(KEY_PREFIX, 0) == 0; }
inline std::string keyFor(const std::string& deckId) { return KEY_PREFIX + deckId; }
std::string dirFor(const std::string& deckId);  // with a trailing '/'
std::string idOf(const std::string& key);

bool readInfo(const std::string& deckId, DeckInfo& out);
bool writeInfo(const DeckInfo& info);
bool readIndex(const std::string& deckId, std::vector<CardRecord>& out);

// The index read from the card a record at a time: a sync keeps the old index on the card
// rather than in RAM beside the new one. Records are sorted by card id.
class IndexReader {
 public:
  bool open(const std::string& deckId);  // true with size() 0 when the deck has no index yet
  size_t size() const { return count; }
  bool at(size_t i, CardRecord& out);
  bool find(uint64_t cardId, CardRecord& out);  // binary search; false when absent or unreadable
  void close() {
    if (file.isOpen()) file.close();  // a deck without an index never opened it
  }

 private:
  HalFile file;
  size_t count = 0;
};

// The new index written a record at a time, in card id order; commit() replaces the old one.
class IndexWriter {
 public:
  bool open(const std::string& deckId);
  bool add(const CardRecord& record);
  bool commit();

 private:
  HalFile file;
  std::string path;
  std::string tmp;
};
// Every deck on the card, by name.
void list(std::vector<DeckInfo>& out);

std::string frontPath(const std::string& deckId, uint64_t cardId);
std::string backPath(const std::string& deckId, uint64_t cardId);
std::string cssPath(const std::string& deckId);

}  // namespace ankideck
