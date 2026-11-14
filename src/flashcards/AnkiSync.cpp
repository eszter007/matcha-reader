#include "AnkiSync.h"

#include <AnkiHtml.h>
#include <AnkiSyncStore.h>
#include <Epub/converters/ImageDecoderFactory.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_heap_caps.h>
#include <mbedtls/base64.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "AnkiDeck.h"
#include "FlashcardStore.h"
#include "JsonValueSkipper.h"

namespace ankisync {
namespace {
constexpr size_t BATCH = 50;              // ids per request: no request grows with the deck
constexpr size_t CHECKPOINT_BATCHES = 2;  // index saved every 100 cards: a cut-off sync resumes
#ifdef BOARD_HAS_PSRAM
constexpr size_t MAX_CARD_XHTML = 160 * 1024;  // one side as stored
#else
constexpr size_t MAX_CARD_XHTML = 40 * 1024;  // one side as stored
#endif
#ifdef BOARD_HAS_PSRAM
constexpr size_t MAX_CSS = 128 * 1024;  // a deck's styling
#else
constexpr size_t MAX_CSS = 32 * 1024;  // a deck's styling
#endif
constexpr size_t NOTE_GROUP = 10;  // notes looked up per request
// Raised when cards that were too large may fit now: those are fetched once more.
// 1: each card's reply leaves out the note type's CSS.
// 2: images stream to the card, so large ones that failed before are looked for again (cards
//    still too large are also tried once more, as with any raise).
// 3: a reply too large for both sides is fetched again one side at a time.
constexpr uint8_t MEDIA_STREAM_VERSION = 2;
constexpr uint8_t FETCH_VERSION = 3;
constexpr uint32_t OPERATION_TIMEOUT = 120000;  // AnkiWeb syncs and imports
constexpr const char* RATING_NAMES[] = {"again", "hard", "good", "easy"};
constexpr size_t HEAP_HEADROOM = 8 * 1024;  // left for Wi-Fi and SD after a sync's own table

uint64_t toId(JsonVariantConst v) {
  const char* s = v.as<const char*>();
  return s ? strtoull(s, nullptr, 10) : 0;
}

// Seconds since 1970 for an RFC 3339 UTC time ("2026-11-12T20:54:58.073Z").
uint32_t parseStamp(const char* s) {
  int y, mo, d, h, mi, sec;
  if (!s || sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) != 6) return 0;
  // Days from civil (Howard Hinnant), valid for the Gregorian calendar.
  y -= mo <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const int64_t days = static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
  return static_cast<uint32_t>(days * 86400 + h * 3600 + mi * 60 + sec);
}

// Appends to the deck's style.css a byte at a time, through a small buffer: a note type's CSS
// can be as large as a card, and the deck's styling is never held in RAM.
class CssAppend {
 public:
  bool open(const std::string& path) {
    if (!Storage.openFileForAppend("ANKI", path.c_str(), file)) return false;
    size = start = file.fileSize();
    return true;
  }
  static void put(void* ctx, const char c) {
    auto* self = static_cast<CssAppend*>(ctx);
    if (self->full || self->failed) return;
    if (self->size + self->n + 1 > MAX_CSS) {
      self->full = true;
      return;
    }
    self->buf[self->n++] = c;
    if (self->n == sizeof(self->buf)) self->flush();
  }
  void putText(const char* text) {
    for (; *text; ++text) put(this, *text);
  }
  // True when all of it reached the file. Otherwise, or without `keep`, the file is cut back to
  // what it held before: a partial block would be added again at the next try.
  bool close(const bool keep = true) {
    flush();
    const bool whole = keep && !failed && !full;
    if (!whole) file.truncate(start);
    file.close();
    return whole;
  }

 private:
  void flush() {
    if (n > 0 && file.write(reinterpret_cast<const uint8_t*>(buf), n) != n) failed = true;
    size += n;
    n = 0;
  }
  HalFile file;
  char buf[128];
  size_t n = 0;
  size_t size = 0;
  size_t start = 0;
  bool full = false;
  bool failed = false;
};

// A deck or note type name inside a quoted Anki search term: quotes and backslashes escaped.
std::string quoted(const std::string& name) {
  std::string out;
  out.reserve(name.size() + 2);
  for (const char c : name) {
    if (c == '"' || c == '\\') out.push_back('\\');
    out.push_back(c);
  }
  return out;
}

uint32_t fnv1a(const char* text) {
  uint32_t hash = 2166136261u;
  for (; *text; ++text) hash = (hash ^ static_cast<uint8_t>(*text)) * 16777619u;
  return hash;
}

// Remembers that style.css holds a block, saving deck.json at once: the block is in the file
// already, and a sync that stops before its end must not add it again.
void rememberCss(ankideck::DeckInfo& info, const uint32_t hash) {
  info.cssHashes.push_back(hash);
  if (!ankideck::writeInfo(info)) LOG_ERR("ANKI", "CSS list not saved");
}

bool cssListFull(const ankideck::DeckInfo& info) {
  if (info.cssHashes.size() < ankideck::MAX_CSS_HASHES) return false;
  LOG_ERR("ANKI", "Deck has %u styles already; a new one is not kept", static_cast<unsigned>(ankideck::MAX_CSS_HASHES));
  return true;
}

// One side as a complete XHTML document for the chapter parser. A template's own <style> arrives
// with every card; it goes to style.css once, remembered by its hash in the deck's cssHashes.
bool writeSide(const std::string& path, const char* html, const size_t limit, std::string& scratch,
               const std::string& cssPath, ankideck::DeckInfo& info) {
  if (!ankihtml::toXhtml(html, scratch, limit)) return false;
  // WebP is drawn from a PNG copy (see fetchMedia).
  for (size_t at = 0; (at = scratch.find(".webp\"", at)) != std::string::npos; at += 9) {
    scratch.insert(at + 5, ".png");
  }
  // Anki desktop leaves a type-in answer's box as "[[type:Field]]": the reader has no box for it.
  for (size_t at = 0; (at = scratch.find("[[type:", at)) != std::string::npos;) {
    const size_t end = scratch.find("]]", at);
    // Only a whole placeholder: a field name holds no markup, brackets or line break.
    if (end == std::string::npos || scratch.find_first_of("<[\n", at + 7) < end) {
      at += 7;
      continue;
    }
    scratch.erase(at, end + 2 - at);
  }
  {
    std::string styles;
    if (ankihtml::extractStyles(html, styles, MAX_CSS) && !styles.empty()) {
      const uint32_t hash = fnv1a(styles.c_str());
      const auto& known = info.cssHashes;
      if (std::find(known.begin(), known.end(), hash) == known.end() && !cssListFull(info)) {
        CssAppend css;
        if (css.open(cssPath)) {
          css.putText(styles.c_str());
          CssAppend::put(&css, '\n');
          if (css.close()) rememberCss(info, hash);
        }
      }
    }
  }
  // Written in pieces: a copy of the side would double its size in RAM.
  static constexpr char HEAD[] =
      "<?xml version=\"1.0\" encoding=\"utf-8\"?><html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title/>"
      "</head><body><div class=\"card\">";
  static constexpr char TAIL[] = "</div></body></html>";
  HalFile file;
  if (!Storage.openFileForWrite("ANKI", path, file)) return false;
  const auto put = [&file](const char* text, const size_t size) {
    return file.write(reinterpret_cast<const uint8_t*>(text), size) == size;
  };
  return put(HEAD, sizeof(HEAD) - 1) && put(scratch.data(), scratch.size()) && put(TAIL, sizeof(TAIL) - 1);
}

// Adds `value` unless it is there already: the lists here hold a handful of names.
void addUnique(std::vector<std::string>& list, std::string value) {
  if (std::find(list.begin(), list.end(), value) == list.end()) list.push_back(std::move(value));
}

// Image file names a side refers to, plain names only (no folders, no remote URLs).
void mediaNames(const std::string& xhtml, std::vector<std::string>& out) {
  size_t at = 0;
  while ((at = xhtml.find("src=\"", at)) != std::string::npos) {
    at += 5;
    const size_t end = xhtml.find('"', at);
    if (end == std::string::npos) break;
    const std::string name = xhtml.substr(at, end - at);
    if (!name.empty() && name.find('/') == std::string::npos && name.find(':') == std::string::npos &&
        name.find("..") == std::string::npos && name[0] != '.') {
      addUnique(out, name);
    }
    at = end;
  }
}

// Decodes base64 as it streams in and writes the bytes to a file through a small buffer: an image
// can be far larger than the reply buffer.
class Base64File {
 public:
#ifdef BOARD_HAS_PSRAM
  static constexpr size_t MAX_BYTES = 4 * 1024 * 1024;  // larger images are not kept
#else
  static constexpr size_t MAX_BYTES = 1024 * 1024;  // the decoder's budget without PSRAM
#endif

