#include "DictionaryWordSelectActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <Memory.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cctype>
#include <climits>
#include <cstdlib>

#include "BookStats.h"
#include "DictionaryDefinitionActivity.h"
#include "HapticFeedback.h"
#include "ReaderUtils.h"
#include "WordSelectionScan.h"
#include "components/UITheme.h"

namespace {

constexpr unsigned long POPUP_DURATION_MS = 1500;
constexpr unsigned long WORD_REPEAT_START_MS = 500;
constexpr unsigned long WORD_REPEAT_INTERVAL_MS = 500;

// A token is selectable when it has an ASCII alphanumeric or a non-ASCII
// codepoint outside U+2000-U+206F (dashes, bullets and other General
// Punctuation that appear as standalone tokens are not words).
bool isSelectableToken(const char* text) {
  for (const uint8_t* p = reinterpret_cast<const uint8_t*>(text); *p != 0; p++) {
    if (*p < 0x80) {
      if (std::isalnum(*p)) return true;
    } else if (*p == 0xE2 && (p[1] == 0x80 || p[1] == 0x81)) {
      if (p[2] == 0) break;  // truncated sequence: skipping would step past the NUL
      p += 2;                // skip the 3-byte General Punctuation codepoint
    } else {
      return true;
    }
  }
  return false;
}

void indexBuildYield(void*) { vTaskDelay(1); }

}  // namespace

void DictionaryWordSelectActivity::onEnter() {
  Activity::onEnter();
  lineHeight = renderer.getLineHeight(fontId);
  if (!lookupText.empty()) {
    performLookup();
    return;
  }
  // No null check: a failed allocation just disables the differential
  // fast path (drawHighlightWithSnapshot skips the read), keeping the
  // full-repaint path as the fallback.
  snapshot = makeUniqueNoThrow<uint8_t[]>(SNAPSHOT_CAPACITY);
  extractWords();
  // Start on the middle row's word nearest mid-screen instead of top-left:
  // any word on the page is then at most half a page of moves away.
  if (!words.empty()) {
    const int initial = closestInRow(rowCount / 2, renderer.getScreenWidth() / 2);
    if (initial >= 0) selected = initial;
  }
  // Opened by a long press on a word: start there and show the definition
  // straight away. A miss (the press landed between words) falls through to
  // ordinary selection rather than closing, so the gesture is never a dead end.
  if (!words.empty() && lookupAtX >= 0 && lookupAtY >= 0) {
    const int hit = wordAt(lookupAtX, lookupAtY);
    if (hit >= 0) {
      selected = hit;
      requestUpdate();
      performLookup();
      return;
    }
    // Missed: from here this is ordinary selection, which the reader is now driving themselves.
    // Forgetting the press keeps the close behaviour ordinary too -- otherwise a definition they
    // opened by hand would still return to the page instead of back to the words.
    lookupAtX = -1;
    lookupAtY = -1;
  }
  requestUpdate();
}

