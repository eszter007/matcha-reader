#pragma once

#include <cstdint>
#include <string>

// A book's exact position on another device, fetched by an Insights sync while it was online, so the
// reader can move there on open without WiFi. Kept as synced.bin in the book's cache folder until it is
// applied, declined, or a sync of the book itself settles the position.
namespace syncedPosition {

struct Position {
  std::string progress;  // the server's position: an XPath for a reflowable book, a page number otherwise
  float percentage = 0;  // 0..1
  bool byShare = false;  // from a twin file ("Title.cbz" elsewhere): only the share of the book carries over
};

bool load(const std::string& path, Position& out);
void clear(const std::string& path);

// The page of a paged book (XTC/XTCH, manga) the position names: its page number when it has one, else the
// share of `pageCount`.
uint32_t pageOf(const Position& position, uint32_t pageCount);

// For every book on the card the progress index shows further ahead than this reader's own progress,
// fetches its exact position from the sync server and keeps it for the next open. Runs online, after the
// progress feed is pulled.
void fetchAhead();

}  // namespace syncedPosition