  bool open(const std::string& path) { return Storage.openFileForWrite("ANKI", path, file); }
  static void put(void* ctx, const char c) {
    auto* self = static_cast<Base64File*>(ctx);
    const bool b64 =
        (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=';
    if (!b64 || self->failed || self->tooLarge) return;
    self->quad[self->quadLen++] = static_cast<unsigned char>(c);
    if (self->quadLen < 4) return;
    self->quadLen = 0;
    size_t n = 0;
    if (mbedtls_base64_decode(self->buf + self->len, sizeof(self->buf) - self->len, &n, self->quad, 4) != 0) {
      self->failed = true;
      return;
    }
    for (size_t i = 0; i < n && self->headLen < sizeof(self->head); i++)
      self->head[self->headLen++] = self->buf[self->len + i];
    self->len += n;
    if (self->len + 3 > sizeof(self->buf)) self->flush();
  }
  // True when every byte reached the file.
  bool close() {
    flush();
    file.close();
    return !failed && !tooLarge && quadLen == 0;
  }
  bool overCap() const { return tooLarge; }
  bool isPng() const {
    static constexpr uint8_t PNG_MAGIC[4] = {0x89, 'P', 'N', 'G'};
    return headLen == sizeof(head) && memcmp(head, PNG_MAGIC, sizeof(head)) == 0;
  }

 private:
  void flush() {
    if (len == 0 || failed) return;
    if (written + len > MAX_BYTES) {
      tooLarge = true;  // the rest of the reply is still read, not written
      len = 0;
      return;
    }
    if (file.write(buf, len) != len) failed = true;
    written += len;
    len = 0;
  }
  HalFile file;
  uint8_t buf[96];
  size_t len = 0;
  size_t written = 0;
  unsigned char quad[4];
  uint8_t quadLen = 0;
  uint8_t head[4];
  uint8_t headLen = 0;
  bool failed = false;
  bool tooLarge = false;
};

bool fetchMedia(AnkiClient& client, const std::string& dir, const std::string& name) {
  const std::string path = dir + name;
  if (Storage.exists(path.c_str())) return true;
  // Only what the device can draw. A WebP image is referenced as "<name>.webp.png": the companion
  // server sends it converted to PNG; Anki desktop sends WebP, which is not kept.
  const bool webp = name.size() > 9 && name.compare(name.size() - 9, 9, ".webp.png") == 0;
  if (!webp && !ImageDecoderFactory::isFormatSupported(name)) return true;
  const std::string request = webp ? name.substr(0, name.size() - 4) : name;
  // Written beside the final name and renamed when whole: a cut-off image is never kept.
  const std::string part = path + ".part";
  Base64File out;
  if (!out.open(part)) return false;
  JsonValueSkipper data("data");
  data.decodeTo(Base64File::put, &out);
  JsonDocument params, result, filter;
  params["filename"] = request;
  filter["filename"] = true;
  const bool ok = client.call("media.get", params, result, &filter, &data);
  data.finish();
  const bool whole = out.close() && ok && data.valueBytes() > 0;
  if (!whole || (webp && !out.isPng())) {
    Storage.remove(part.c_str());
    // Over the size cap, empty, or still WebP (not drawable): it never will be, so not tried again.
    if (ok && (out.overCap() || data.valueBytes() == 0 || whole)) {
      LOG_INF("ANKI", "Image %s not kept", name.c_str());
      return true;
    }
    return false;
  }
  if (!Storage.rename(part.c_str(), path.c_str())) {
    Storage.remove(part.c_str());
    return false;
  }
  return true;
}

class DeckSync {
 public:
  DeckSync(AnkiClient& client, const ProgressFn& progress, Result& result, const int64_t nowMs, int64_t& nextPastMs)
      : client(client), progress(progress), result(result), nowMs(nowMs), nextPastMs(nextPastMs) {}

  bool run(const std::string& name, const std::string& deckId, const char* presetId);

 private:
  bool retryTooLarge = false;          // cards marked too large under an older FETCH_VERSION are tried again
  ankideck::DeckInfo* info = nullptr;  // the deck being synced, saved at the end of run()
  uint32_t typesModSeen = 0;           // newest note type change seen; saved with a complete refresh
  // Asks the screen whether the user cancelled; once true, stays true for this sync.
  bool cancelRequested() {
    if (result.error.kind != AnkiClient::Error::Kind::Cancelled && progress.cancelled()) {
      result.error = AnkiClient::Error{};
      result.error.kind = AnkiClient::Error::Kind::Cancelled;
    }
    return result.error.kind == AnkiClient::Error::Kind::Cancelled;
  }
  // Card ids matching an Anki search, sorted; at most MAX_CARDS, `cut` set when there were more.
  bool searchCards(const std::string& query, std::vector<uint64_t>& ids, bool* cut = nullptr);
  // Cards whose notes changed since the deck's last full refresh. False when that is unknown, the
  // list is incomplete or would save little, and every card's note is then checked.
  void readPreset(const char* presetId, ankideck::DeckInfo& info);
  bool pushLeeches(const std::string& deckId, bool suspend);
  // Fetches the images not on the card yet; false when one could not be fetched.
  bool fetchAllMedia(const std::string& dir, const std::vector<std::string>& media,
                     std::vector<std::string>& failedMedia);
  // For a card kept as it is: looks for its images again when one failed before, or once after
  // MEDIA_STREAM_VERSION. True when the record's MEDIA_MISSING flag changed.
  bool recheckMedia(const std::string& deckId, const std::string& dir, ankideck::CardRecord& r, std::string& scratch,
                    size_t limit, std::vector<std::string>& failedMedia);
  bool checkAllMedia = false;             // set for the first sync after MEDIA_STREAM_VERSION
  std::vector<std::string> presentMedia;  // images with nothing left to do this sync: on the card, or not kept
  void saveBuried(const std::string& name, const std::string& deckId);
  bool fetchCss(const JsonDocument& renderParams, const std::string& cssPath, uint32_t hash);
  bool editedSince(const std::string& name, const ankideck::DeckInfo& info, size_t deckSize,
                   std::vector<uint64_t>& edited);
  // Note id and last change for ids[from, to). A reply too large for the buffer (notes with long
  // fields) is fetched again in halves.
  bool fetchNoteMods(const std::vector<uint64_t>& ids, size_t from, size_t to, std::vector<uint64_t>& noteIds,
                     std::vector<uint32_t>& noteMods);
  // edited: cards whose notes may have changed, sorted; nullptr when any may have.
  bool refreshCards(const std::string& name, const std::string& deckId, const std::vector<uint64_t>& ids,
                    const std::vector<uint64_t>* edited);
  // Sets info->refetchAll when a note type this deck uses changed (template or style) since the
  // last complete refresh, by Anki's own change times.
  void checkNoteTypes(const std::string& name);
  // A saved index holds the cards fetched again up to `id`: a stopped refetch resumes after it.
  void noteRefetched(uint64_t id) {
    if (!info->refetchAll) return;
    info->refetchedUpTo = id;
    if (!ankideck::writeInfo(*info)) LOG_ERR("ANKI", "Refetch progress not saved");
  }
  bool deckUses(const std::string& name, const char* noteType);
  bool pullReviews(ankideck::DeckInfo& info);
  bool pushReviews(ankideck::DeckInfo& info);
  // Sends one read of the log (history, then each card's schedule) and drops it from the log.
  bool pushChunk(ankideck::DeckInfo& info, const std::string& key, const std::vector<flashcards::ReviewLogEntry>& log);
  // A sync's large tables and each card are checked first: a failed allocation aborts the device.
  bool roomFor(size_t bytes, const char* what);

  // A review's time, which is its id in Anki. One the clock put in the future (it ran fast before
  // this sync set it) gets a unique time just before now, so the next pull does not skip ahead.
  int64_t stampOf(const flashcards::ReviewLogEntry& e) {
    if (nowMs <= 0 || e.timeMs <= nowMs) return e.timeMs;
    return nextPastMs++;
  }

  AnkiClient& client;
  const ProgressFn& progress;
  Result& result;
  const int64_t nowMs;  // from the network clock; 0 when it could not be set
  int64_t& nextPastMs;  // shared by every deck of one sync: Anki review ids are collection-wide
};

bool DeckSync::roomFor(const size_t bytes, const char* what) {
  if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) >= bytes + HEAP_HEADROOM) return true;
  LOG_ERR("ANKI", "OOM: %u bytes for %s", static_cast<unsigned>(bytes), what);
  result.error.kind = AnkiClient::Error::Kind::NoMemory;
  return false;
}

bool DeckSync::searchCards(const std::string& query, std::vector<uint64_t>& ids, bool* cut) {
  ids.clear();
  // Two passes, counting first: the table is then reserved once at its size, never copied while
  // it grows. Pages stay small; a parsed page of ids costs about three times its text.
  static constexpr int PAGE = 200;
  const auto eachPage = [this, &query](auto&& onPage) {
    std::string cursor;
    do {
      JsonDocument params, page;
      params["query"] = query;
      params["limit"] = PAGE;
      if (!cursor.empty()) params["cursor"] = cursor;
      if (!client.call("cards.search", params, page)) return false;
      if (!onPage(page["ids"].as<JsonArrayConst>())) return true;
      cursor = page["nextCursor"].isNull() ? std::string() : std::string(page["nextCursor"].as<const char*>());
    } while (!cursor.empty());
    return true;
  };
  size_t count = 0;
  if (!eachPage([&count](JsonArrayConst found) {
        count += found.size();
        return count < flashcards::Session::MAX_CARDS;
      })) {
    return false;
  }
  if (cut) *cut = count >= flashcards::Session::MAX_CARDS;
  count = std::min(count, flashcards::Session::MAX_CARDS);
  if (!roomFor(count * sizeof(uint64_t), "card ids")) return false;
  ids.reserve(count);
  if (!eachPage([&ids, count](JsonArrayConst found) {
        for (JsonVariantConst id : found) {
          if (ids.size() >= count) return false;
          ids.push_back(toId(id));
        }
        return ids.size() < count;
      })) {
    return false;
  }
  std::sort(ids.begin(), ids.end());
  return true;
}