void DictionaryWordSelectActivity::extractWords() {
  words.clear();
  dropCapWord_ = -1;
  words.reserve(128);
  rowCount = 0;

  // Single walk: collect the selectable words while accumulating their text
  // and styles (~2KB transient string, freed on return). Widths are measured
  // afterwards: merging the page's codepoints into the SD font's persistent
  // advance table first keeps getTextAdvanceX on the in-RAM path instead of
  // loading glyphs from SD one overflow slot at a time.
  std::string pageText;
  pageText.reserve(2048);
  uint8_t styleMask = 0;
  int16_t pendingHyphen = -1;  // line-final word ending in '-', awaiting its remainder

  for (const auto& element : page->elements) {
    // Layout hyphenation only ever continues onto the IMMEDIATELY following text line, so
    // anything else in between (an image, a line with no usable block) disarms the join.
    if (element->getTag() != TAG_PageLine) {
      pendingHyphen = -1;
      continue;
    }
    const auto* line = static_cast<const PageLine*>(element.get());
    const auto* block = line->getBlock();
    if (!block || !block->valid()) {
      // Reset the pending hyphen too: a skipped line must not let a hyphenated word join across
      // the gap it leaves.
      pendingHyphen = -1;
      continue;
    }

    bool rowHasWords = false;
    const int lineFontId = block->getBlockStyle().resolveFontId(fontId);
    const size_t lineFirstWord = words.size();
    const bool lineHasDropCap = dropCapWord_ < 0 && block->getDropCap().present();
    const uint16_t lastWordIndex = block->wordCount() > 0 ? static_cast<uint16_t>(block->wordCount() - 1) : 0;
    const int ascender = renderer.getFontAscenderSize(lineFontId);
    const int rubyShift = block->getRubyShift(ascender);
    for (uint16_t i = 0; i < block->wordCount(); i++) {
      const char* text = block->wordText(i);
      if (!isSelectableToken(text)) continue;

      WordBox box;
      box.x = static_cast<int16_t>(line->xPos + block->wordXpos(i) + marginLeft);
      box.y = static_cast<int16_t>(line->yPos + marginTop + rubyShift);
      box.style = block->wordStyle(i);
      box.width = 0;  // measured below, once the advance table is ready
      box.row = rowCount;
      box.text = text;
      // An inline font-size (a <span> inside the block) overrides the block's font for this
      // word alone, exactly as TextBlock::render resolves it. 0 = no override.
      const int32_t wordFont = block->wordFont(i);
      box.fontId = wordFont != 0 ? static_cast<int>(wordFont) : lineFontId;
      // Link a hyphenated pair as it is discovered: the prefix was remembered when its line ended,
      // and this is the first selectable word of the next line -- the remainder.
      if (pendingHyphen >= 0) {
        words[static_cast<size_t>(pendingHyphen)].joinNext = static_cast<int16_t>(words.size());
        box.joinPrev = pendingHyphen;
        pendingHyphen = -1;
      }
      words.push_back(box);
      // A trailing hyphen on the LINE'S last word is layout hyphenation, not the author's text.
      // Remember it so the next line's first word can be joined to it.
      if (i == lastWordIndex) {
        const size_t len = strlen(text);
        if (len > 1 && text[len - 1] == '-') pendingHyphen = static_cast<int16_t>(words.size() - 1);
      }
      rowHasWords = true;

      pageText.append(text);
      pageText.push_back(' ');
      styleMask |= static_cast<uint8_t>(1u << (static_cast<uint8_t>(box.style) & 0x03));
    }
    // Only when the line put a word down: its first word is the one missing the letter.
    if (lineHasDropCap && words.size() > lineFirstWord) {
      dropCapWord_ = static_cast<int16_t>(lineFirstWord);
      dropCapCp_ = block->getDropCap().cp;
      dropCapPrefixCp_ = block->getDropCap().prefixCp;
    }
    if (rowHasWords) {
      rowCount++;
    } else {
      // A line with nothing selectable on it: same rule, the pending prefix cannot reach past it.
      pendingHyphen = -1;
    }
  }

  if (styleMask == 0) styleMask = 0x01;  // REGULAR
  renderer.ensureSdCardFontReady(fontId, pageText.c_str(), styleMask);
  for (auto& word : words) {
    word.width = static_cast<int16_t>(renderer.getTextAdvanceX(word.fontId, word.text, word.style));
  }
}

// Either half of a hyphenated pair looks up the whole word: "any-" joins the remainder that
// follows it, "one" joins the prefix before it. The hyphen itself is dropped -- layout put it
// there, the author did not. Words that are not part of a split are returned untouched, with no
// allocation.
const char* DictionaryWordSelectActivity::lookupTextFor(const size_t index, std::string& scratch) const {
  const WordBox& box = words[index];
  const WordBox* prefix = nullptr;
  const WordBox* remainder = nullptr;
  if (box.joinNext >= 0 && static_cast<size_t>(box.joinNext) < words.size()) {
    prefix = &box;
    remainder = &words[static_cast<size_t>(box.joinNext)];
  } else if (box.joinPrev >= 0 && static_cast<size_t>(box.joinPrev) < words.size()) {
    prefix = &words[static_cast<size_t>(box.joinPrev)];
    remainder = &box;
  } else {
    return box.text;
  }
  scratch.assign(prefix->text);
  if (!scratch.empty() && scratch.back() == '-') scratch.pop_back();
  scratch.append(remainder->text);
  return scratch.c_str();
}

