#include "MangaWordLookupActivity.h"

#include <Arduino.h>
#include <DictIndex.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <SdCardFontSystem.h>
#include <WordLookup.h>

#include <algorithm>
#include <cstdint>

#include "BookStats.h"
#include "CrossPointSettings.h"
#include "DefinitionTextRenderer.h"
#include "DictSourceNames.h"
#include "DictionaryDefinitionActivity.h"
#include "HapticFeedback.h"
#include "JapaneseNestedLookup.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "components/DictionaryPanel.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/SentenceMining.h"

MangaWordLookupActivity::MangaWordLookupActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                 const std::string& panelText, std::string scanCachePath,
                                                 const uint16_t pageIndex, const uint16_t panelIndex,
                                                 const int targetGlyph)
    : Activity("MangaWordLookup", renderer, mappedInput),
      targetGlyph(targetGlyph),
      scanCachePath(std::move(scanCachePath)),
      scanPage(pageIndex),
      scanPanel(panelIndex) {
  scan.initFromUtf8Text(panelText);
  initScanFromCacheOrBurst();
}

// A persisted scan for this exact text skips all scanning; otherwise start progressively.
void MangaWordLookupActivity::initScanFromCacheOrBurst() {
  // Self-heal fragmentation before the lookup allocates its scan vectors and dict caches.
  // Device telemetry showed maxAlloc degrading monotonically across open/close cycles
  // (28.7K -> 22.5K -> 19.4K ...) while total free fully recovered: font hot-group/slab
  // buffers regrown while RENDERING definitions persist past onExit and split the large
  // block the dict caches vacate. Left unchecked this ends in an allocation abort() a few
  // pages later (confirmed crash_report). Freeing font memory coalesces the heap; fonts
  // reload lazily, and the reader re-warms its caches on return anyway.
  if (ESP.getMaxAllocHeap() < 28 * 1024) {
    LOG_INF("MWLA", "Low contiguous heap (maxAlloc=%u); releasing font caches", ESP.getMaxAllocHeap());
    if (auto* fcm = renderer.getFontCacheManager()) {
      fcm->releaseAllFontMemory();
      LOG_INF("MWLA", "After font release: maxAlloc=%u", ESP.getMaxAllocHeap());
    }
  }
  if (!scanCachePath.empty() && scan.tryLoadCache(scanCachePath, scanPage, scanPanel)) {
    return;
  }
  runInitialBurst();
}

// Progressive open: scan only far enough to find the FIRST selectable word; the rest of the
// text is mapped incrementally from loop(). Manga texts are short, so this usually completes
// in well under a second anyway -- the burst just guarantees a fast open on long combined text.
void MangaWordLookupActivity::runInitialBurst() {
  const uint32_t scanStart = millis();
  LOG_INF("MWLA", "progressive scan: %u characters", static_cast<unsigned>(scan.allGlyphs.size()));
  while (!scan.isDone() && scan.selectableGlyphs.empty() && millis() - scanStart < 1500) {
    scan.step(50);
  }
  LOG_INF("MWLA", "progressive scan: first word after %u ms", millis() - scanStart);
}