bool DeckSync::editedSince(const std::string& name, const ankideck::DeckInfo& info, const size_t deckSize,
                           std::vector<uint64_t>& edited) {
  if (nowMs <= 0 || info.cardsSyncedMs <= 0 || info.cardsSyncedMs > nowMs) return false;
  // Anki counts edited:N in whole days from its day rollover: one day more covers the partial ones.
  constexpr int64_t DAY_MS = 24LL * 60 * 60 * 1000;
  const int64_t days = (nowMs - info.cardsSyncedMs) / DAY_MS + 2;
  bool cut = false;
  if (searchCards("\"deck:" + quoted(name) + "\" edited:" + std::to_string(days), edited, &cut) && !cut &&
      edited.size() <= deckSize / 2) {
    return true;
  }
  // Checking every note works without the list: a failure here does not fail the sync.
  result.error = AnkiClient::Error{};
  edited.clear();
  edited.shrink_to_fit();
  return false;
}

// Asks for the card again, this time streaming its CSS into the deck's style.css. Without room
// for all of it, it is not remembered, and so tried again at the next sync.
bool DeckSync::fetchCss(const JsonDocument& renderParams, const std::string& cssPath, const uint32_t hash) {
  if (cssListFull(*info)) return true;
  CssAppend css;
  if (!css.open(cssPath)) return true;
  JsonValueSkipper keep("css");
  keep.decodeTo(CssAppend::put, &css);
  JsonDocument filter, rendered;
  filter["cardId"] = true;  // the rest of the reply is not needed again
  const bool ok = client.call("cards.render", renderParams, rendered, &filter, &keep);
  keep.finish();
  CssAppend::put(&css, '\n');
  if (css.close(ok)) {
    rememberCss(*info, hash);
  } else {
    LOG_ERR("ANKI", "Card CSS not saved; tried again next sync");
  }
  return ok;
}

bool DeckSync::fetchAllMedia(const std::string& dir, const std::vector<std::string>& media,
                             std::vector<std::string>& failedMedia) {
  bool all = true;
  for (const auto& file : media) {
    // Images a deck shares (a template's logos) are checked once a sync: an existence check in a
    // folder of thousands of card files is a slow scan.
    if (std::find(presentMedia.begin(), presentMedia.end(), file) != presentMedia.end()) continue;
    if (std::find(failedMedia.begin(), failedMedia.end(), file) != failedMedia.end()) {
      all = false;
      continue;
    }
    if (fetchMedia(client, dir, file)) {
      if (presentMedia.size() < 32) presentMedia.push_back(file);
    } else {
      LOG_ERR("ANKI", "Image %s not fetched", file.c_str());
      if (failedMedia.size() < 16) failedMedia.push_back(file);
      all = false;
    }
  }
  return all;
}

bool DeckSync::recheckMedia(const std::string& deckId, const std::string& dir, ankideck::CardRecord& r,
                            std::string& scratch, const size_t limit, std::vector<std::string>& failedMedia) {
  if ((r.flags & ankideck::TOO_LARGE) != 0) return false;
  if (!checkAllMedia && (r.flags & ankideck::MEDIA_MISSING) == 0) return false;
  std::vector<std::string> media;
  for (const std::string& path : {ankideck::frontPath(deckId, r.cardId), ankideck::backPath(deckId, r.cardId)}) {
    HalFile f;
    if (!Storage.openFileForRead("ANKI", path, f)) continue;
    scratch.resize(std::min<size_t>(f.fileSize(), limit));
    scratch.resize(std::max(0, f.read(scratch.data(), scratch.size())));
    mediaNames(scratch, media);
  }
  std::string().swap(scratch);  // given back before the image requests
  const uint32_t before = r.flags;
  if (fetchAllMedia(dir, media, failedMedia)) {
    r.flags &= ~ankideck::MEDIA_MISSING;
  } else {
    r.flags |= ankideck::MEDIA_MISSING;
  }
  return r.flags != before;
}

// Leeches found while reviewing: tagged "leech" in Anki and, when the preset says so, suspended
// there, in batches of 64. True when all were sent; run() then deletes the list once the rest of
// the sync has worked too (until then the device keeps skipping suspended leeches).
bool DeckSync::pushLeeches(const std::string& deckId, const bool suspend) {
  const std::string path = ankideck::leechPath(deckId);
  if (!Storage.exists(path.c_str())) return false;
  if (!roomFor(4096, "leeches")) {
    result.error = AnkiClient::Error{};
    return false;
  }
  HalFile file;
  if (!Storage.openFileForRead("ANKI", path, file)) return false;
  bool more = true;
  while (more) {
    JsonDocument cardsParams, notesParams, done;
    JsonArray cards = cardsParams["ids"].to<JsonArray>();
    JsonArray notes = notesParams["noteIds"].to<JsonArray>();
    uint8_t rec[16];
    int n = 0;
    for (; n < 64 && file.read(rec, sizeof(rec)) == static_cast<int>(sizeof(rec)); n++) {
      uint64_t card = 0, note = 0;
      memcpy(&card, rec, 8);
      memcpy(&note, rec + 8, 8);
      cards.add(std::to_string(card));
      if (note != 0) notes.add(std::to_string(note));
    }
    more = n == 64;
    if (n == 0) break;
    cardsParams["suspended"] = true;
    notesParams["tags"][0] = "leech";
    const bool tagged = notes.size() == 0 || client.call("tags.add", notesParams, done);
    const bool suspended = !suspend || client.call("cards.setSuspended", cardsParams, done);
    if (!tagged || !suspended) {
      LOG_ERR("ANKI", "Leeches not sent; tried again next sync");
      result.error = AnkiClient::Error{};
      return false;
    }
  }
  return true;
}

bool DeckSync::deckUses(const std::string& name, const char* noteType) {
  JsonDocument params, page, filter;
  params["query"] = "\"deck:" + quoted(name) + "\" \"note:" + quoted(noteType) + "\"";
  params["limit"] = 1;
  filter["ids"] = true;
  if (!client.call("cards.search", params, page, &filter)) return false;
  return page["ids"].size() > 0;
}

void DeckSync::checkNoteTypes(const std::string& name) {
  JsonDocument none, types;
  none.to<JsonObject>();
  if (!client.call("noteTypes.list", none, types)) {
    result.error = AnkiClient::Error{};
    return;  // checked again at the next sync: typesMod does not move
  }
  // Anki's own times only, so the two clocks never meet. Before a first check nothing counts as
  // changed: the newest time is only noted.
  const uint32_t since = info->typesMod;
  uint32_t newest = since;
  bool complete = true;
  static constexpr const char* const BIG[] = {"css",         "questionHtml", "answerHtml",
                                              "latexHeader", "latexFooter",  nullptr};
  for (JsonVariantConst t : types.as<JsonArrayConst>()) {
    if (cancelRequested()) return;
    JsonDocument params, type, filter;
    params["noteType"]["id"] = t["id"].as<const char*>();
    filter["modifiedAt"] = true;
    JsonValueSkipper skip(BIG);
    if (!client.call("noteTypes.get", params, type, &filter, &skip)) {
      complete = false;
      continue;
    }
    const uint32_t mod = parseStamp(type["modifiedAt"] | "");
    newest = std::max(newest, mod);
    if (since == 0 || mod <= since || info->refetchAll) continue;
    const bool uses = deckUses(name, t["name"] | "");
    if (client.error().kind != AnkiClient::Error::Kind::None && !uses) {
      complete = false;
      continue;
    }
    if (!uses) continue;
    LOG_INF("ANKI", "Note type %s changed: every card again", t["name"] | "");
    info->refetchAll = true;
    if (!ankideck::writeInfo(*info)) LOG_ERR("ANKI", "Refetch not noted");
  }
  result.error = AnkiClient::Error{};
  // A type not read this time keeps the old mark, so its change is still found next sync.
  typesModSeen = complete ? newest : since;
}