const DictionaryWordSelectActivity::WordBox* DictionaryWordSelectActivity::joinedPartner(const size_t index) const {
  const WordBox& box = words[index];
  const int16_t other = box.joinNext >= 0 ? box.joinNext : box.joinPrev;
  if (other < 0 || static_cast<size_t>(other) >= words.size()) return nullptr;
  return &words[static_cast<size_t>(other)];
}

int DictionaryWordSelectActivity::wordHeight(const WordBox& word) const {
  return word.fontId == fontId ? lineHeight : renderer.getLineHeight(word.fontId);
}

// Index of the word whose box (with finger-sized slop) contains the touch
// point; -1 when the touch lands on no word. Boxes never overlap after the
// slop grows them, at worst they touch, so first hit wins.
int DictionaryWordSelectActivity::wordAt(const int x, const int y) const {
  constexpr int SLOP = 4;  // matches the highlight box (+2) plus finger error
  for (int i = 0; i < static_cast<int>(words.size()); i++) {
    const WordBox& word = words[i];
    if (x >= word.x - SLOP && x < word.x + word.width + SLOP && y >= word.y - SLOP &&
        y < word.y + wordHeight(word) + SLOP) {
      return i;
    }
  }
  return -1;
}

// Index of the word in `row` whose horizontal center is closest to centerX;
// -1 when the row has no words.
int DictionaryWordSelectActivity::closestInRow(const uint16_t row, const int centerX) const {
  int best = -1;
  int bestDistance = INT_MAX;
  for (int i = 0; i < static_cast<int>(words.size()); i++) {
    if (words[i].row != row) continue;
    const int distance = std::abs(words[i].x + words[i].width / 2 - centerX);
    if (distance < bestDistance) {
      bestDistance = distance;
      best = i;
    }
  }
  return best;
}

void DictionaryWordSelectActivity::moveVertical(const int direction) {
  const WordBox& current = words[selected];
  const int targetRow = static_cast<int>(current.row) + direction;
  if (targetRow < 0 || targetRow >= static_cast<int>(rowCount)) return;

  const int best = closestInRow(static_cast<uint16_t>(targetRow), current.x + current.width / 2);
  if (best >= 0 && best != selected) {
    selected = best;
    requestUpdate();
  }
}

