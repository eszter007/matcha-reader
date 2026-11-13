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

namespace ankisync {
namespace {
constexpr size_t BATCH = 50;                    // ids per request: no request grows with the deck
constexpr size_t CHECKPOINT_BATCHES = 2;        // index saved every 100 cards: a cut-off sync resumes
constexpr size_t MAX_CARD_XHTML = 40 * 1024;    // one side as stored
constexpr size_t MAX_CSS = 32 * 1024;           // a deck's styling
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

bool writeText(const std::string& path, const std::string& text) {
  HalFile file;
  if (!Storage.openFileForWrite("ANKI", path, file)) return false;
  return file.write(reinterpret_cast<const uint8_t*>(text.data()), text.size()) == text.size();
}

// One side as a complete XHTML document for the chapter parser.
bool writeSide(const std::string& path, const char* html, const size_t limit, std::string& scratch, std::string& css) {
  if (!ankihtml::toXhtml(html, scratch, limit)) return false;
  // WebP is drawn from a PNG copy (see fetchMedia).
  for (size_t at = 0; (at = scratch.find(".webp\"", at)) != std::string::npos; at += 9) {
    scratch.insert(at + 5, ".png");
  }
  std::string styles;
  // A template's own <style> arrives with every card: kept once.
  if (ankihtml::extractStyles(html, styles, MAX_CSS) && !styles.empty() && css.find(styles) == std::string::npos &&
      css.size() + styles.size() <= MAX_CSS) {
    css += styles;
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

bool fetchMedia(AnkiClient& client, const std::string& dir, const std::string& name) {
  const std::string path = dir + name;
  if (Storage.exists(path.c_str())) return true;
  // Only what the device can draw. A WebP image is referenced as "<name>.webp.png": the companion
  // server sends it converted to PNG; Anki desktop sends WebP, which is not kept.
  const bool webp = name.size() > 9 && name.compare(name.size() - 9, 9, ".webp.png") == 0;
  if (!webp && !ImageDecoderFactory::isFormatSupported(name)) return true;
  const std::string request = webp ? name.substr(0, name.size() - 4) : name;
  JsonDocument params, result;
  params["filename"] = request;
  if (!client.call("media.get", params, result)) return false;
  const char* data = result["data"] | "";
  const size_t len = strlen(data);
  const size_t capacity = len / 4 * 3 + 3;
  auto bytes = makeUniqueNoThrow<uint8_t[]>(capacity);
  if (!bytes) {
    LOG_ERR("ANKI", "OOM: image %s", name.c_str());
    return false;
  }
  size_t size = 0;
  if (mbedtls_base64_decode(bytes.get(), capacity, &size, reinterpret_cast<const unsigned char*>(data), len) != 0) {
    LOG_ERR("ANKI", "Image %s: bad base64", name.c_str());
    return false;
  }
  static constexpr uint8_t PNG_MAGIC[4] = {0x89, 'P', 'N', 'G'};
  if (webp && (size < 4 || memcmp(bytes.get(), PNG_MAGIC, 4) != 0)) return true;  // still WebP: not drawable
  HalFile file;
  if (!Storage.openFileForWrite("ANKI", path, file)) return false;
  return file.write(bytes.get(), size) == size;
}

uint32_t fnv1a(const char* text) {
  uint32_t hash = 2166136261u;
  for (; *text; ++text) hash = (hash ^ static_cast<uint8_t>(*text)) * 16777619u;
  return hash;
}

class DeckSync {
 public:
  DeckSync(AnkiClient& client, const ProgressFn& progress, Result& result, const int64_t nowMs, int64_t& nextPastMs)
      : client(client), progress(progress), result(result), nowMs(nowMs), nextPastMs(nextPastMs) {}

  bool run(const std::string& name, const std::string& deckId);

 private:
  // Card ids matching an Anki search, sorted; at most MAX_CARDS, `cut` set when there were more.
  bool searchCards(const std::string& query, std::vector<uint64_t>& ids, bool* cut = nullptr);
  // Cards whose notes changed since the deck's last full refresh. False when that is unknown, the
  // list is incomplete or would save little, and every card's note is then checked.
  bool editedSince(const std::string& name, const ankideck::DeckInfo& info, size_t deckSize,
                   std::vector<uint64_t>& edited);
  // Note id and last change for ids[from, to). A reply too large for the buffer (notes with long
  // fields) is fetched again in halves.
  bool fetchNoteMods(const std::vector<uint64_t>& ids, size_t from, size_t to, std::vector<uint64_t>& noteIds,
                     std::vector<uint32_t>& noteMods);
  // edited: cards whose notes may have changed, sorted; nullptr when any may have.
  bool refreshCards(const std::string& name, const std::string& deckId, const std::vector<uint64_t>& ids,
                    const std::vector<uint64_t>* edited);
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
  if (searchCards("\"deck:" + name + "\" edited:" + std::to_string(days), edited, &cut) && !cut &&
      edited.size() <= deckSize / 2) {
    return true;
  }
  // Checking every note works without the list: a failure here does not fail the sync.
  result.error = AnkiClient::Error{};
  edited.clear();
  edited.shrink_to_fit();
  return false;
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
  std::string css;
  if (Storage.exists(ankideck::cssPath(deckId).c_str())) {
    HalFile f;
    if (Storage.openFileForRead("ANKI", ankideck::cssPath(deckId), f)) {
      css.resize(std::min<size_t>(f.fileSize(), MAX_CSS));
      css.resize(std::max(0, f.read(css.data(), css.size())));
    }
  }
  const size_t cssBefore = css.size();
  // A side is no longer than the reply it came in.
  const size_t sideLimit = std::min(MAX_CARD_XHTML, client.capacity());
  std::vector<uint32_t> cssSeen;  // hashes of the note types' CSS already handled
  std::string scratch;

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
  uint32_t kept = 0, checkpoints = 0;
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
      if ((had & bit) && edited && !std::binary_search(edited->begin(), edited->end(), ids[i])) {
        keep |= bit;
      } else {
        need.push_back(ids[i]);
      }
    }
    // The other notes, for their last change; noteIds and noteMods follow need's order.
    noteIds.clear();
    noteMods.clear();
    if (!fetchNoteMods(need, 0, need.size(), noteIds, noteMods)) return false;
    size_t next = 0;
    for (size_t i = at; i < end; i++) {
      const ankideck::CardRecord& old = olds[i - at];
      if (keep & (1ULL << (i - at))) {
        if (!fresh.add(old)) return false;
        kept++;
        continue;
      }
      ankideck::CardRecord r;
      r.cardId = ids[i];
      r.noteId = noteIds[next];
      r.noteMod = noteMods[next];
      next++;
      const bool wasHere = (had & (1ULL << (i - at))) != 0;
      // A card too large stays so until its note changes: it is not fetched again meanwhile.
      const bool current =
          wasHere && old.noteMod == r.noteMod &&
          ((old.flags & ankideck::TOO_LARGE) != 0 || Storage.exists(ankideck::frontPath(deckId, r.cardId).c_str()));
      if (current) {
        kept++;
        if (!fresh.add(old)) return false;
        continue;
      }
      JsonDocument renderParams, rendered;
      renderParams["id"] = std::to_string(r.cardId);
      if (!client.call("cards.render", renderParams, rendered)) {
        if (client.error().kind != AnkiClient::Error::Kind::TooLarge) return false;
        r.flags |= ankideck::TOO_LARGE;  // shown as a notice, tried again when the note changes
        changed = true;
        if (!fresh.add(r)) return false;
        continue;
      }
      // Room for the cleaned side, about as long as the side itself. The reply is in already; it
      // fails softly (BadReply) when the heap is short.
      const size_t sideBytes = std::max(strlen(rendered["questionHtml"] | ""), strlen(rendered["answerHtml"] | ""));
      if (!roomFor(std::min(sideBytes + 1024, sideLimit), "a card")) return false;
      const char* cardCss = rendered["css"] | "";
      // Each note type's CSS once: decks share one style file.
      const uint32_t cssHash = fnv1a(cardCss);
      if (*cardCss && std::find(cssSeen.begin(), cssSeen.end(), cssHash) == cssSeen.end()) {
        cssSeen.push_back(cssHash);
        const size_t size = strlen(cardCss);
        if (css.find(cardCss) == std::string::npos && css.size() + size + 1 <= MAX_CSS) {
          // Grows late in a sync, when little heap is left: the new block is checked first.
          const size_t needed = css.size() + size + 1;
          if (needed > css.capacity()) {
            const size_t grown = std::min(MAX_CSS, std::max(needed, css.capacity() * 2));
            if (!roomFor(grown, "deck CSS")) return false;
            css.reserve(grown);
          }
          css += cardCss;
          css += '\n';
        }
      }
      std::vector<std::string> media;
      if (!writeSide(ankideck::frontPath(deckId, r.cardId), rendered["questionHtml"] | "", sideLimit, scratch, css)) {
        r.flags |= ankideck::TOO_LARGE;
      } else {
        mediaNames(scratch, media);
        if (!writeSide(ankideck::backPath(deckId, r.cardId), rendered["answerHtml"] | "", sideLimit, scratch, css)) {
          r.flags |= ankideck::TOO_LARGE;
        } else {
          mediaNames(scratch, media);
        }
      }
      for (const auto& file : media) {
        if (!fetchMedia(client, dir, file)) LOG_ERR("ANKI", "Image %s not fetched", file.c_str());
      }
      if (!wasHere) result.cardsAdded++;
      changed = true;
      if (!fresh.add(r)) return false;
    }
    p.done = static_cast<uint32_t>(end);
    progress(p);
    if (changed && end < ids.size() && (at / BATCH + 1) % CHECKPOINT_BATCHES == 0) {
      // The CSS first: the saved cards' sides link to it.
      if (css.size() != cssBefore && !writeText(ankideck::cssPath(deckId), css)) return false;
      dropRemoved(deckId, known, ids, checkedUpTo, ids[end - 1]);
      if (!fresh.checkpoint(known, ids[end - 1], checkedUpTo)) return false;
      checkpoints++;
      changed = false;
    }
  }

  LOG_INF("ANKI", "%s: %u cards, %u unchanged, %u index saves", name.c_str(), static_cast<unsigned>(ids.size()),
          static_cast<unsigned>(kept), static_cast<unsigned>(checkpoints));
  dropRemoved(deckId, known, ids, checkedUpTo, UINT64_MAX);
  if (css.size() != cssBefore && !writeText(ankideck::cssPath(deckId), css)) return false;
  known.close();  // before the new index replaces it
  return fresh.commit();
}

bool DeckSync::pullReviews(ankideck::DeckInfo& info) {
  // Applied and remembered a page at a time: a sync cut off later neither applies a page twice
  // nor holds a long history in RAM.
  constexpr size_t PAGE = 200;
  std::vector<flashcards::PulledReview> pulled;
  if (!roomFor(PAGE * sizeof(flashcards::PulledReview), "reviews")) return false;
  pulled.reserve(PAGE);
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
    params["limit"] = PAGE;
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
      if (pulled.size() >= PAGE) break;  // the server sent more than asked: the rest come next sync
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
  std::vector<int64_t> stamps;
  stamps.reserve(log.size());  // at most MAX_LOG_ENTRIES
  for (const auto& e : log) stamps.push_back(stampOf(e));
  const auto importBatch = [this, &log, &stamps](const size_t from, const size_t count) {
    JsonDocument params, done;
    JsonArray reviews = params["reviews"].to<JsonArray>();
    for (size_t i = 0; i < count; i++) {
      const auto& e = log[from + i];
      JsonObject r = reviews.add<JsonObject>();
      r["id"] = std::to_string(stamps[from + i]);
      r["cardId"] = std::to_string(e.id);
      r["rating"] = RATING_NAMES[std::clamp<int>(e.rating, 1, 4) - 1];
      r["interval"]["value"] = e.interval;
      r["interval"]["unit"] = "days";
      r["previousInterval"]["value"] = e.previous;
      r["previousInterval"]["unit"] = "days";
      r["factor"]["encoding"] = "unknown";
      r["factor"]["value"] = 0;
      r["elapsedMs"] = 0;
      r["kind"] = e.kind == 0 ? "learning" : (e.kind == 2 ? "relearning" : "review");
    }
    return client.callOperation("advanced.reviews.import", params, done, OPERATION_TIMEOUT);
  };
  for (size_t at = 0; at < log.size(); at += BATCH) {
    const size_t count = std::min(BATCH, log.size() - at);
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
  result.reviewsPushed += static_cast<uint32_t>(log.size());
  // Past these reviews before they leave the log, so the next pull does not take them for Anki's.
  for (const int64_t t : stamps) info.lastReviewId = std::max(info.lastReviewId, static_cast<uint64_t>(t));
  if (!ankideck::writeInfo(info)) return false;
  return flashcards::dropReviewLog(key, log.size());
}

bool DeckSync::run(const std::string& name, const std::string& deckId) {
  if (!Storage.ensureDirectoryExists(ankideck::dirFor(deckId).c_str())) return false;
  ankideck::DeckInfo info;
  if (!ankideck::readInfo(deckId, info)) {
    info.id = deckId;
    info.name = name;
  }
  info.name = name;
  std::vector<uint64_t> ids;
  if (!searchCards("\"deck:" + name + "\"", ids)) return false;
  {
    std::vector<uint64_t> edited;
    const bool known = editedSince(name, info, ids.size(), edited);
    if (!refreshCards(name, deckId, ids, known ? &edited : nullptr)) return false;
  }
  // Only a complete refresh moves this on: the next sync asks for the notes edited since.
  if (nowMs > 0) info.cardsSyncedMs = nowMs;
  // Pull before push: Anki's reviews land first, then the device's own go out on top.
  if (!pullReviews(info) || !pushReviews(info)) return false;
  // Past the schedule changes just made, so they are not pulled back as Anki's.
  JsonDocument latestParams, latest;
  latestParams["deck"]["id"] = deckId;
  if (client.call("reviews.getLatestId", latestParams, latest) && !latest["id"].isNull()) {
    info.lastReviewId = std::max(info.lastReviewId, toId(latest["id"]));
  }
  return ankideck::writeInfo(info);
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

Result syncAll(AnkiClient& client, const ProgressFn& progress, const int64_t nowMs) {
  Result result;
  int64_t nextPastMs = nowMs - 60000;
  const auto fail = [&](const std::string& deck) {
    result.failedDeck = deck;
    if (result.error.kind != AnkiClient::Error::Kind::NoMemory) result.error = client.error();
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
  JsonDocument decks;
  if (!client.call("decks.list", decks)) return fail({});
  for (const auto& wanted : ANKI_STORE.getSyncedDecks()) {
    std::string deckId;
    for (JsonVariantConst d : decks.as<JsonArrayConst>()) {
      if (wanted.name == (d["name"] | "")) deckId = d["id"].as<const char*>();
    }
    if (deckId.empty()) {
      LOG_ERR("ANKI", "Deck %s is not in Anki", wanted.name.c_str());
      continue;
    }
    DeckSync deck(client, progress, result, nowMs, nextPastMs);
    if (!deck.run(wanted.name, deckId)) return fail(wanted.name);
  }
  if (ANKI_STORE.getSyncAnkiWeb()) {
    JsonDocument params, done;
    params["includeMedia"] = false;
    if (!client.callOperation("collection.sync", params, done, OPERATION_TIMEOUT) &&
        client.error().type != "authenticationRequired") {
      return fail("AnkiWeb");
    }
  }
  result.ok = true;
  return result;
}

}  // namespace ankisync