// The deck's options in Anki: new cards a day, desired retention, maximum interval. Without an
// answer (an older Anki, a filtered deck) the deck keeps what it had, or the device settings.
void DeckSync::readPreset(const char* presetId, ankideck::DeckInfo& info) {
  if (!presetId || !*presetId) return;
  JsonDocument params, preset, filter;
  params["preset"]["id"] = presetId;
  filter["settings"]["newPerDay"] = true;
  filter["settings"]["desiredRetention"] = true;
  filter["settings"]["maximumIntervalDays"] = true;
  filter["settings"]["reviewsPerDay"] = true;
  filter["settings"]["learnStepsMinutes"] = true;
  filter["settings"]["relearnStepsMinutes"] = true;
  for (const char* key : {"reviewOrder", "newGatherOrder", "newSortOrder", "newMix", "buryNew", "buryReviews",
                          "easyDaysPercentages", "leechThreshold", "leechAction"}) {
    filter["settings"][key] = true;
  }
  if (!client.call("deckPresets.get", params, preset, &filter)) {
    result.error = AnkiClient::Error{};
    return;
  }
  JsonVariantConst s = preset["settings"];
  if (s["newPerDay"].is<int32_t>()) info.newPerDay = s["newPerDay"].as<int32_t>();
  if (s["desiredRetention"].is<float>()) info.retention = s["desiredRetention"].as<float>();
  if (s["reviewsPerDay"].is<int32_t>()) info.reviewsPerDay = s["reviewsPerDay"].as<int32_t>();
  // Steps under a minute count as one; Anki allows fractions such as 0.5.
  const auto steps = [](JsonVariantConst list, uint16_t* out, uint8_t& count) {
    if (!list.is<JsonArrayConst>()) return;
    count = 0;
    for (JsonVariantConst m : list.as<JsonArrayConst>()) {
      if (count >= ankideck::DeckInfo::MAX_STEPS) break;
      const float minutes = m.as<float>();
      out[count++] = static_cast<uint16_t>(std::clamp(minutes + 0.5f, 1.0f, 60000.0f));
    }
  };
  steps(s["learnStepsMinutes"], info.learnSteps, info.learnCount);
  steps(s["relearnStepsMinutes"], info.relearnSteps, info.relearnCount);
  // Orders the device has no equivalent for keep its own (by due date; new cards as Anki added them).
  const char* review = s["reviewOrder"] | "";
  info.reviewOrder = strcmp(review, "random") == 0               ? ankideck::DeckInfo::REVIEW_RANDOM
                     : strcmp(review, "intervalAscending") == 0  ? ankideck::DeckInfo::REVIEW_INTERVAL_ASC
                     : strcmp(review, "intervalDescending") == 0 ? ankideck::DeckInfo::REVIEW_INTERVAL_DESC
                                                                 : ankideck::DeckInfo::REVIEW_DUE;
  const char* gather = s["newGatherOrder"] | "";
  const char* sort = s["newSortOrder"] | "";
  info.newRandom = strncmp(gather, "random", 6) == 0 || strcmp(sort, "randomCard") == 0 ||
                   strcmp(sort, "randomNoteThenTemplate") == 0 || strcmp(sort, "templateThenRandom") == 0;
  const char* mix = s["newMix"] | "";
  info.newMix = strcmp(mix, "beforeReviews") == 0 ? ankideck::DeckInfo::NEW_BEFORE
                : strcmp(mix, "mixed") == 0       ? ankideck::DeckInfo::NEW_MIXED
                                                  : ankideck::DeckInfo::NEW_AFTER;
  info.buryNew = s["buryNew"] | false;
  info.buryReviews = s["buryReviews"] | false;
  if (s["easyDaysPercentages"].is<JsonArrayConst>() && s["easyDaysPercentages"].size() == 7) {
    for (size_t i = 0; i < 7; i++) {
      info.easyDays[i] =
          static_cast<uint8_t>(std::clamp(s["easyDaysPercentages"][i].as<float>() * 100.0f + 0.5f, 0.0f, 100.0f));
    }
  }
  if (s["leechThreshold"].is<int>())
    info.leechThreshold = static_cast<uint8_t>(std::clamp(s["leechThreshold"].as<int>(), 0, 255));
  info.leechSuspend = strcmp(s["leechAction"] | "suspend", "tagOnly") != 0;
  if (s["maximumIntervalDays"].is<uint32_t>()) {
    info.maxInterval = static_cast<uint16_t>(std::min<uint32_t>(s["maximumIntervalDays"].as<uint32_t>(), 36500));
  }
}

// Buried cards wait for Anki's next day: the device skips them on the day of this sync. Without
// a known day or an answer the old list stays, and only applies on its own day anyway.
void DeckSync::saveBuried(const std::string& name, const std::string& deckId) {
  const int32_t day = flashcards::today();
  if (nowMs <= 0 || day < 0) return;
  std::vector<uint64_t> buried;
  if (!searchCards("\"deck:" + quoted(name) + "\" is:buried", buried)) {
    result.error = AnkiClient::Error{};
    return;
  }
  if (!ankideck::writeBuried(deckId, day, buried)) LOG_ERR("ANKI", "Buried cards not saved");
}

bool DeckSync::fetchNoteMods(const std::vector<uint64_t>& ids, const size_t from, const size_t to,
                             std::vector<uint64_t>& noteIds, std::vector<uint32_t>& noteMods) {
  if (from >= to) return true;
  JsonDocument cardsParams, cards;
  JsonArray cardIds = cardsParams["ids"].to<JsonArray>();
  for (size_t i = from; i < to; i++) cardIds.add(std::to_string(ids[i]));
  JsonDocument cardFilter;
  cardFilter["items"][0]["result"]["noteId"] = true;
  JsonDocument notesParams, notes;
  bool ok = client.call("cards.getMany", cardsParams, cards, &cardFilter);
  if (ok) {
    JsonArray list = notesParams["ids"].to<JsonArray>();
    for (JsonVariantConst item : cards["items"].as<JsonArrayConst>()) list.add(item["result"]["noteId"]);
    JsonDocument noteFilter;
    noteFilter["items"][0]["result"]["modifiedAt"] = true;
    ok = client.call("notes.getMany", notesParams, notes, &noteFilter);
  }
  if (!ok) {
    if (client.error().kind != AnkiClient::Error::Kind::TooLarge) return false;
    if (to - from == 1) {  // one note too large to look at: render it anyway, it is flagged if that fails
      noteIds.push_back(0);
      noteMods.push_back(0);
      return true;
    }
    const size_t mid = from + (to - from) / 2;
    return fetchNoteMods(ids, from, mid, noteIds, noteMods) && fetchNoteMods(ids, mid, to, noteIds, noteMods);
  }
  for (size_t i = 0; i < to - from; i++) {
    noteIds.push_back(toId(cards["items"][i]["result"]["noteId"]));
    noteMods.push_back(parseStamp(notes["items"][i]["result"]["modifiedAt"] | ""));
  }
  return true;
}

// Cards that left the deck in Anki leave the device too; old records from index `from` with ids up
// to `upTo` are checked.
void dropRemoved(const std::string& deckId, ankideck::IndexReader& known, const std::vector<uint64_t>& ids,
                 const size_t from, const uint64_t upTo) {
  ankideck::CardRecord r;
  for (size_t i = from; i < known.size() && known.at(i, r) && r.cardId <= upTo; i++) {
    if (!std::binary_search(ids.begin(), ids.end(), r.cardId)) {
      Storage.remove(ankideck::frontPath(deckId, r.cardId).c_str());
      Storage.remove(ankideck::backPath(deckId, r.cardId).c_str());
    }
  }
}