std::string DictionaryWordSelectActivity::miningSentence(const std::string_view surface) const {
  if (words.empty() || selected < 0 || static_cast<size_t>(selected) >= words.size()) return {};
  // A hyphen-split word ("any-" / "one") is one word on the card: its other half is neither
  // before nor after it.
  const auto sel = static_cast<size_t>(selected);
  const WordBox& box = words[sel];
  const size_t first = box.joinPrev >= 0 ? static_cast<size_t>(box.joinPrev) : sel;
  const size_t last = box.joinNext >= 0 ? static_cast<size_t>(box.joinNext) : sel;
  // Words on either side, one sentence's reach at most; sentenceHtml() cuts at the ends it finds.
  constexpr size_t REACH = 80;
  // A sentence never crosses a paragraph break. The page does not mark those, but it spaces them:
  // a heading ("Chapter I", "ZWEI"), a caption or an image sits after a gap wider than the line
  // advance, and a heading ends without punctuation, so it would otherwise be read as the start
  // of the paragraph under it. The advance is the tightest one between two lines on this page.
  int minAdvance = 0;
  for (size_t i = 1; i < words.size(); ++i) {
    if (words[i].row == words[i - 1].row) continue;
    const int advance = words[i].y - words[i - 1].y;
    if (advance > 0 && (minAdvance == 0 || advance < minAdvance)) minAdvance = advance;
  }
  const auto breaksBetween = [&](const size_t a, const size_t b) {
    if (minAdvance == 0 || words[a].row == words[b].row) return false;
    return words[b].y - words[a].y > minAdvance * 13 / 10;
  };
  // A drop cap's letter belongs in front of the first word of its line ("I" + "T" = "IT").
  const auto appendWord = [this](std::string& out, const size_t index) {
    if (static_cast<int>(index) == dropCapWord_) {
      if (dropCapPrefixCp_ != 0) WordSelectionScan::encodeUtf8(dropCapPrefixCp_, out);
      WordSelectionScan::encodeUtf8(dropCapCp_, out);
    }
    out += words[index].text;
  };

  size_t from = first;
  while (from > 0 && first - from < REACH && !breaksBetween(from - 1, from)) --from;
  std::string before;
  for (size_t i = from; i < first; ++i) {
    appendWord(before, i);
    before += ' ';
  }
  std::string after;
  size_t i = last + 1;
  bool segmentEnded = false;
  for (; i < words.size() && i <= last + REACH; ++i) {
    if (breaksBetween(i - 1, i)) {
      segmentEnded = true;
      break;
    }
    after += ' ';
    appendWord(after, i);
  }
  // Ran off the bottom of the page mid-sentence: finish it from the next page's text.
  if (!segmentEnded && i >= words.size() && !sentencemining::hasSentenceEnd(after) && !mining_.nextPageText.empty()) {
    after += ' ';
    after += mining_.nextPageText;
  }
  return sentencemining::sentenceHtml(before, surface, after);
}

