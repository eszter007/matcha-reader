#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// How far books got somewhere other than this reader: on another device, as the sync server's progress feed
// reports it, or marked read here before the book had a reading cache. The Library and Home badges show the
// larger of this and the reader's own progress, so the furthest position anywhere wins. Kept by document key,
// the first 32 bits of the sync document hash.
namespace remoteProgress {

// Server percentage from which a book counts as finished, as the sync server and its app count it.
constexpr float FINISHED_AT = 0.98f;
// Bounds the file and the screens' copy of it: 20 KB.
constexpr size_t MAX_ENTRIES = 4000;

// The key of a document hash (at least 8 hex digits); 0 when it is not one.
inline uint32_t keyOf(const char* hash, const size_t len) {
  if (len < 8) return 0;
  uint32_t key = 0;
  for (size_t i = 0; i < 8; i++) {
    const char c = hash[i];
    uint32_t v;
    if (c >= '0' && c <= '9') {
      v = c - '0';
    } else if (c >= 'a' && c <= 'f') {
      v = c - 'a' + 10;
    } else if (c >= 'A' && c <= 'F') {
      v = c - 'A' + 10;
    } else {
      return 0;
    }
    key = (key << 4) | v;
  }
  return key;
}

// A server percentage (0..1) as a badge percent: 100 from FINISHED_AT on, else rounded down below it.
inline uint8_t percentOf(const float fraction) {
  if (fraction >= FINISHED_AT) return 100;
  if (fraction <= 0) return 0;
  const int pct = static_cast<int>(fraction * 100);
  return static_cast<uint8_t>(pct > 99 ? 99 : pct);
}

// The whole index for one screen; percent() is a binary search.
class Snapshot {
 public:
  void load();
  // The book's percent elsewhere, -1 when the index does not have it.
  int percent(const std::string& path) const;

 private:
  std::vector<uint32_t> keys;  // sorted
  std::vector<uint8_t> percents;
};

// One lookup straight from the file, for a screen with only a few books; -1 when the index does not have it.
int percent(const std::string& path);

// Mark as Read for a book that has no reading cache to hold its progress.
bool markRead(const std::string& path);
// Takes the book out of the index (Mark as Unread). True when it was in it.
bool forget(const std::string& path);
// A reading session here moved a book from `enterPercent` to `exitPercent` (this reader's own progress). When
// the book counts as finished elsewhere and it now stands below the end, that is a restart: it leaves the
// index, so this reader's progress shows again. Opening it without moving keeps the finish.
void readHere(const std::string& path, int enterPercent, int exitPercent);

// The progress feed's side: the index in memory while a sync applies pages of changes.
class Feed {
 public:
  // False when the index does not fit the heap. A missing index, or one kept for another account, starts
  // empty from cursor 0: the feed then sends every book again.
  bool begin(uint32_t accountKey);
  uint64_t cursor() const { return feedCursor; }
  // A book's newest percent; 0 takes it out. False when this sync's room is used up: the page it came with
  // must be fetched again next time.
  bool set(uint32_t key, uint8_t percent);
  // A book was left out because the index holds MAX_ENTRIES.
  bool overflowed() const { return full; }
  // Everything up to `next` is applied.
  void advance(const uint64_t next) { feedCursor = next; }
  bool save() const;

 private:
  uint32_t account = 0;
  uint64_t feedCursor = 0;
  bool full = false;
  std::vector<uint32_t> keys;  // sorted
  std::vector<uint8_t> percents;
};

}  // namespace remoteProgress