bool DeckSync::refreshCards(const std::string& name, const std::string& deckId, const std::vector<uint64_t>& ids,
                            const std::vector<uint64_t>* edited) {
  const std::string dir = ankideck::dirFor(deckId);
  // What is already here, and the new index, both on the card: a large deck's two tables would
  // not fit in RAM beside Wi-Fi.
  ankideck::IndexReader known;
  ankideck::IndexWriter fresh;
  if (!known.open(deckId) || !fresh.open(deckId)) return false;
  const std::string cssPath = ankideck::cssPath(deckId);
  // A side is no longer than the reply it came in.
  const size_t sideLimit = std::min(MAX_CARD_XHTML, client.capacity());
  std::string scratch;
  // Images that failed this sync (e.g. too large): one a whole deck shares is asked for once.
  std::vector<std::string> failedMedia;
  failedMedia.reserve(4);
  presentMedia.reserve(32);

  // Per batch, reserved once.
  std::vector<uint64_t> need, noteIds;
  std::vector<uint32_t> noteMods;
  std::vector<ankideck::CardRecord> olds;
  if (!roomFor(BATCH * (2 * sizeof(uint64_t) + sizeof(uint32_t) + sizeof(ankideck::CardRecord)), "a batch")) {
    return false;
  }
  olds.resize(BATCH);
  need.reserve(BATCH);
  noteIds.reserve(BATCH);
  noteMods.reserve(BATCH);

  Progress p{name, 0, static_cast<uint32_t>(ids.size())};
  uint32_t kept = 0, checkpoints = 0, skipped = 0;
  bool changed = false;    // since the last index save
  size_t checkedUpTo = 0;  // old records before this index were saved by a checkpoint, all in ids
  for (size_t at = 0; at < ids.size(); at += BATCH) {
    const size_t end = std::min(ids.size(), at + BATCH);
    // A card here already, with its note unedited since the last refresh, is kept
    // without asking Anki about its note.
    static_assert(BATCH <= 64, "one bit per card of a batch");
    uint64_t had = 0, keep = 0;
    need.clear();
    for (size_t i = at; i < end; i++) {
      const uint64_t bit = 1ULL << (i - at);
      // A record that cannot be read counts as absent: the card is rendered again.
      ankideck::CardRecord& old = olds[i - at];
      if (known.findNext(ids[i], old)) had |= bit;
      // With the edited list, the index is trusted: a missing file is found by the reader, which
      // asks for the next sync to check every card.
      const bool retry = retryTooLarge && (had & bit) && (old.flags & ankideck::TOO_LARGE) != 0;
      if ((had & bit) && edited && !retry && !std::binary_search(edited->begin(), edited->end(), ids[i])) {
        keep |= bit;
      } else {
        need.push_back(ids[i]);
      }
    }
    // The other notes, for their last change; noteIds and noteMods follow need's order.
    noteIds.clear();
    noteMods.clear();
    // In groups of NOTE_GROUP: a note comes with all its fields, and a reply of 50 notes takes
    // the heap down to a few KB while it arrives, though only the change time is kept.
    for (size_t from = 0; from < need.size(); from += NOTE_GROUP) {
      if (!fetchNoteMods(need, from, std::min(need.size(), from + NOTE_GROUP), noteIds, noteMods)) return false;
    }
    size_t next = 0;
    bool stopped = false;
    for (size_t i = at; i < end; i++) {
      if (cancelRequested()) {
        // The rest of this batch keeps what it had, so the index saved below is complete.
        for (size_t j = i; j < end; j++) {
          if ((had & (1ULL << (j - at))) && !fresh.add(olds[j - at])) return false;
        }
        stopped = true;
        break;
      }
      const ankideck::CardRecord& old = olds[i - at];
      if (keep & (1ULL << (i - at))) {
        ankideck::CardRecord kept1 = old;
        if (recheckMedia(deckId, dir, kept1, scratch, sideLimit, failedMedia)) changed = true;
        if (!fresh.add(kept1)) return false;
        kept++;
        continue;
      }
      ankideck::CardRecord r;
      r.cardId = ids[i];
      r.noteId = noteIds[next];
      r.noteMod = noteMods[next];
      next++;
      const bool wasHere = (had & (1ULL << (i - at))) != 0;
      // A card too large stays so until its note changes, or until a newer way of fetching may fit it.
      const bool tooLarge = (old.flags & ankideck::TOO_LARGE) != 0;
      // While refetching, only the cards the saved index has not fetched again yet.
      const bool refetch = info->refetchAll && r.cardId > info->refetchedUpTo;
      const bool current = wasHere && old.noteMod == r.noteMod && !refetch &&
                           (tooLarge ? !retryTooLarge : Storage.exists(ankideck::frontPath(deckId, r.cardId).c_str()));
      if (current) {
        kept++;
        ankideck::CardRecord kept1 = old;
        if (recheckMedia(deckId, dir, kept1, scratch, sideLimit, failedMedia)) changed = true;
        if (!fresh.add(kept1)) return false;
        continue;
      }
      JsonDocument renderParams, rendered, cardOnly;
      renderParams["id"] = std::to_string(r.cardId);
      cardOnly["cardId"] = true;  // the sides are decoded in the reply buffer itself, not copied
      // Each reply repeats the note type's whole CSS: it is left out here and fetched once below.
      // A reply too large for both sides is asked for again once per side, the other left out.
      static constexpr const char* const BOTH[] = {"css", nullptr};
      static constexpr const char* const FRONT_ONLY[] = {"css", "answerHtml", nullptr};
      static constexpr const char* const BACK_ONLY[] = {"css", "questionHtml", nullptr};
      static constexpr const char* const SIDES[] = {"questionHtml", "answerHtml"};
      const std::string paths[2] = {ankideck::frontPath(deckId, r.cardId), ankideck::backPath(deckId, r.cardId)};
      std::vector<std::string> media;
      uint32_t cssHash = 0;
      size_t cssBytes = 0;
      enum class Fetch : uint8_t { Ok, ReplyTooLarge, SideTooLarge, NoRoom, Failed };
      // One cards.render request; writes the sides in `sides` (bit 0 front, bit 1 back).
      const auto fetchSides = [&](const char* const* skipKeys, const unsigned sides) {
        JsonValueSkipper skip(skipKeys);
        if (!client.call("cards.render", renderParams, rendered, &cardOnly, &skip)) {
          return client.error().kind == AnkiClient::Error::Kind::TooLarge ? Fetch::ReplyTooLarge : Fetch::Failed;
        }
        cssHash = skip.hash();
        cssBytes = skip.valueBytes();
        char* side[2];
        takeJsonStrings(client.replyText(), client.replyLength(), SIDES, side, 2);
        // Room for the cleaned side, about as long as the side itself.
        const size_t sideBytes = std::max(side[0] ? strlen(side[0]) : 0, side[1] ? strlen(side[1]) : 0);
        if (!roomFor(std::min(sideBytes + 1024, sideLimit), "a card")) return Fetch::NoRoom;
        for (unsigned k = 0; k < 2; k++) {
          if ((sides & (1U << k)) == 0) continue;
          const bool written = writeSide(paths[k], side[k] ? side[k] : "", sideLimit, scratch, cssPath, *info);
          if (written) mediaNames(scratch, media);
          // Given back before the next request, which needs that room while a reply arrives.
          std::string().swap(scratch);
          if (!written) return Fetch::SideTooLarge;
        }
        return Fetch::Ok;
      };
      Fetch fetched = fetchSides(BOTH, 3);
      if (fetched == Fetch::ReplyTooLarge) {
        fetched = fetchSides(FRONT_ONLY, 1);
        if (fetched == Fetch::Ok) {
          fetched = fetchSides(BACK_ONLY, 2);
          // No half card: the front is dropped with the back that did not fit.
          if (fetched == Fetch::ReplyTooLarge) Storage.remove(paths[0].c_str());
        }
      }
      if (fetched == Fetch::Failed) return false;
      if (fetched == Fetch::NoRoom) {
        // Left out of the index, so the next sync fetches it again rather than calling it too large.
        result.error = AnkiClient::Error{};
        skipped++;
        changed = true;
        continue;
      }
      if (fetched == Fetch::ReplyTooLarge) {
        r.flags |= ankideck::TOO_LARGE;  // shown as a notice, tried again when the note changes
        changed = true;
        if (!fresh.add(r)) return false;
        continue;
      }
      if (fetched == Fetch::SideTooLarge) r.flags |= ankideck::TOO_LARGE;
      if (!fetchAllMedia(dir, media, failedMedia)) r.flags |= ankideck::MEDIA_MISSING;
      // A note type's CSS the deck does not have yet; this reuses the reply buffer.
      auto& hashes = info->cssHashes;
      if (cssBytes > 0 && std::find(hashes.begin(), hashes.end(), cssHash) == hashes.end()) {
        if (!fetchCss(renderParams, cssPath, cssHash)) return false;
      }
      if (!wasHere) result.cardsAdded++;
      changed = true;
      if (!fresh.add(r)) return false;
    }
    if (stopped) {
      LOG_INF("ANKI", "%s: cancelled after %u of %u cards", name.c_str(), static_cast<unsigned>(at),
              static_cast<unsigned>(ids.size()));
      if (changed) {
        dropRemoved(deckId, known, ids, checkedUpTo, ids[end - 1]);
        if (fresh.checkpoint(known, ids[end - 1], checkedUpTo)) noteRefetched(ids[end - 1]);
      }
      return false;
    }
    p.done = static_cast<uint32_t>(end);
    progress(p);
    if (changed && end < ids.size() && (at / BATCH + 1) % CHECKPOINT_BATCHES == 0) {
      dropRemoved(deckId, known, ids, checkedUpTo, ids[end - 1]);
      if (!fresh.checkpoint(known, ids[end - 1], checkedUpTo)) return false;
      noteRefetched(ids[end - 1]);
      checkpoints++;
      changed = false;
    }
  }

  LOG_INF("ANKI", "%s: %u cards, %u unchanged, %u index saves, %u left for the next sync", name.c_str(),
          static_cast<unsigned>(ids.size()), static_cast<unsigned>(kept), static_cast<unsigned>(checkpoints),
          static_cast<unsigned>(skipped));
  dropRemoved(deckId, known, ids, checkedUpTo, UINT64_MAX);
  known.close();  // before the new index replaces it
  return fresh.commit();
}