void DictionaryWordSelectActivity::performLookup() {
  for (size_t d = 0; d < dictCount; d++) {
    auto& slot = dicts[d];
    if (slot.openAttempted) continue;
    slot.openAttempted = true;
    slot.openOk = slot.dict.open(slot.folder.c_str(), language.c_str());
    // needsIndex() opens and validates the .qidx sidecar, so ask it once per
    // open rather than once per word: the answer only changes when we build
    // the sidecar ourselves, which is handled below.
    slot.needsIndex = slot.openOk && slot.dict.needsIndex();
  }
  // No busy popup: the lookup is fast enough that one only flashes, and the definition panel
  // draws over the page without clearing it, so a popup painted here would survive underneath
  // as debris. Failures still get their own popup below.
  {
    RenderLock lock;
    if (auto* fcm = renderer.getFontCacheManager()) fcm->releaseAllFontMemory();
  }
  LOG_DBG("DICT", "Lookup maxAlloc after font release: %u", ESP.getMaxAllocHeap());

  // Every dictionary is asked; each that has the word adds an entry, in dictionary order. The
  // failure reported when none does is the first dictionary's that could not answer.
  std::string joined;
  const char* word = !lookupText.empty() ? lookupText.c_str() : lookupTextFor(static_cast<size_t>(selected), joined);
  std::vector<DictionaryDefinitionActivity::Entry> entries;
  entries.reserve(dictCount);
  bool ok = false;
  Dictionary::IndexResult indexResult = Dictionary::IndexResult::Ok;
  Dictionary::LookupResult result = Dictionary::LookupResult::NotFound;
  for (size_t d = 0; d < dictCount; d++) {
    auto& slot = dicts[d];
    if (!slot.openOk) continue;
    if (slot.needsIndex) {
      Dictionary::IndexResult built = Dictionary::IndexResult::Ok;
      slot.needsIndex = !slot.dict.buildIndex(&indexBuildYield, nullptr, &built);
      if (slot.needsIndex) {  // a failed build retries on the next word
        if (!ok && indexResult == Dictionary::IndexResult::Ok) indexResult = built;
        continue;
      }
    }
    ok = true;
    DictionaryDefinitionActivity::Entry entry;
    Dictionary::LookupResult lookupResult = Dictionary::LookupResult::NotFound;
    if (!slot.dict.lookup(word, entry.definition, entry.headword, &lookupResult)) {
      if (result == Dictionary::LookupResult::NotFound) result = lookupResult;
      continue;
    }
    entry.html = slot.dict.definitionsAreHtml();
    entry.dictName = slot.dict.getBookName();
    auto& draft = entry.draft;
    draft.card.word = entry.headword;
    draft.card.sentence = miningSentence(word);
    draft.card.book = mining_.bookTitle;
    draft.card.author = mining_.bookAuthor;
    draft.card.dictionary = entry.dictName;
    draft.language = sentencemining::languageForDictionary(slot.folder, language);
    draft.bookPath = mining_.bookPath;
    entries.push_back(std::move(entry));
  }

  if (!entries.empty()) {
    popup = Popup::None;
    BookStats::addCounts(mining_.bookPath.c_str(), 1, 0);
    auto definitionView = makeUniqueNoThrow<DictionaryDefinitionActivity>(renderer, mappedInput, std::move(entries));
    if (!definitionView) {
      LOG_ERR("DICT", "OOM: definition view");
      return;
    }
    if (mappedInput.hasTouch()) {
      const WordBox& box = words[selected];
      const Rect wordRect{box.x, box.y, box.width, wordHeight(box)};
      LookupClipHandles handles;
      handles.set(wordRect, wordRect, /*isVertical=*/false);
      definitionView->setClipHandles(handles);
    }
    startActivityForResult(std::move(definitionView), [this](const ActivityResult& result) {
      // A drag on the word's handles: the reader opens clip selection on it.
      if (std::holds_alternative<ClipStartResult>(result.data)) {
        ActivityResult forward;
        forward.data = result.data;
        setResult(std::move(forward));
        finish();
        return;
      }
      // The definition view cancels when its power click asked to leave the dictionary
      // entirely, rather than step back to this selection.
      if (result.isCancelled) {
        finish();
        return;
      }
      // A long press opened the definition directly, so closing it returns to the
      // page. Word selection was never a step the reader asked for, and stopping
      // here would strand them in a screen they did not open.
      if ((lookupAtX >= 0 && lookupAtY >= 0) || !lookupText.empty()) {
        finish();
        return;
      }
      requestUpdate();
    });
    return;
  }
  // Name the failure: a genuine miss is "Not found"; a word that WAS found but
  // couldn't be read is a real error — and we distinguish decompression from a
  // low-memory allocation from a generic read error.
  if (!ok) {
    popup = Popup::Error;
    // An index build allocates a scan buffer, so it fails the same way lookups
    // do on a fragmented heap — name that rather than a generic error.
    switch (indexResult) {
      case Dictionary::IndexResult::LowMemory:
        popupMsg = StrId::STR_DICT_LOW_MEMORY;
        break;
      case Dictionary::IndexResult::ReadError:
        popupMsg = StrId::STR_DICT_READ_FAILED;
        break;
      case Dictionary::IndexResult::Ok:
      default:
        popupMsg = StrId::STR_DICT_ERROR;  // dict.open() failed, not the index
        break;
    }
  } else {
    switch (result) {
      case Dictionary::LookupResult::Decompress:
        popup = Popup::Error;
        popupMsg = StrId::STR_DICT_DECOMPRESS_ERROR;
        break;
      case Dictionary::LookupResult::LowMemory:
        popup = Popup::Error;
        popupMsg = StrId::STR_DICT_LOW_MEMORY;
        break;
      case Dictionary::LookupResult::ReadError:
        popup = Popup::Error;
        popupMsg = StrId::STR_DICT_READ_FAILED;
        break;
      case Dictionary::LookupResult::NotFound:
      default:
        popup = Popup::NotFound;
        popupMsg = StrId::STR_DICT_NOT_FOUND;
        break;
    }
  }
  popupTime = millis();
  requestUpdate();
}

