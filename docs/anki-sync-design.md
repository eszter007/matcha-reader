# Anki sync design

Status: design, not built yet.

## Goal

Keep the device's flashcards and the user's Anki collection in step, in both directions:

- Words saved on the device become notes in Anki, in the user's own note type and deck.
- A review done on either side counts on the other: reviews on the device appear in Anki's
  history and move the Anki card's due date, and reviews in Anki move the device card's.

## Why AnkiConnect

AnkiWeb's own sync protocol expects the client to hold a full Anki collection (an SQLite
database, change counters on every object, zstd-compressed transfers). That does not fit the
ESP32-C3's ~380 KB of RAM. Instead the device talks to **AnkiConnect**, the Anki desktop add-on
(code 2055492159), over the local network while Anki is open. Anki then syncs to AnkiWeb as it
always does; the device can ask it to (`collection.sync`) at the end of a sync.

We use AnkiConnect's **v1 API** (JSON-RPC 2.0 at `http://<host>:<port>/api/v1`), which needs
**Anki 26.08 or later**. Only v1 can write review history with its real timestamps
(`advanced.reviews.import`) and read it back (`reviews.list`); the legacy API cannot. The
device checks `api.describe` first and says so plainly when a method is missing.

The user has to set two AnkiConnect options once, on the computer: `webBindAddress` to
`0.0.0.0` (so the device on the same Wi-Fi can reach it) and, recommended, an `apiKey`.

## Settings

All of these are edited in **Settings → Connectivity → Anki** on the device and on the web
settings page; the text fields are easier to type on the web page. They live in a new
`AnkiSyncStore` (`/.crosspoint/anki.json`), modelled on `KOReaderCredentialStore`: a
`PersistableStore`, the key XOR-obfuscated with the device secret.

| Setting | Meaning |
| --- | --- |
| Address | Host or IP of the computer running Anki, e.g. `192.168.178.20` |
| Port | Default 8765 |
| Key | AnkiConnect's `apiKey`, sent as `Authorization: Bearer` |
| Sync to AnkiWeb afterwards | Ask Anki to run its own sync when ours is done |
| Test connection | Checks reachability, key and the Anki version |

### Note type and field mapping (per language)

People already have their own note types (Kaishi, Core 2k/6k, Migaku, their own...). So
each language deck gets a mapping, stored in the same file:

- **Deck** in Anki, e.g. `Japanese::Mining`.
- **Note type**, e.g. `Japanese sentences`.
- For each Anki field, which CSV value fills it: Word, Reading, Sentence, Definition, Book,
  Author, Date, Dictionary, Furigana, or nothing. A field can also take a fixed text.
- **Card template** whose scheduling is synced, for note types with several cards per note
  (e.g. "Recognition" and "Production"). Default: the first.

The web settings page offers the choices from Anki itself: the device forwards
`decks.list`, `noteTypes.list` and `noteTypes.get` (fields and templates) through a small
endpoint, so the page shows dropdowns instead of free text. A field whose name matches a
CSV column (case-insensitive, e.g. our `docs/anki` note type) is pre-selected. Without a mapping
for a language, that deck is not synced, and the overview says so.

The device's own card layout does not change with the mapping: it always reads the CSV.

## Identity: which Anki card is which device card

AnkiConnect does not expose a note's GUID, so the CSV's GUID cannot be looked up directly.
The device keeps a **link table** per deck, `flashcards-<lang>.anki` in the system folder:
16 bytes per card (device id → Anki note id + card id), sorted like the state table.

- **Device card, no link yet:** search the mapped deck for the mapped Word field
  (`notes.search` with `"deck:X" "Field:value"`), then compare the mapped Sentence field
  (HTML stripped). One match: link it. None: create the note (`notes.createMany`) and link
  the new note and card. Several: link the first and log it.
- **Anki card deleted:** `notFound` on a linked id drops the link; the next sync relinks
  or recreates.
- Notes are never deleted on either side by sync.

## What a sync does

Sync runs from the Flashcards overview (**Sync with Anki**), only on demand: no background
Wi-Fi. It connects to Wi-Fi like KOReader Sync does, and shows progress per deck.