bool DeckSync::pullReviews(ankideck::DeckInfo& info) {
  // Applied and remembered a page at a time: a sync cut off later neither applies a page twice
  // nor holds a long history in RAM.
  // Pages as large as the heap allows, from 20 to 200 reviews: a fragmented heap only means more pages.
  constexpr size_t MIN_PAGE = 20, MAX_PAGE = 200;
  const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  const size_t fits = largest > HEAP_HEADROOM ? (largest - HEAP_HEADROOM) / sizeof(flashcards::PulledReview) : 0;
  if (fits < MIN_PAGE && !roomFor(MIN_PAGE * sizeof(flashcards::PulledReview), "reviews")) return false;
  const size_t pageSize = std::max(MIN_PAGE, std::min(MAX_PAGE, fits));
  std::vector<flashcards::PulledReview> pulled;
  pulled.reserve(pageSize);
  const std::string key = ankideck::keyFor(info.id);
  // Ratings made here and not yet sent: an Anki review of the same card from before one of them
  // is already outdated on this device.
  std::vector<flashcards::ReviewLogEntry> unsent;
  if (!flashcards::readReviewLog(key, unsent)) return false;
  const auto outdated = [&unsent](const uint64_t cardId, const int64_t timeMs) {
    for (const auto& e : unsent) {
      if (e.id == cardId && e.timeMs > timeMs) return true;
    }
    return false;
  };
  std::string cursor;
  do {
    JsonDocument params, page;
    params["deck"]["id"] = info.id;
    params["includeManual"] = false;
    params["limit"] = pageSize;
    if (!cursor.empty()) {
      params["cursor"] = cursor;
    } else if (info.lastReviewId != 0) {
      params["afterId"] = std::to_string(info.lastReviewId);
    }
    JsonDocument filter;
    JsonObject f = filter["items"][0].to<JsonObject>();
    f["id"] = f["cardId"] = f["rating"] = f["kind"] = true;
    f["interval"] = true;
    filter["nextCursor"] = true;
    if (!client.call("reviews.list", params, page, &filter)) return false;
    pulled.clear();
    uint64_t newest = info.lastReviewId;
    for (JsonVariantConst r : page["items"].as<JsonArrayConst>()) {
      if (pulled.size() >= pageSize) break;  // the server sent more than asked: the rest come next sync
      const uint64_t id = toId(r["id"]);
      newest = std::max(newest, id);
      const char* rating = r["rating"] | "";
      uint8_t value = 0;
      for (uint8_t i = 0; i < 4; i++) {
        if (strcmp(rating, RATING_NAMES[i]) == 0) value = i + 1;
      }
      if (value == 0 || outdated(toId(r["cardId"]), static_cast<int64_t>(id))) continue;
      flashcards::PulledReview pr;
      pr.id = toId(r["cardId"]);
      pr.timeMs = static_cast<int64_t>(id);  // a review's id is its time in ms
      pr.rating = value;
      pr.interval = strcmp(r["interval"]["unit"] | "", "days") == 0 ? (r["interval"]["value"] | 0) : 0;
      pulled.push_back(pr);
    }
    if (!flashcards::applyPulledReviews(key, pulled)) return false;
    result.reviewsPulled += static_cast<uint32_t>(pulled.size());
    info.lastReviewId = newest;
    if (!ankideck::writeInfo(info)) return false;
    cursor = page["nextCursor"].isNull() ? std::string() : std::string(page["nextCursor"].as<const char*>());
  } while (!cursor.empty());
  return true;
}

bool DeckSync::pushReviews(ankideck::DeckInfo& info) {
  const std::string key = ankideck::keyFor(info.id);
  std::vector<flashcards::ReviewLogEntry> log;
  for (;;) {
    if (!flashcards::readReviewLog(key, log)) return false;
    if (log.empty()) return true;
    if (!pushChunk(info, key, log)) return false;
  }
}

bool DeckSync::pushChunk(ankideck::DeckInfo& info, const std::string& key,
                         const std::vector<flashcards::ReviewLogEntry>& log) {
  // History first, in batches. A review already in Anki (a sync cut off after its import) is a
  // conflict for the whole batch, so a failed batch is retried one review at a time.
  // Each review's time once: a batch refused for a conflict is sent again a review at a time.
  // Schedule-only entries (no rating: a mined card's progress from before reviews were logged)
  // send no history.
  std::vector<uint16_t> history;
  std::vector<int64_t> stamps;
  history.reserve(log.size());  // at most MAX_LOG_ENTRIES
  stamps.reserve(log.size());
  for (size_t i = 0; i < log.size(); i++) {
    if (log[i].rating == 0) continue;
    history.push_back(static_cast<uint16_t>(i));
    stamps.push_back(stampOf(log[i]));
  }
  const auto importBatch = [this, &log, &history, &stamps](const size_t from, const size_t count) {
    JsonDocument params, done;
    JsonArray reviews = params["reviews"].to<JsonArray>();
    for (size_t i = 0; i < count; i++) {
      const auto& e = log[history[from + i]];
      JsonObject r = reviews.add<JsonObject>();
      r["id"] = std::to_string(stamps[from + i]);
      r["cardId"] = std::to_string(e.id);
      r["rating"] = RATING_NAMES[std::clamp<int>(e.rating, 1, 4) - 1];
      // A learning step is logged in minutes and reaches Anki in seconds, as its own log keeps it.
      const bool minutes = (e.kind & flashcards::ReviewLogEntry::MINUTES) != 0;
      r["interval"]["value"] = minutes ? static_cast<uint32_t>(e.interval) * 60 : e.interval;
      r["interval"]["unit"] = minutes ? "seconds" : "days";
      r["previousInterval"]["value"] = e.previous;
      r["previousInterval"]["unit"] = "days";
      r["factor"]["encoding"] = "unknown";
      r["factor"]["value"] = 0;
      r["elapsedMs"] = 0;
      const uint8_t kind = e.kind & ~flashcards::ReviewLogEntry::MINUTES;
      r["kind"] = kind == 0 ? "learning" : (kind == 2 ? "relearning" : "review");
    }
    return client.callOperation("advanced.reviews.import", params, done, OPERATION_TIMEOUT);
  };
  for (size_t at = 0; at < history.size(); at += BATCH) {
    const size_t count = std::min(BATCH, history.size() - at);
    if (importBatch(at, count)) continue;
    if (client.error().type != "conflict") return false;
    for (size_t i = 0; i < count; i++) {
      if (!importBatch(at + i, 1) && client.error().type != "conflict") return false;
    }
  }
  // Then each reviewed card's resulting schedule.
  std::vector<uint64_t> ids;
  ids.reserve(log.size());
  for (const auto& e : log) ids.push_back(e.id);
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  std::vector<flashcards::CardState> states;
  uint16_t today = 0;
  if (!flashcards::readStates(key, ids, states, today)) return false;
  for (const auto& s : states) {
    JsonDocument due, ok;
    due["cardIds"][0] = std::to_string(s.id);
    due["days"] = std::to_string(s.due > today ? s.due - today : 0);
    if (!client.call("scheduling.setDueDate", due, ok)) return false;
    JsonDocument memory, card;
    memory["cardId"] = std::to_string(s.id);
    memory["patch"]["fsrsMemory"]["difficulty"] = std::clamp(s.difficulty, 1.0f, 10.0f);
    memory["patch"]["fsrsMemory"]["stabilityDays"] = s.stability;
    JsonDocument none;
    none["id"] = true;
    // Decks on the old SM-2 scheduler refuse an FSRS memory; their due date is set already.
    if (!client.call("advanced.cards.update", memory, card, &none)) LOG_INF("ANKI", "No FSRS memory for %llu", s.id);
  }
  result.reviewsPushed += static_cast<uint32_t>(history.size());
  // Past these reviews before they leave the log, so the next pull does not take them for Anki's.
  for (const int64_t t : stamps) info.lastReviewId = std::max(info.lastReviewId, static_cast<uint64_t>(t));
  if (!ankideck::writeInfo(info)) return false;
  return flashcards::dropReviewLog(key, log.size());
}

bool DeckSync::run(const std::string& name, const std::string& deckId, const char* presetId) {
  if (!Storage.ensureDirectoryExists(ankideck::dirFor(deckId).c_str())) return false;
  ankideck::DeckInfo info;
  if (!ankideck::readInfo(deckId, info)) {
    info.id = deckId;
    info.name = name;
  }
  info.name = name;
  readPreset(presetId, info);
  const bool leechesSent = pushLeeches(deckId, info.leechSuspend);
  retryTooLarge = info.fetchVersion < FETCH_VERSION;
  checkAllMedia = info.fetchVersion < MEDIA_STREAM_VERSION;
  this->info = &info;
  std::vector<uint64_t> ids;
  // Suspended cards leave the device; their progress stays, for when they are unsuspended.
  if (!searchCards("\"deck:" + quoted(name) + "\" -is:suspended", ids)) return false;
  {
    std::vector<uint64_t> edited;
    checkNoteTypes(name);
    if (cancelRequested()) return false;
    if (info.refetchAll && info.refetchedUpTo == 0) {
      // Every card again: their templates and styles (note type CSS and template <style>) come
      // back with them, so style.css starts over (once: a stopped refetch keeps what it rebuilt).
      Storage.remove(ankideck::cssPath(deckId).c_str());
      info.cssHashes.clear();
    }
    const bool known = !info.refetchAll && editedSince(name, info, ids.size(), edited);
    if (!refreshCards(name, deckId, ids, known ? &edited : nullptr)) return false;
  }
  saveBuried(name, deckId);
  // Only a complete refresh moves this on: the next sync asks for the notes edited since.
  if (nowMs > 0) info.cardsSyncedMs = nowMs;
  if (typesModSeen > info.typesMod) info.typesMod = typesModSeen;
  info.refetchAll = false;
  info.refetchedUpTo = 0;
  info.fetchVersion = FETCH_VERSION;
  // Pull before push: Anki's reviews land first, then the device's own go out on top.
  if (!pullReviews(info) || !pushReviews(info)) return false;
  // Past the schedule changes just made, so they are not pulled back as Anki's.
  JsonDocument latestParams, latest;
  latestParams["deck"]["id"] = deckId;
  if (client.call("reviews.getLatestId", latestParams, latest) && !latest["id"].isNull()) {
    info.lastReviewId = std::max(info.lastReviewId, toId(latest["id"]));
  }
  if (!ankideck::writeInfo(info)) return false;
  // Anki has the leeches and the card search has seen it suspend them.
  if (leechesSent) Storage.remove(ankideck::leechPath(deckId).c_str());
  return true;
}
}  // namespace

