#pragma once

#include <cstdint>
#include <string>

namespace remoteProgress {
class Feed;
}

// A book's exact position on another device, fetched by an Insights sync while it was online, so the
// reader can move there on open without WiFi. Kept as synced.bin in the book's cache folder, stamped with
// the sync account. Once applied, declined or overtaken it stays as a handled record, so the next sync does
// not fetch the same position again unless the server's record for the book changed.
namespace syncedPosition {

struct Position {
  std::string progress;  // the server's position: an XPath for a reflowable book, a page number otherwise
  float percentage = 0;  // 0..1
  bool byShare = false;  // from a twin file ("Title.cbz" elsewhere): only the share of the book carries over
};

// The book's position still to be offered, for the current account; false when there is none.
bool load(const std::string& path, Position& out);
// Applied, declined, overtaken by reading here, or settled by a sync of the book: not offered again.
void dismiss(const std::string& path);

// The page of a paged book (XTC/XTCH, manga) the position names: its page number when it has one, else the
// share of `pageCount`.
uint32_t pageOf(const Position& position, uint32_t pageCount);

// For every book on the card `feed` shows further ahead than this reader's own progress, fetches its exact
// position from the sync server and keeps it for the next open. Runs online, after the feed is pulled.
void fetchAhead(const remoteProgress::Feed& feed);

}  // namespace syncedPosition