void MangaWordLookupActivity::onEnter() {
  Activity::onEnter();
  // Heap telemetry for the word-lookup OOM crash hunt -- see EpubReaderWordLookupActivity.
  LOG_INF("MWLA", "onEnter heap: free=%u maxAlloc=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  // A hold on the page names its own character, which beats both the remembered position and the
  // first match: the reader pointed at something.
  if (targetGlyph >= 0) {
    const int hit = selectableForGlyph(static_cast<size_t>(targetGlyph));
    if (hit >= 0) {
      cursorIndex = hit;
      performLookup();
      requestUpdate();
      return;
    }
  }
  // A scan-cache hit remembers the last cursor position for this exact panel/page text -- see
  // EpubReaderWordLookupActivity::onEnter().
  if (scan.restoredCursorIndex != WordSelectionScan::kNoRestoredCursor &&
      scan.restoredCursorIndex < scan.selectableGlyphs.size()) {
    cursorIndex = scan.restoredCursorIndex;
    performLookup();
    requestUpdate();
    return;
  }
  const int maxIdx = static_cast<int>(scan.selectableGlyphs.size()) - 1;
  for (cursorIndex = 0; cursorIndex <= maxIdx; cursorIndex++) {
    performLookup();
    if (hasResult) break;
  }
  if (cursorIndex > maxIdx) cursorIndex = 0;
  requestUpdate();
}

int MangaWordLookupActivity::selectableForGlyph(const size_t glyph) {
  // Segmentation is sequential, so the word covering `glyph` is known once the scan has mapped a
  // word starting past it (or finished). A view's text is a few bubbles, so this is short --
  // moveCursor() scans ahead synchronously the same way.
  auto mappedPast = [&] { return !scan.selectToAllIdx.empty() && scan.selectToAllIdx.back() > glyph; };
  while (!scan.isDone() && !mappedPast()) scan.step(50);

  int nearest = -1;
  size_t nearestDistance = SIZE_MAX;
  for (size_t i = 0; i < scan.selectToAllIdx.size(); i++) {
    const size_t start = scan.selectToAllIdx[i];
    const size_t span = std::max<size_t>(scan.selectableGlyphs[i].matchLen, 1);
    if (glyph >= start && glyph < start + span) return static_cast<int>(i);
    // A character no word covers (punctuation, a particle the dictionary skipped): the closest
    // word by position, preferring the one it ends -- the same snap the book panel makes.
    const size_t distance = glyph >= start + span ? glyph - (start + span - 1) : start - glyph;
    if (distance < nearestDistance) {
      nearestDistance = distance;
      nearest = static_cast<int>(i);
    }
  }
  return nearest;
}

void MangaWordLookupActivity::onExit() {
  LOG_INF("MWLA", "onExit heap: free=%u maxAlloc=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  // Persist the current cursor position -- see EpubReaderWordLookupActivity::onExit().
  if (!scanCachePath.empty()) {
    scan.saveCache(scanCachePath, scanPage, scanPanel, static_cast<uint16_t>(cursorIndex));
  }
  // Return the dictionary cache memory (~30KB) to the pool -- see EpubReaderWordLookupActivity.
  DictIndex::releaseCaches();
  {
    RenderLock lock;
    sdFontSystem.releaseWordLookupFallback(renderer);
  }
  BookStats::addCounts(mining_.bookPath.c_str(), static_cast<uint32_t>(countedLookups_.size()), 0);
  Activity::onExit();
}

std::string MangaWordLookupActivity::miningSentence() const {
  if (cursorIndex < 0 || static_cast<size_t>(cursorIndex) >= scan.selectToAllIdx.size()) return {};
  const size_t start = scan.selectToAllIdx[static_cast<size_t>(cursorIndex)];
  const auto& glyphs = scan.allGlyphs;
  if (start >= glyphs.size()) return {};
  // One speech bubble is one paragraph of the OCR text: the sentence stays inside it.
  const uint32_t paragraph = glyphs[start].paragraphIndex;
  const size_t cap = sentencemining::MAX_SENTENCE_CODEPOINTS;
  size_t from = start;
  while (from > 0 && start - from < cap && glyphs[from - 1].paragraphIndex == paragraph) --from;
  std::string before;
  for (size_t i = from; i < start; ++i) WordSelectionScan::encodeUtf8(glyphs[i].codepoint, before);
  const size_t wordEnd = std::min(glyphs.size(), start + static_cast<size_t>(std::max(1, resultMatchLen)));
  std::string word;
  for (size_t i = start; i < wordEnd; ++i) WordSelectionScan::encodeUtf8(glyphs[i].codepoint, word);
  std::string after;
  for (size_t i = wordEnd; i < glyphs.size() && i - wordEnd < cap && glyphs[i].paragraphIndex == paragraph; ++i) {
    WordSelectionScan::encodeUtf8(glyphs[i].codepoint, after);
  }
  return sentencemining::sentenceHtml(before, word, after);
}

// The result as one panel entry, with the card a save from the panel writes.
bool MangaWordLookupActivity::resultEntries(std::vector<DictionaryDefinitionActivity::Entry>& out) {
  out.clear();
  if (!hasResult) return false;
  DictionaryDefinitionActivity::Entry entry;
  entry.headword = resultHeadword;
  entry.definition = std::move(resultDefinition);  // the panel shows it; a step looks the next word up anew
  entry.reading = resultReading;
  entry.grammar = resultGrammar;
  entry.definitionText = true;
  if (resultSource != nullptr) entry.kind = dictsource::kind(resultDict);
  const std::string_view label = dictionaryLabel() ? dictionaryLabel() : "";
  entry.dictName = std::string(label);
  auto& draft = entry.draft;
  draft.card.word = resultHeadword;
  draft.card.reading = resultReading;
  draft.card.sentence = miningSentence();
  draft.card.book = mining_.bookTitle;
  draft.card.author = mining_.bookAuthor;
  draft.card.dictionary = std::string(label.substr(0, label.find(" | ")));
  // Filed under the comic's language. Folders converted before the tool recorded one are
  // Japanese manga, which is what they were made for.
  draft.language = sentencemining::languageForDictionary("", mining_.bookLanguage);
  if (draft.language.empty()) draft.language = sentencemining::JAPANESE;
  draft.bookPath = mining_.bookPath;
  out.reserve(1);
  out.push_back(std::move(entry));
  return true;
}

bool MangaWordLookupActivity::stepWord(void* ctx, const int direction,
                                       std::vector<DictionaryDefinitionActivity::Entry>& out) {
  auto* self = static_cast<MangaWordLookupActivity*>(ctx);
  const int from = self->cursorIndex;
  self->moveCursor(direction);
  // A word that could not be read (low memory) leaves the panel on the word it shows: so does
  // the cursor, or the next step would start from a word the reader never saw.
  if (!self->resultEntries(out)) {
    self->cursorIndex = from;
    return false;
  }
  return true;
}

// The page keeps being mapped while the panel is up, so the cursor can cycle and the scan cache
// gets written on exit.
bool MangaWordLookupActivity::scanTick(void* ctx) {
  auto* self = static_cast<MangaWordLookupActivity*>(ctx);
  if (!self->scan.isDone() && self->scan.step(40)) DictIndex::logAndResetStats("progressive scan complete");
  return !self->scan.isDone();
}

// The entry opens in the shared definition panel; this activity keeps the page's word cursor
// behind it, which the panel steps through when it reads on past the entry.
void MangaWordLookupActivity::openPanel() {
  std::vector<DictionaryDefinitionActivity::Entry> entries;
  if (!resultEntries(entries)) return;
  auto panel = makeUniqueNoThrow<DictionaryDefinitionActivity>(renderer, mappedInput, std::move(entries));
  if (!panel) {
    LOG_ERR("MWLA", "OOM: definition panel");
    panelFailed = true;  // shown as low memory until Confirm retries
    requestUpdate();
    return;
  }
  panelOpen = true;
  panel->setHostTick({this, &MangaWordLookupActivity::scanTick});
  panel->setWordSteps({this, &MangaWordLookupActivity::stepWord});
  panel->setNestedLookup({nullptr, &japanese_nested::lookup});
  panel->setPageTurnReversed(pageTurnReversed);
  // Back, a tap outside and the power click all return to the comic.
  startActivityForResult(std::move(panel), [this](const ActivityResult&) { finishCancelled(); });
}

void MangaWordLookupActivity::moveCursor(int delta) {
  // Moving past the last already-discovered word while the background scan is still running:
  // scan forward just enough to reveal the next one (see EpubReaderWordLookupActivity).
  if (delta > 0 && !scan.isDone() && cursorIndex + delta >= static_cast<int>(scan.selectableGlyphs.size())) {
    const size_t want = static_cast<size_t>(cursorIndex + delta) + 1;
    while (!scan.isDone() && scan.selectableGlyphs.size() < want) {
      scan.step(50);
    }
  }
  if (scan.selectableGlyphs.empty()) return;
  const int maxIdx = static_cast<int>(scan.selectableGlyphs.size()) - 1;
  // scan.selectableGlyphs is already the pre-filtered list of positions buildSelectableGlyphs()
  // confirmed have a dictionary match -- every index in it is valid by construction. Same fix as
  // EpubReaderWordLookupActivity::moveCursor(): the previous retry-skip loop re-validated via a
  // fresh performLookup() and kept advancing past any position that didn't independently agree,
  // silently skipping entries ("every second entry is skipped" during navigation).
  int newIndex = cursorIndex + delta;
  if (scan.isDone()) {
    // Full text mapped -- cycle past the ends instead of dead-ending (see EPUB activity).
    if (newIndex < 0)
      newIndex = maxIdx;
    else if (newIndex > maxIdx)
      newIndex = 0;
  } else {
    if (newIndex < 0) newIndex = 0;
    if (newIndex > maxIdx) newIndex = maxIdx;
  }
  cursorIndex = newIndex;
  performLookup();
}

std::string MangaWordLookupActivity::buildLookupText(size_t startIdx) const {
  std::string text;
  if (startIdx >= scan.selectToAllIdx.size()) return text;

  const size_t allStart = scan.selectToAllIdx[startIdx];
  int charCount = 0;
  const int limit = scan.lookupCharLimit(startIdx);
  for (size_t i = allStart; i < scan.allGlyphs.size() && charCount < limit; i++) {
    WordSelectionScan::encodeUtf8(scan.allGlyphs[i].codepoint, text);
    charCount++;
  }
  return text;
}

void MangaWordLookupActivity::performLookup() {
  // Serialize against the render task, which reads the result strings concurrently -- see
  // EpubReaderWordLookupActivity::performLookup() for the confirmed tear/abort.
  RenderLock lock;
  // Mid-session self-heal: the open-time check can't help when the heap degrades DURING a long
  // navigation session (font glyphs loaded per rendered definition accumulate; a crash_report
  // showed the definition read inside DictIndex aborting after renders had slowed from 1.2s to
  // 6.3s as the heap ran down). Same release as at open; fonts reload lazily.
  if (ESP.getMaxAllocHeap() < 20 * 1024) {
    LOG_INF("MWLA", "Low heap mid-session (maxAlloc=%u); releasing font caches", ESP.getMaxAllocHeap());
    if (auto* fcm = renderer.getFontCacheManager()) {
      fcm->releaseAllFontMemory();
    }
  }
  // Render shows "Loading..." instead of "No match found" while this runs (fast navigation).
  lookupInFlight = true;
  performLookupImpl();
  countLookup();
  lookupInFlight = false;
}

void MangaWordLookupActivity::performLookupImpl() {
  // A tapped word shows both its vocab and grammar entries; stepping through the panel word by
  // word keeps one entry each.
  const bool tapLookup = targetGlyph >= 0;
  // The grammar entry shown after the main result: the word's own, or a longer pattern found
  // around the cursor, which wins.
  int bestGramLen = 0;
  std::string bestGramHw, bestGramDef;
  hasResult = false;
  resultHeadword.clear();
  resultSource = nullptr;
  resultDict = 0;
  resultDefinition.clear();
  resultReading.clear();
  resultGrammar.clear();
  resultDictionaryLabel.clear();
  resultMatchLen = 0;

  std::string text = buildLookupText(static_cast<size_t>(cursorIndex));
  if (text.empty()) return;

  // A number is selected together with its counter (360度, ３人) -- the scan's selectable entry
  // starts at the digits -- but the dictionary knows the counter, not "360度". Look up what
  // follows the digits and show the digits as a prefix, as the book panel does. Without this every
  // number-plus-counter word in manga answered "No match found".
  std::string digitPrefix;
  {
    size_t b = 0;
    while (b < text.size()) {
      const auto c = static_cast<unsigned char>(text[b]);
      if (c >= '0' && c <= '9') {
        b += 1;
      } else if (c == 0xEF && b + 2 < text.size() && static_cast<unsigned char>(text[b + 1]) == 0xBC &&
                 static_cast<unsigned char>(text[b + 2]) >= 0x90 && static_cast<unsigned char>(text[b + 2]) <= 0x99) {
        b += 3;  // fullwidth ０-９
      } else {
        break;
      }
    }
    if (b > 0 && b < text.size()) {
      digitPrefix = text.substr(0, b);
      text = text.substr(b);
    }
  }

  WordLookupResult result;
  if (WordLookup::lookup(text, 0, result)) {
    WordSelectionScan::stripTrailingParticle(text, result);
    hasResult = true;
    resultHeadword = digitPrefix + result.entry.headword;
    resultDefinition = std::move(result.entry.definition);
    DefinitionText::EntryMetadata metadata;
    DefinitionText::extractEntryMetadata(resultDefinition, resultHeadword, metadata);
    resultReading = std::move(metadata.reading);
    resultGrammar = std::move(metadata.grammar);
    resultDict = result.entry.sourceDict;
    resultSource = dictsource::name(resultDict);
    resultDictionaryLabel = std::move(metadata.source);

    int chars = 0;
    size_t pos = 0;
    while (pos < result.matchLength && pos < text.size()) {
      auto c = static_cast<unsigned char>(text[pos]);
      if (c < 0x80)
        pos += 1;
      else if ((c & 0xE0) == 0xC0)
        pos += 2;
      else if ((c & 0xF0) == 0xE0)
        pos += 3;
      else
        pos += 4;
      chars++;
    }
    resultMatchLen = chars;

    // Check grammar dictionary for short hiragana matches
    if (chars <= 3 && Storage.exists(DictIndex::grammarIdxPath())) {
      bool allHiragana = true;
      for (size_t b = 0; b < result.matchLength && b < text.size();) {
        auto c = static_cast<unsigned char>(text[b]);
        uint32_t cp2 = 0;
        if (c < 0x80) {
          cp2 = c;
          b += 1;
        } else if ((c & 0xE0) == 0xC0) {
          cp2 = ((c & 0x1F) << 6) | (text[b + 1] & 0x3F);
          b += 2;
        } else if ((c & 0xF0) == 0xE0) {
          cp2 = ((c & 0x0F) << 12) | ((text[b + 1] & 0x3F) << 6) | (text[b + 2] & 0x3F);
          b += 3;
        } else {
          b += 4;
        }
        if (cp2 < 0x3040 || cp2 > 0x309F) {
          allHiragana = false;
          break;
        }
      }
      if (allHiragana) {
        DictEntry gramEntry;
        if (tapLookup && DictIndex::lookupInFile(resultHeadword.c_str(), DictIndex::grammarIdxPath(),
                                                 DictIndex::grammarDatPath(), gramEntry)) {
          // Opened on a tapped word: show its grammar entry after the vocab one, not instead of it.
          bestGramLen = chars;
          bestGramHw = std::move(gramEntry.headword);
          bestGramDef = std::move(gramEntry.definition);
        } else if (!tapLookup && DictIndex::lookupInFile(resultHeadword.c_str(), DictIndex::grammarIdxPath(),
                                                         DictIndex::grammarDatPath(), gramEntry)) {
          // Stepping through every word of the panel: one entry per word keeps that quick.
          resultDefinition = std::move(gramEntry.definition);
          DefinitionText::EntryMetadata grammarMetadata;
          DefinitionText::extractEntryMetadata(resultDefinition, resultHeadword, grammarMetadata);
          resultReading = std::move(grammarMetadata.reading);
          resultGrammar = std::move(grammarMetadata.grammar);
          resultDictionaryLabel = std::move(grammarMetadata.source);
          resultDict = DictIndex::DICT_GRAMMAR;
          resultSource = dictsource::name(resultDict);
        }
      }
    }
  }

  // Grammar scan
  // Nicety on top of the main result; skip under memory pressure instead of risking an
  // allocation abort() -- see EpubReaderWordLookupActivity's grammar scan.
  if (ESP.getMaxAllocHeap() < 16 * 1024) {
    LOG_ERR("MWLA", "Skipping grammar scan, heap too low (maxAlloc=%u)", ESP.getMaxAllocHeap());
  } else if (Storage.exists(DictIndex::grammarIdxPath()) &&
             cursorIndex < static_cast<int>(scan.selectToAllIdx.size())) {
    const size_t allStart = scan.selectToAllIdx[cursorIndex];

    for (int backoff = 3; backoff >= 0; backoff--) {
      size_t scanStart = allStart;
      for (int b2 = 0; b2 < backoff && scanStart > 0; b2++) scanStart--;

      std::string gramText;
      int gCharCount = 0;
      for (size_t j = scanStart; j < scan.allGlyphs.size() && gCharCount < 12; j++) {
        WordSelectionScan::encodeUtf8(scan.allGlyphs[j].codepoint, gramText);
        gCharCount++;
      }

      for (int wLen = std::min(gCharCount, 10); wLen >= 2; wLen--) {
        size_t byteEnd = 0;
        int cnt = 0;
        for (size_t b2 = 0; b2 < gramText.size() && cnt < wLen; cnt++) {
          auto c = static_cast<unsigned char>(gramText[b2]);
          if (c < 0x80)
            b2 += 1;
          else if ((c & 0xE0) == 0xC0)
            b2 += 2;
          else if ((c & 0xF0) == 0xE0)
            b2 += 3;
          else
            b2 += 4;
          byteEnd = b2;
        }
        std::string window = gramText.substr(0, byteEnd);
        DictEntry gramEntry;
        if (DictIndex::lookupInFile(window.c_str(), DictIndex::grammarIdxPath(), DictIndex::grammarDatPath(),
                                    gramEntry)) {
          const bool sameWord = !tapLookup && gramEntry.headword == resultHeadword;
          if (!sameWord && wLen > bestGramLen) {
            bestGramLen = wLen;
            bestGramHw = std::move(gramEntry.headword);
            bestGramDef = std::move(gramEntry.definition);
          }
          break;
        }
      }
    }
  }

  if (bestGramLen > 0) {
    // Guarded reserve + appends instead of a temporary chain -- see the EPUB activity.
    const size_t mergedLen = resultDefinition.size() + bestGramHw.size() + bestGramDef.size() + 32;
    if (ESP.getMaxAllocHeap() > mergedLen + 8 * 1024) {
      resultDefinition.reserve(mergedLen);
      resultDefinition += "\n\n— Grammar: ";
      resultDefinition += bestGramHw;
      resultDefinition += " —\n";
      resultDefinition += bestGramDef;
    } else {
      LOG_ERR("MWLA", "Skipping grammar merge, heap too low (maxAlloc=%u)", ESP.getMaxAllocHeap());
    }
  }

  DefinitionText::formatEntryBody(
      resultDefinition, resultDict == DictIndex::DICT_GRAMMAR ? resultHeadword : std::string(), resultGrammar);
  requestUpdate();
}

void MangaWordLookupActivity::loop() {
  // Until a word is found: the page is still being mapped. Back leaves; Confirm or the side button
  // bound to Word Lookup looks the word up again (no match, or a lookup the heap refused).
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) || ReaderUtils::wordLookupPowerClick(mappedInput)) {
    finishCancelled();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || ReaderUtils::wordLookupSideToggle(mappedInput)) {
    panelFailed = false;
    performLookup();
    requestUpdate();
    return;
  }
  // Progressive background scan + one-time result persistence -- same structure as
  // EpubReaderWordLookupActivity::loop(), see there for the full rationale.
  if (!scan.isDone()) {
    const bool done = scan.step(40);
    if (!hasResult && !scan.selectableGlyphs.empty()) {
      performLookup();
      requestUpdate();
    }
    if (done) {
      DictIndex::logAndResetStats("progressive scan complete");
      requestUpdate();
    }
  }
  if (hasResult && !panelOpen && !panelFailed) openPanel();
}