bool testConnection(AnkiClient& client, std::string& ankiVersion) {
  JsonDocument description;
  JsonDocument filter;
  filter["ankiVersion"] = true;
  filter["methods"][0]["name"] = true;
  JsonDocument none;
  none.to<JsonObject>();
  if (!client.call("api.describe", none, description, &filter)) return false;
  ankiVersion = description["ankiVersion"] | "";
  return true;
}

// ---- Mined words: handed over to Anki as notes, one deck per language

constexpr const char* MINED_NOTE_TYPE = "Matcha sentence";
constexpr size_t MINED_BATCH = 5;  // notes per request: a mined row holds a whole definition
// Fields in CSV column order (WORD .. FURIGANA); TAGS become the note's tags.
constexpr const char* MINED_FIELDS[] = {"Word",   "Reading", "Sentence",   "Definition", "Book",
                                        "Author", "Date",    "Dictionary", "Furigana"};
// The note type of docs/anki, made in Anki when it is missing (kept in step with docs/anki).
constexpr const char* MINED_FRONT = "<div class=\"sentence\">{{Sentence}}</div>\n<div class=\"book\">{{Book}}</div>";
constexpr const char* MINED_BACK =
    "<div class=\"word\">{{#Furigana}}{{furigana:Furigana}}{{/Furigana}}{{^Furigana}}{{Word}}{{/Furigana}}</div>\n"
    "<div class=\"reading\">{{Reading}}</div>\n<hr>\n<div class=\"def\">{{Definition}}</div>\n<hr>\n"
    "<div class=\"sentence small\">{{Sentence}}</div>\n<div class=\"book\">{{Book}}</div>";
constexpr const char* MINED_CSS =
    ".card {\n  font-family: \"Hiragino Mincho ProN\", \"Noto Serif JP\", \"Noto Serif SC\", serif;\n"
    "  font-size: 22px;\n  line-height: 1.6;\n  color: #111;\n  background: #fff;\n  padding: 28px 32px;\n"
    "  text-align: left;\n}\n"
    ".word { font-size: 64px; font-weight: 700; text-align: center; line-height: 1.9; }\n"
    ".word rt { font-size: 0.38em; font-weight: 400; color: #333; }\n"
    ".reading { text-align: center; color: #555; font-size: 18px; margin-top: -6px; }\n"
    ".sentence b { border-bottom: 2px solid #111; }\n.small { font-size: 19px; }\n"
    ".book {\n  font-style: italic;\n  color: #666;\n  font-size: 15px;\n  margin-top: 10px;\n"
    "  border-top: 1px solid #111;\n  padding-top: 8px;\n  display: inline-block;\n  max-width: 100%;\n}\n"
    ".def { font-size: 18px; }\nhr { border: none; border-top: 1px solid #111; margin: 16px 0; }\n"
    ".card.nightMode { color: #e8e8e8; background: #1f1f1f; }\n.nightMode .word rt { color: #ccc; }\n"
    ".nightMode .reading { color: #aaa; }\n.nightMode .book { color: #aaa; border-top-color: #e8e8e8; }\n"
    ".nightMode .sentence b { border-bottom-color: #e8e8e8; }\n.nightMode hr { border-top-color: #e8e8e8; }\n";

// A mined field without control characters other than tab and newline: ArduinoJson writes the
// rest as they are, which is not valid JSON (dictionary text can hold a 0x1F separator).
std::string minedField(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (const char c : value) {
    if (static_cast<unsigned char>(c) >= 0x20 || c == '\n' || c == '\t') out.push_back(c);
  }
  return out;
}

// A value inside a quoted field search ("Word:..."): Anki's wildcards * and _ taken literally.
std::string fieldQuoted(const std::string& value) {
  std::string out;
  out.reserve(value.size() + 4);
  for (const char c : value) {
    if (c == '"' || c == '\\' || c == '*' || c == '_') out.push_back('\\');
    out.push_back(c);
  }
  return out;
}

bool ensureMinedNoteType(AnkiClient& client) {
  JsonDocument types;
  if (!client.call("noteTypes.list", types)) return false;
  for (JsonVariantConst t : types.as<JsonArrayConst>()) {
    if (strcmp(t["name"] | "", MINED_NOTE_TYPE) == 0) return true;
  }
  JsonDocument params, created;
  params["name"] = MINED_NOTE_TYPE;
  JsonArray fields = params["fields"].to<JsonArray>();
  for (const char* f : MINED_FIELDS) fields.add<JsonObject>()["name"] = f;
  JsonObject t = params["templates"].to<JsonArray>().add<JsonObject>();
  t["name"] = "Card 1";
  t["questionHtml"] = MINED_FRONT;
  t["answerHtml"] = MINED_BACK;
  params["css"] = MINED_CSS;
  JsonDocument filter;
  filter["name"] = true;
  return client.call("noteTypes.create", params, created, &filter);
}

// The deck's id: the one used before when Anki still has it (renamed or not), else the deck of
// that name, made in Anki when it is not there yet. Empty on failure.
std::string ensureDeck(AnkiClient& client, const JsonDocument& decks, const std::string& knownId,
                       const std::string& name) {
  for (JsonVariantConst d : decks.as<JsonArrayConst>()) {
    if (!knownId.empty() && knownId == (d["id"] | "")) return knownId;
  }
  for (JsonVariantConst d : decks.as<JsonArrayConst>()) {
    if (name == (d["name"] | "")) return d["id"] | "";
  }
  JsonDocument params, deck, filter;
  params["name"] = name;
  filter["id"] = true;
  if (!client.call("decks.create", params, deck, &filter)) return {};
  return deck["id"] | "";
}

enum class Adopt : uint8_t { Found, FoundNew, None, Failed };

// A note already in Anki for this row: the same word and sentence (a sync cut off after adding
// it, or a CSV imported by hand). Its first card moves to `deckId`.
Adopt adoptMinedNote(AnkiClient& client, const flashcards::MinedRow& row, const std::string& deckId, uint64_t& cardId) {
  JsonDocument params, page;
  params["query"] = std::string("\"note:") + quoted(MINED_NOTE_TYPE) +
                    "\" \"Word:" + fieldQuoted(minedField(row.fields[flashcards::WORD])) + "\"";
  params["limit"] = 20;
  if (!client.call("notes.search", params, page)) return Adopt::Failed;
  if (page["ids"].as<JsonArrayConst>().size() == 0) return Adopt::None;
  JsonDocument ids, notes, filter;
  for (JsonVariantConst id : page["ids"].as<JsonArrayConst>()) ids["ids"].add(id.as<const char*>());
  filter["items"][0]["result"]["fields"]["Sentence"] = true;
  filter["items"][0]["result"]["cardIds"] = true;
  if (!client.call("notes.getMany", ids, notes, &filter)) return Adopt::Failed;
  for (JsonVariantConst item : notes["items"].as<JsonArrayConst>()) {
    JsonVariantConst note = item["result"];
    if (minedField(row.fields[flashcards::SENTENCE]) != (note["fields"]["Sentence"] | "")) continue;
    const char* card = note["cardIds"][0] | "";
    if (!*card) continue;
    JsonDocument move, moved;
    move["ids"][0] = card;
    move["deck"]["id"] = deckId;
    if (!client.call("cards.move", move, moved)) return Adopt::Failed;
    cardId = strtoull(card, nullptr, 10);
    // Reviewed in Anki: its schedule there stays. Still new (made by a sync cut off before its
    // hand-over): the device's progress goes to it as for a note just added.
    JsonDocument cardIds, cards, cardFilter;
    cardIds["ids"][0] = card;
    cardFilter["items"][0]["result"]["scheduling"]["type"] = true;
    if (!client.call("cards.getMany", cardIds, cards, &cardFilter)) return Adopt::Failed;
    const char* type = cards["items"][0]["result"]["scheduling"]["type"] | "new";
    return strcmp(type, "new") == 0 ? Adopt::FoundNew : Adopt::Found;
  }
  return Adopt::None;
}

void addMinedNote(JsonArray notes, const flashcards::MinedRow& row, const std::string& deckId) {
  JsonObject n = notes.add<JsonObject>();
  n["noteType"]["name"] = MINED_NOTE_TYPE;
  n["deck"]["id"] = deckId;
  for (size_t f = 0; f < std::size(MINED_FIELDS); f++) {
    n["fields"][MINED_FIELDS[f]] = minedField(row.fields[flashcards::WORD + f]);
  }
  JsonArray tags = n["tags"].to<JsonArray>();
  tags.add("matcha");
  const std::string extra = minedField(row.fields[flashcards::TAGS]);
  for (size_t at = 0; at < extra.size();) {
    size_t end = extra.find_first_of(" \t\n", at);
    if (end == std::string::npos) end = extra.size();
    if (end > at) tags.add(extra.substr(at, end - at));
    at = end + 1;
  }
}