For each mapped deck:

1. **Link and add.** Link or create notes for device cards without a link (see above).
   Update fields only when the device has something Anki lacks (e.g. an empty Furigana field).
2. **Pull Anki's reviews.** `reviews.list` for the deck, after the newest Anki review id
   seen last time (stored per deck), in pages of 100. For each review of a linked card, fold
   it into the device state in time order: run the device's FSRS with that rating on that
   day, then take Anki's due date (review day + Anki's interval) as the device's due date.
   Anki's own intervals win for reviews done in Anki.
3. **Push the device's reviews.** Every device review since the last sync, from a new
   **review log** (`flashcards-<lang>.log`: card id, time, rating; 16 bytes each, cleared
   after a successful push):
   - `advanced.reviews.import` writes them into Anki's history with their real times, so
     Anki's statistics and its FSRS optimizer see them.
   - Then, per card, the result: `scheduling.setDueDate` to the device's due date and
     `advanced.cards.update` with the device's FSRS memory (stability, difficulty), so
     Anki shows the same next review as the device.
4. **Same card reviewed on both sides since the last sync:** the reviews are merged by time
   (both histories keep every review). The later of the two decides the due date.
5. Save the new "last seen" Anki review id and clear the pushed log, only after every step
   for that deck succeeded. A failed sync changes nothing that would make a retry wrong:
   imports use the device review's own id (its time in ms), so re-sending one is rejected as a
   duplicate rather than recorded twice.
6. If set, `collection.sync` asks Anki to sync to AnkiWeb.

### Scheduler differences

Both sides use FSRS, but with their own parameters (Anki's are optimized per preset, the
device's are the defaults with the user's retention and maximum interval). Taking the due
date from whichever side reviewed last avoids the two fighting. Anki users on the old SM-2
scheduler still get history and due dates; the FSRS memory write is skipped.

## Memory and network budget (ESP32-C3)

- One HTTP request at a time over plain HTTP on the LAN (no TLS, so no 35 KB TLS reserve).
- Requests and responses go through one 8 KB heap buffer (checked allocation, freed when
  the sync ends); `reviews.list` pages and searches are parsed with ArduinoJson filters so
  only the needed fields are kept.
- Link table: 16 bytes per card, loaded per deck (2000 cards = 32 KB, with the same
  largest-block check as the state table). Review log: streamed, not loaded.
- Batches of 50 for creating notes and importing reviews, so no request grows with the deck.
- Wi-Fi is on only during the sync and is turned off when it ends or fails.

## Failure cases

| Case | Behaviour |
| --- | --- |
| Anki closed, wrong address | "Could not reach Anki at …" with the address; nothing changed |
| Wrong key (401) / origin refused (403) | "Anki refused the key" / "Allow this device in AnkiConnect" |
| Anki older than 26.08, method missing | Names the missing method and the needed version |
| Editor open on a note (`editorConflict`) | That card is skipped and retried next time |
| Note type or field renamed in Anki | The deck is skipped; the web page shows the mapping as broken |
| Wi-Fi drops mid-sync | Steps already finished stay; the rest repeats next time |

## Files touched (planned)

- `lib/AnkiConnect/`: request building and response parsing (pure, host-tested), and the
  mapping and link-table formats.
- `src/flashcards/AnkiSync.{h,cpp}`: the sync itself, on top of `FlashcardStore`.
- `src/flashcards/FlashcardStore.*`: the review log, and applying a pulled review.
- `src/AnkiSyncStore.*`, `SettingsList.cpp`, `SettingsActivity.cpp`: settings in Connectivity.
- Web server: the mapping page and the endpoint that forwards Anki's decks and note types.
- `USER_GUIDE.md`, `README.md`, `docs/anki/README.md`: setup on the computer and the device.

## Testing

- Host tests: request JSON, response parsing, the link table and the review merge.
- A local Anki 26.08+ with AnkiConnect, driven by a script, as the server in an end-to-end
  test (`tools/anki_sync_test/`), using the device simulator.
- On the X4: sync with a real Anki on the same Wi-Fi, reviews on both sides, heap in the log.