// Shown only until the first word is found: the entry itself opens in the definition panel.
void MangaWordLookupActivity::render(RenderLock&&) {
  const auto layout = DictionaryPanel::draw(renderer, "", nullptr, nullptr, nullptr, false, false);
  const bool stillWorking = lookupInFlight || !scan.isDone();
  UITheme::drawCenteredText(renderer, layout.body, UI_12_FONT_ID, layout.body.y + layout.body.height / 2,
                            stillWorking  ? tr(STR_LOADING)
                            : panelFailed ? tr(STR_LOW_MEMORY_RETRY)
                                          : tr(STR_NO_MATCH),
                            true);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), stillWorking ? "" : tr(STR_SELECT), "", "");
  DictionaryPanel::clearButtonHints(renderer);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  // FAST only: the framebuffer holds just the page's BW plane, so a full refresh would flash the
  // whole manga page; a FAST wave drives only the panel's pixels.
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void MangaWordLookupActivity::countLookup() {
  if (!hasResult || resultHeadword.empty() || countedLookups_.size() >= MAX_COUNTED_LOOKUPS) return;
  const auto hash = static_cast<uint32_t>(std::hash<std::string>{}(resultHeadword));
  if (std::find(countedLookups_.begin(), countedLookups_.end(), hash) != countedLookups_.end()) return;
  if (countedLookups_.empty()) countedLookups_.reserve(32);
  countedLookups_.push_back(hash);
}