size_t minedRowBytes(const flashcards::MinedRow& row) {
  size_t bytes = 256;  // names, ids, quoting
  for (const auto& f : row.fields) bytes += f.size();
  return bytes;
}

// Mined words not in Anki yet become "Matcha sentence" notes in Matcha::<language>, with their
// history (sent by that deck's sync, which follows). The deck joins the synced decks. A failure
// here is logged and leaves the words for the next sync: it never keeps the decks from syncing.
void handOverMined(AnkiClient& client, const ProgressFn& progress, Result& result) {
  std::vector<std::string> languages;
  flashcards::miningLanguages(languages);
  if (languages.empty()) return;
  bool typeReady = false;
  JsonDocument decks;
  if (!client.call("decks.list", decks)) return;
  for (const auto& language : languages) {
    std::vector<flashcards::MinedRow> rows;
    std::vector<uint64_t> skipped;  // rows Anki did not take this time; tried again next sync
    if (flashcards::newMinedRows(language, skipped, MINED_BATCH, rows) == 0) continue;
    if (!typeReady && !ensureMinedNoteType(client)) return;
    typeReady = true;
    const char* own = flashcards::autonym(language);
    std::string deckName = "Matcha::";
    if (own) {
      deckName += own;
    } else {
      for (const char c : language) deckName.push_back(static_cast<char>(toupper(static_cast<unsigned char>(c))));
    }
    const std::string deckId = ensureDeck(client, decks, flashcards::minedDeckId(language), deckName);
    if (deckId.empty() || !flashcards::setMinedDeckId(language, deckId)) return;
    // Synced from now on, so the handed-over words come back as an Anki deck.
    std::vector<SyncedDeck> wanted = ANKI_STORE.getSyncedDecks();
    const bool listed = std::any_of(wanted.begin(), wanted.end(),
                                    [&](const SyncedDeck& w) { return w.id == deckId || w.name == deckName; });
    if (!listed) {
      SyncedDeck d;
      d.name = deckName;
      d.id = deckId;
      wanted.push_back(std::move(d));
      ANKI_STORE.setSyncedDecks(std::move(wanted));
      ANKI_STORE.saveToFile();
    }
    const std::string key = ankideck::keyFor(deckId);
    uint32_t handed = 0;
    bool stop = false;
    do {
      if (progress.cancelled()) return;
      // As many rows as the heap holds twice over (the document and its serialized body).
      const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
      const size_t budget = largest > HEAP_HEADROOM ? (largest - HEAP_HEADROOM) / 3 : 0;
      size_t take = 0, bytes = 0;
      while (take < rows.size() && bytes + minedRowBytes(rows[take]) <= budget) bytes += minedRowBytes(rows[take++]);
      if (take == 0) {
        if (budget < 2 * 1024) {  // the heap, not the row: the rest wait for the next sync
          LOG_ERR("ANKI", "Mined words of %s left for the next sync: %u bytes free", language.c_str(),
                  static_cast<unsigned>(largest));
          break;
        }
        LOG_ERR("ANKI", "Mined word %s too large to send now", rows[0].fields[flashcards::WORD].c_str());
        skipped.push_back(rows[0].id);
        continue;
      }
      rows.resize(take);
      JsonDocument params, created;
      JsonArray notes = params["notes"].to<JsonArray>();
      for (const auto& row : rows) addMinedNote(notes, row, deckId);
      if (!client.call("notes.createMany", params, created)) return;
      std::vector<flashcards::HandedCard> cards;
      cards.reserve(rows.size());
      std::vector<bool> done(rows.size(), false);
      for (JsonVariantConst item : created["items"].as<JsonArrayConst>()) {
        if (!item["index"].is<size_t>()) continue;
        const size_t index = item["index"].as<size_t>();
        if (index >= rows.size() || done[index]) continue;
        flashcards::HandedCard card{rows[index].id, strtoull(item["result"]["cardIds"][0] | "0", nullptr, 10), false};
        if (card.cardId == 0) {
          // Refused (a duplicate word): the same note may be there already; otherwise it is added
          // anyway. A failed lookup adds nothing: it would make a duplicate after a passing error.
          const Adopt adopt = adoptMinedNote(client, rows[index], deckId, card.cardId);
          if (adopt == Adopt::Failed) {
            stop = true;
            break;
          }
          card.adopted = adopt == Adopt::Found;  // a reviewed card keeps Anki's schedule
          if (adopt == Adopt::None) {
            JsonDocument again, made;
            addMinedNote(again["notes"].to<JsonArray>(), rows[index], deckId);
            again["notes"][0]["duplicates"]["allow"] = true;
            if (client.call("notes.createMany", again, made)) {
              card.cardId = strtoull(made["items"][0]["result"]["cardIds"][0] | "0", nullptr, 10);
            }
          }
        }
        if (card.cardId == 0) continue;
        done[index] = true;
        cards.push_back(card);
      }
      // Anything not confirmed waits for the next sync rather than being asked for again now.
      for (size_t r = 0; r < rows.size(); r++) {
        if (!done[r]) {
          LOG_ERR("ANKI", "Mined word %s not added", rows[r].fields[flashcards::WORD].c_str());
          skipped.push_back(rows[r].id);
        }
      }
      if (!flashcards::handOver(language, key, cards)) return;
      handed += static_cast<uint32_t>(cards.size());
    } while (!stop && flashcards::newMinedRows(language, skipped, MINED_BATCH, rows) > 0);
    if (handed > 0) {
      LOG_INF("ANKI", "%u mined words of %s handed to %s", static_cast<unsigned>(handed), language.c_str(),
              deckName.c_str());
    }
    result.cardsAdded += handed;

    if (stop) return;
  }
}

Result syncAll(AnkiClient& client, const ProgressFn& progress, const int64_t nowMs) {
  Result result;
  int64_t nextPastMs = nowMs - 60000;
  const auto fail = [&](const std::string& deck) {
    result.failedDeck = deck;
    if (result.error.kind != AnkiClient::Error::Kind::NoMemory &&
        result.error.kind != AnkiClient::Error::Kind::Cancelled) {
      result.error = client.error();
    }
    return result;
  };
  if (ANKI_STORE.getSyncAnkiWeb()) {
    JsonDocument params, done;
    params["includeMedia"] = false;
    // Without an AnkiWeb account (a server set up without one) the sync goes on locally.
    if (!client.callOperation("collection.sync", params, done, OPERATION_TIMEOUT) &&
        client.error().type != "authenticationRequired") {
      return fail("AnkiWeb");
    }
  }
  handOverMined(client, progress, result);
  JsonDocument decks;
  if (!client.call("decks.list", decks)) return fail({});
  // A copy: the loop renames entries, and the store is saved once after it.
  std::vector<SyncedDeck> wanted = ANKI_STORE.getSyncedDecks();
  bool renamed = false;
  std::string missing;
  for (auto& w : wanted) {
    // The id follows a deck renamed in Anki. The name finds it the first time, and again when the
    // deck was re-imported under a new id.
    const char* name = nullptr;
    const char* id = nullptr;
    const char* preset = "";
    for (const bool byId : {true, false}) {
      if (byId && w.id.empty()) continue;
      for (JsonVariantConst d : decks.as<JsonArrayConst>()) {
        const char* dId = d["id"] | "";
        const char* dName = d["name"] | "";
        if (byId ? w.id == dId : w.name == dName) {
          name = dName;
          id = dId;
          preset = d["presetId"] | "";
          break;
        }
      }
      if (id) break;
    }
    if (!id || !*id) {
      LOG_ERR("ANKI", "Deck %s is not in Anki", w.name.c_str());
      if (missing.empty()) missing = w.name;
      continue;
    }
    if (w.id != id || w.name != name) {
      if (w.name != name) LOG_INF("ANKI", "Deck %s is now %s", w.name.c_str(), name);
      w.id = id;
      w.name = name;
      renamed = true;
    }
    if (progress.cancelled()) {
      result.error = AnkiClient::Error{};
      result.error.kind = AnkiClient::Error::Kind::Cancelled;
      return fail(w.name);
    }
    DeckSync deck(client, progress, result, nowMs, nextPastMs);
    if (!deck.run(w.name, w.id, preset)) return fail(w.name);
  }
  if (renamed) {
    ANKI_STORE.setSyncedDecks(std::move(wanted));
    ANKI_STORE.saveToFile();
  }
  if (ANKI_STORE.getSyncAnkiWeb()) {
    JsonDocument params, done;
    params["includeMedia"] = false;
    if (!client.callOperation("collection.sync", params, done, OPERATION_TIMEOUT) &&
        client.error().type != "authenticationRequired") {
      return fail("AnkiWeb");
    }
  }
  // The other decks are synced; a missing one still fails the sync so it is not missed.
  if (!missing.empty()) {
    result.failedDeck = missing;
    result.error = AnkiClient::Error{};
    result.error.kind = AnkiClient::Error::Kind::DeckMissing;
    return result;
  }
  result.ok = true;
  return result;
}

}  // namespace ankisync