void DictionaryWordSelectActivity::loop() {
  if (popup == Popup::NotFound || popup == Popup::Error) {
    if (millis() - popupTime >= POPUP_DURATION_MS) {
      popup = Popup::None;
      requestUpdate();
    }
    return;
  }

  // The power click looks the highlighted word up rather than leaving, matching the vertical
  // panel: while a word is selected the button means "show me this", and it only closes the
  // dictionary from the definition itself. Two clicks still leave without the hand moving.
  if (ReaderUtils::wordLookupPowerClick(mappedInput)) {
    if (!words.empty()) {
      performLookup();
      return;
    }
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  // The reader can enter this activity while Confirm is still held after a long press. Ignore
  // that stale release until a fresh press has happened here.
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) confirmPressSeen = true;
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && confirmPressSeen && !words.empty()) {
    performLookup();
    return;
  }

  if (words.empty()) return;

  // Touch: a touch-down moves the highlight to the touched word (differential
  // repaint), a tap on a word selects and looks it up in one go.
  int tx = 0;
  int ty = 0;
  if (mappedInput.wasScreenTouchDown(tx, ty)) {
    const int hit = wordAt(tx, ty);
    if (hit >= 0 && hit != selected) {
      selected = hit;
      requestUpdate();
    }
    return;
  }
  if (mappedInput.wasScreenTapped(tx, ty)) {
    const int hit = wordAt(tx, ty);
    if (hit >= 0) {
      haptic_feedback::touchAction();
      selected = hit;
      performLookup();
    }
    return;
  }

  const bool hasNextWord = selected + 1 < static_cast<int>(words.size());
  const unsigned long now = millis();
  const bool repeat =
      mappedInput.getHeldTime() >= WORD_REPEAT_START_MS && now - lastHorizontalMoveTime >= WORD_REPEAT_INTERVAL_MS;
  const bool moveLeft = mappedInput.wasPressed(MappedInputManager::Button::ScreenLeft) ||
                        (repeat && mappedInput.isPressed(MappedInputManager::Button::ScreenLeft));
  const bool moveRight = mappedInput.wasPressed(MappedInputManager::Button::ScreenRight) ||
                         (repeat && mappedInput.isPressed(MappedInputManager::Button::ScreenRight));
  if (moveLeft && selected > 0) {
    selected--;
    lastHorizontalMoveTime = now;
    requestUpdate();
  } else if (moveRight && hasNextWord) {
    selected++;
    lastHorizontalMoveTime = now;
    requestUpdate();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::ScreenUp)) {
    moveVertical(-1);
  } else if (mappedInput.wasPressed(MappedInputManager::Button::ScreenDown)) {
    moveVertical(1);
  }
}

// Saves the pixels under words[selected]'s highlight box, then draws the
// highlight over them. Returns false when the pixels could not be saved
// (no buffer / oversize box) — the highlight is drawn regardless, but the
// next cursor move must do a full repaint.
bool DictionaryWordSelectActivity::drawHighlightWithSnapshot() {
  const WordBox& word = words[selected];
  // A hyphenated word is highlighted in both halves, so the reader sees the whole word they are
  // looking up rather than the fragment on this line. The second box sits on another line, which
  // one saved region cannot restore -- so the snapshot fast path is skipped and the next cursor
  // move repaints fully. That costs a refresh only on the rare split word.
  const WordBox* partner = joinedPartner(static_cast<size_t>(selected));
  int hx = word.x - 2;
  int hy = word.y - 2;
  int hw = word.width + 4;
  int hh = wordHeight(word) + 4;
  // Clamp to the panel so save, draw and restore all use the same box.
  if (hx < 0) {
    hw += hx;
    hx = 0;
  }
  if (hy < 0) {
    hh += hy;
    hy = 0;
  }

  bool saved = false;
  if (!partner && snapshot && hw > 0 && hh > 0) {
    saved = renderer.readFramebufferRegion(hx, hy, hw, hh, snapshot.get(), SNAPSHOT_CAPACITY) > 0;
  }
  snapshotX = static_cast<int16_t>(hx);
  snapshotY = static_cast<int16_t>(hy);
  snapshotW = static_cast<int16_t>(hw);
  snapshotH = static_cast<int16_t>(hh);
  snapshotIdx = saved ? selected : -1;

  renderer.fillRect(hx, hy, hw, hh, true);
  // word.fontId, NOT the reader's font: the box was measured with the font the page laid this
  // word out in, so drawing it in a different size spills white glyphs past the black panel and
  // over the neighbouring words (the page's own ink is only erased inside the box).
  renderer.drawText(word.fontId, word.x, word.y, word.text, false, word.style);
  if (partner) {
    int px = partner->x - 2;
    int py = partner->y - 2;
    int pw = partner->width + 4;
    int ph = wordHeight(*partner) + 4;
    if (px < 0) {
      pw += px;
      px = 0;
    }
    if (py < 0) {
      ph += py;
      py = 0;
    }
    if (pw > 0 && ph > 0) {
      renderer.fillRect(px, py, pw, ph, true);
      renderer.drawText(partner->fontId, partner->x, partner->y, partner->text, false, partner->style);
    }
  }
  return saved;
}

// Front-button bar (Back/Confirm/Left/Right). Drawn last on every repaint
// path, including the differential highlight-only path, so it always ends
// up as the top layer even when a highlighted word's box falls under a
// hint's screen area. No side-button hints: the full-bleed reader page has no
// spare gutter for them, so a hint box there would hide text.
void DictionaryWordSelectActivity::drawHints() const {
  // No selectable word on this page: Confirm and navigation are all no-ops
  // (guarded by words.empty() in loop()/performLookup), so only Back does
  // anything and only Back is hinted.
  if (words.empty()) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    return;
  }
  const auto labels = mappedInput.mapDirectionalLabels(tr(STR_BACK), tr(STR_LOOKUP), tr(STR_DIR_LEFT),
                                                       tr(STR_DIR_RIGHT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void DictionaryWordSelectActivity::render(RenderLock&&) {
  // Differential fast path: only the highlight moved and the framebuffer
  // still holds a clean page (no popup or sub-activity since the last full
  // repaint). Restore the pixels under the old highlight, draw the new one,
  // and push — skipping the two-pass page render entirely.
  if (popup == Popup::None && snapshotIdx >= 0 && !words.empty() && selected != snapshotIdx) {
    renderer.writeFramebufferRegion(snapshotX, snapshotY, snapshotW, snapshotH, snapshot.get());
    // The full path's PrewarmScope cleared the glyph cache on exit; batch-load
    // just the highlighted word's glyphs before drawing them white-on-black.
    renderer.getFontCacheManager()->prewarmCache(
        words[selected].fontId, words[selected].text,
        static_cast<uint8_t>(1u << (static_cast<uint8_t>(words[selected].style) & 0x03)));
    if (drawHighlightWithSnapshot()) {
      drawHints();
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      return;
    }
    // Snapshot failed (oversize box) — fall through to a full repaint.
  }

  renderer.clearScreen();

  // Same prewarm-scan-then-render pass the reader uses, so SD-card fonts hit
  // the in-RAM glyph cache during the real draw.
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  page->render(renderer, fontId, marginLeft, marginTop);
  scope.endScanAndPrewarm();
  page->render(renderer, fontId, marginLeft, marginTop);

  if (!words.empty()) {
    drawHighlightWithSnapshot();
  }

  drawHints();

  if (popup != Popup::None) {
    // The popup overdraws the page, so the snapshot no longer matches the
    // framebuffer — force the next render onto the full-repaint path.
    snapshotIdx = -1;
    // drawPopup overlays the framebuffer and refreshes the display itself.
    // I18N.get directly: tr() only accepts literal key names.
    GUI.drawPopup(renderer, I18N.get(popupMsg));
    return;
  }
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
