#include "DictionaryDefinitionActivity.h"

#include <Arduino.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "BookStats.h"
#include "CrossPointSettings.h"
#include "HapticFeedback.h"
#include "JapaneseNestedLookup.h"
#include "PanelInput.h"
#include "ReaderUtils.h"
#include "SdCardFontSystem.h"
#include "components/DictionaryPanel.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/DictHtmlPages.h"
#include "util/HtmlToPlainText.h"
#include "util/SentenceMining.h"

namespace {

// Styled-path ceiling: the laid-out Pages keep the whole definition resident
// (TextBlock arenas ≈ text + ~7 bytes/word plus per-line objects), roughly
// doubling the string's footprint while this activity is stacked over the
// reader and word-select. Bigger definitions take the span-based plain-text
// path, which holds no per-page copies.
constexpr size_t MAX_STYLED_HTML_BYTES = 16 * 1024;

// Contiguous heap a nested lookup needs before it starts: the next panel lays out a definition of
// up to MAX_STYLED_HTML_BYTES while this one keeps its own pages.
constexpr uint32_t NESTED_LOOKUP_MIN_HEAP = 24 * 1024;
// Panels a chain may open over the first. The ESP32-C3 boards (X4, X3) take one: measured on an X4,
// each Japanese lookup there drew free heap from ~45KB down to under 3KB at its peak.
#if defined(CONFIG_IDF_TARGET_ESP32C3) && CONFIG_IDF_TARGET_ESP32C3
constexpr uint8_t MAX_NESTED_LEVELS = 1;
#else
constexpr uint8_t MAX_NESTED_LEVELS = 7;  // eight panels in all
#endif

// The word in a token: surrounding punctuation ("(see", "word;") is not part of it.
std::string trimToken(const char* text, const size_t len) {
  static constexpr char kPunct[] = "()[]{}<>\"'.,;:!?*/";
  size_t start = 0;
  size_t end = len;
  while (start < end && std::strchr(kPunct, text[start]) != nullptr) start++;
  while (end > start && std::strchr(kPunct, text[end - 1]) != nullptr) end--;
  return std::string(text + start, end - start);
}

}  // namespace

// Serif at the Word Lookup Font Size. The Japanese panels stay on the sans faces -- their text
// is CJK, which the Latin serif does not carry -- so only this path is remapped. There is no 8pt
// serif, so Tiny lands on the 12pt face.
int DictionaryDefinitionActivity::definitionFontId() {
  switch (SETTINGS.wordLookupFontSize) {
    case CrossPointSettings::WORD_LOOKUP_FONT_MEDIUM:
      return NOTOSERIF_14_FONT_ID;
    case CrossPointSettings::WORD_LOOKUP_FONT_LARGE:
      return NOTOSERIF_16_FONT_ID;
    case CrossPointSettings::WORD_LOOKUP_FONT_TINY:
    case CrossPointSettings::WORD_LOOKUP_FONT_SMALL:
    default:
      return NOTOSERIF_12_FONT_ID;
  }
}

uint8_t DictionaryDefinitionActivity::definitionPointSize() {
  switch (SETTINGS.wordLookupFontSize) {
    case CrossPointSettings::WORD_LOOKUP_FONT_MEDIUM:
      return 14;
    case CrossPointSettings::WORD_LOOKUP_FONT_LARGE:
      return 16;
    case CrossPointSettings::WORD_LOOKUP_FONT_TINY:
    case CrossPointSettings::WORD_LOOKUP_FONT_SMALL:
    default:
      return 12;
  }
}

void DictionaryDefinitionActivity::ensureGlyphFallback() const {
  sdFontSystem.ensureWordLookupFallback(renderer, definitionFontId(), definitionPointSize());
}

DictionaryDefinitionActivity::DictionaryDefinitionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                           std::vector<Entry> entries)
    : Activity("DictionaryDefinition", renderer, mappedInput), entries(std::move(entries)) {
  for (auto& entry : this->entries) prepareDraft(entry);
}

// The card keeps the definition as it came from the dictionary; the layout frees its copy.
void DictionaryDefinitionActivity::prepareDraft(Entry& entry) {
  if (!entry.draft.valid()) return;
  entry.draft.card.definition =
      entry.html ? sentencemining::capHtml(entry.definition) : sentencemining::definitionHtml(entry.definition);
}

// One more word's entries, between input polls. Under the render lock only for the append: the
// render task reads the entry on screen.
void DictionaryDefinitionActivity::fetchMoreEntries() {
  std::vector<Entry> fresh;
  const bool moreToCome = more.next(more.ctx, entries, fresh);
  RenderLock lock;
  if (!moreToCome) {
    if (more.failure) emptyMessage = more.failure(more.ctx);
    more = MoreEntries{};
  }
  if (!fresh.empty()) {
    if (entries.empty()) BookStats::addCounts(fresh.front().draft.bookPath.c_str(), 1, 0);
    entries.reserve(entries.size() + fresh.size());
    for (auto& entry : fresh) {
      prepareDraft(entry);
      entries.push_back(std::move(entry));
    }
  }
  // A reader on the Loading page lands on the first new entry, or on the last one at the end.
  if (onLoadingPage && !entries.empty() && (!fresh.empty() || !more.next)) {
    onLoadingPage = false;
    showEntry(fresh.empty() ? entries.size() - 1 : entries.size() - fresh.size(), fresh.empty());
  }
  // A word with no entry changes nothing on screen: no e-ink refresh for it.
  if (!fresh.empty() || !moreToCome) requestUpdate();
}

std::string DictionaryDefinitionActivity::footerLabel() const {
  if (!showingEntry()) return {};
  std::string label = entries[currentEntry].dictName;
  if (entries.size() > 1) {
    char place[16];
    snprintf(place, sizeof(place), " (%u/%u)", static_cast<unsigned>(currentEntry + 1),
             static_cast<unsigned>(entries.size()));
    label += place;
  }
  return label;
}

void DictionaryDefinitionActivity::onEnter() {
  // A nested panel keeps the placement of the panel it opened from, so it covers that one exactly.
  if (depth == 0) DictionaryPanel::setAvoid(clipHandles.band(renderer));  // before the entries are laid out
  ensureGlyphFallback();
  Activity::onEnter();
  if (entries.empty()) {
    if (!more.next) {
      finish();
      return;
    }
    onLoadingPage = true;  // nothing yet for the whole text; its words are on their way
    requestUpdate();
    return;
  }
  showEntry(0, false);
  requestUpdate();
}

void DictionaryDefinitionActivity::showEntry(const size_t index, const bool atLastPage) {
  currentEntry = index;
  const Entry& entry = entries[index];
  pages.clear();
  textPages.clear();
  definition = entry.definition;
  std::replace(definition.begin(), definition.end(), '\0', '\n');
  nestedHost.drawnText.valid = false;
  if (entry.definitionText) {
    // Paged by whole lines of the shared wrapper: a dry run with no room to draw counts them.
    const BodyArea body = bodyArea();
    const Rect area{body.x, body.y, body.width, body.height};
    const int fontId = DefinitionText::wordLookupFontId();
    const uint16_t scale = DefinitionText::wordLookupFontScale();
    const int lineHeight = std::max(1, renderer.getLineHeightScaled(fontId, scale));
    const int metadataLines = DefinitionText::entryMetadataLineCount(renderer, area, fontId, scale,
                                                                     {entry.reading, entry.grammar, std::string()});
    const auto wrap = DefinitionText::drawWrapped(renderer, fontId, definition, body.x, body.y, lineHeight, body.width,
                                                  body.y, 0, scale);
    definitionTextLines = std::max(1, body.height / lineHeight);
    totalPages = std::max(1, (metadataLines + wrap.totalLines + definitionTextLines - 1) / definitionTextLines);
    currentPage = atLastPage ? totalPages - 1 : 0;
    miningFeedback_.clear();
    return;
  }
  if (!(entry.html && definition.size() <= MAX_STYLED_HTML_BYTES && layoutHtmlPages())) {
    definition = htmlToPlainText(definition);
    const BodyArea body = bodyArea();
    textPages.layout(renderer, definitionFontId(), definition, body.width, body.height);
    totalPages = textPages.pageCount();
  }
  currentPage = atLastPage ? totalPages - 1 : 0;
  miningFeedback_.clear();
}

bool DictionaryDefinitionActivity::stepPage(const int direction) {
  if (direction > 0) {
    if (currentPage + 1 < totalPages) {
      currentPage++;
    } else if (currentEntry + 1 < entries.size()) {
      RenderLock lock;  // the render task draws from the pages this replaces
      showEntry(currentEntry + 1, false);
    } else if (more.next && !onLoadingPage) {
      onLoadingPage = true;
    } else {
      return false;
    }
  } else {
    if (onLoadingPage) {
      if (entries.empty()) return false;
      onLoadingPage = false;  // back onto the last entry's last page, still laid out
    } else if (currentPage > 0) {
      currentPage--;
    } else if (currentEntry > 0) {
      RenderLock lock;
      showEntry(currentEntry - 1, true);
    } else {
      return false;
    }
  }
  requestUpdate();
  return true;
}

bool DictionaryDefinitionActivity::skipEntry(const int direction) {
  if (direction > 0) {
    if (onLoadingPage) return false;
    if (currentEntry + 1 < entries.size()) {
      RenderLock lock;
      showEntry(currentEntry + 1, false);
    } else if (more.next) {
      onLoadingPage = true;
    } else {
      return false;
    }
  } else {
    if (onLoadingPage) {
      if (entries.empty()) return false;
      onLoadingPage = false;
      RenderLock lock;
      showEntry(currentEntry, false);
    } else if (currentEntry > 0) {
      RenderLock lock;
      showEntry(currentEntry - 1, false);
    } else if (currentPage > 0) {
      currentPage = 0;
    } else {
      return false;
    }
  }
  requestUpdate();
  return true;
}

void DictionaryDefinitionActivity::onExit() {
  if (depth == 0) DictionaryPanel::setAvoid(Rect{});
  Activity::onExit();
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->releaseAllFontMemory();
  }
  {
    RenderLock lock;  // it frees fonts the render task may still be drawing with
    sdFontSystem.releaseWordLookupFallback(renderer);
  }
}

DictionaryDefinitionActivity::BodyArea DictionaryDefinitionActivity::bodyArea() const {
  const auto body = DictionaryPanel::compute(renderer).body;
  return {body.x, body.y, body.width, body.height};
}

// Styled path: lay the HTML definition out through the EPUB chapter parser
// into reader-identical Pages. Frees `definition` on success (the page arenas
// own the text); any failure leaves state untouched for the plain-text path.
bool DictionaryDefinitionActivity::layoutHtmlPages() {
  const BodyArea body = bodyArea();
  if (body.width <= 0 || body.height <= 0) return false;
  // Warm the advance table for this exact text BEFORE the parser measures it. Layout prices
  // non-resident glyphs at 0 (it reads advance tables and resident glyphs only, never the
  // on-demand SD loader), while drawing resolves them for real -- so an entry carrying glyphs
  // outside the resident set, such as the IPA in a pronunciation, measured as almost nothing,
  // never wrapped, and drew its characters on top of each other. REGULAR|BOLD|ITALIC: the
  // normalizer emits all three.
  renderer.ensureSdCardFontReady(definitionFontId(), definition.c_str(), 0x07);
  if (!buildDictionaryHtmlPages(renderer, definition, static_cast<uint16_t>(body.width),
                                static_cast<uint16_t>(body.height), definitionFontId(), pages)) {
    return false;
  }
  definition.clear();
  definition.shrink_to_fit();
  totalPages = static_cast<int>(pages.size());
  currentPage = 0;
  return true;
}

void DictionaryDefinitionActivity::saveSentence() {
  if (!showingEntry()) return;
  auto& draft = entries[currentEntry].draft;
  if (!draft.valid()) return;
  draft.card.date = sentencemining::today();  // the day of the save, not of the lookup
  const auto result = miningFeedback_.saved() ? sentencemining::SaveResult::AlreadySaved
                                              : sentencemining::saveOnce(draft.card, draft.language);
  if (result == sentencemining::SaveResult::Saved) BookStats::addCounts(draft.bookPath.c_str(), 0, 1);
  miningFeedback_.record(result != sentencemining::SaveResult::Failed, millis());
  requestUpdate();
}

void DictionaryDefinitionActivity::loop() {
  if (miningFeedback_.popupExpired(millis())) requestUpdate();
  // One word per tick; input below still runs every tick, so Back closes the view at once.
  if (more.next && !RenderLock::peek()) fetchMoreEntries();
  if (ClipStartResult clip; clipHandles.pressed(renderer, mappedInput, clip)) {
    ActivityResult result;
    result.data = clip;
    setResult(std::move(result));
    finish();
    return;
  }
  const auto input = PanelInput::read(renderer, mappedInput, &nestedHost, miningAvailable(),
                                      nested.lookup && showingEntry(), pageTurnReversed);
  if (input.redraw) requestUpdate();
  switch (input.event) {
    case PanelInput::Event::CursorPick: {
      std::string word;
      if (entries[currentEntry].definitionText) {
        word = japanese_nested::lookupTextAt(definition, input.tag);
      } else if (input.tag < cursorWords.size()) {
        word = std::move(cursorWords[input.tag]);
      }
      std::vector<std::string>{}.swap(cursorWords);
      if (!word.empty()) lookUpNested(word);
      return;
    }
    case PanelInput::Event::None:
      if (!nestedHost.cursorActive() && !cursorWords.empty()) std::vector<std::string>{}.swap(cursorWords);
      return;
    case PanelInput::Event::LongBack:
      // Leaves a nested lookup altogether, every panel of the chain at once.
      if (depth > 0) setResult(ActivityResult{LookupChainCloseResult{}});
      finish();
      return;
    case PanelInput::Event::Back:
      finish();
      return;
    case PanelInput::Event::CloseAll:
    case PanelInput::Event::SideToggle: {
      // Back to the book: the word selection that opened this view closes too.
      finishCancelled();
      return;
    }
    case PanelInput::Event::LongPress:
      if (nested.lookup && showingEntry()) lookUpAt(input.x, input.y);
      return;
    case PanelInput::Event::StartCursor:
      if (!nested.lookup) {
        if (miningAvailable()) saveSentence();  // no cursor here: a held Confirm is a Confirm
      } else if (startWordCursor()) {
        requestUpdate();
      }
      return;
    case PanelInput::Event::Confirm:
      // Where there is nothing to save, Confirm starts the cursor instead.
      if (miningAvailable()) {
        saveSentence();
      } else if (nested.lookup && startWordCursor()) {
        requestUpdate();
      }
      return;
    case PanelInput::Event::ReadOn:
      stepPage(input.direction);
      return;
    case PanelInput::Event::Skip:
      skipEntry(input.direction);
      return;
  }
}

// A long press on the definition: look up the word under it.
void DictionaryDefinitionActivity::lookUpAt(const int x, const int y) {
  std::string word;
  {
    RenderLock lock;  // render() writes drawnText and shares the font cache these measure with
    if (entries[currentEntry].definitionText) {
      word = japanese_nested::wordAt(renderer, nestedHost.drawnText, definition, x, y);
    } else {
      std::vector<PanelWord> words;
      collectPageWords(words);
      for (auto& candidate : words) {
        if (x >= candidate.x && x < candidate.x + candidate.width && y >= candidate.y &&
            y < candidate.y + candidate.height) {
          word = std::move(candidate.text);
          break;
        }
      }
    }
  }
  if (word.empty()) return;
  haptic_feedback::touchAction();
  lookUpNested(word);
}

// Draws the current page: a styled Page when the HTML layout succeeded,
// otherwise the wrapped line spans (copied into a stack buffer for NUL
// termination). Called twice per render: once in font-cache scan mode, once
// for the real paint.
void DictionaryDefinitionActivity::drawBody(const int fontId, const int x, const int startY) {
  if (!showingEntry()) return;
  if (entries[currentEntry].definitionText) {
    drawDefinitionText(x, startY);
    return;
  }
  if (!pages.empty()) {
    pages[currentPage]->render(renderer, fontId, x, startY);
    return;
  }
  textPages.draw(renderer, fontId, x, startY, definition, currentPage);
}

void DictionaryDefinitionActivity::render(RenderLock&&) {
  // No clearScreen: the panel floats over the page the word-select activity left in the
  // framebuffer, and its own fill is opaque, so a re-render overwrites the previous one.
  char counter[16] = "";
  if (showingEntry() && totalPages > 1) {
    snprintf(counter, sizeof(counter), "%d/%d", currentPage + 1, totalPages);
  }
  const char* headword = showingEntry() ? entries[currentEntry].headword.c_str() : "";
  const std::string label = footerLabel();
  clipHandles.draw(renderer);  // under the card, beside the word on the page
  const auto layout = DictionaryPanel::draw(renderer, headword, label.c_str(), counter, nullptr,
                                            miningAvailable() && mappedInput.hasTouch(), miningFeedback_.saved());
  if (!showingEntry()) {
    // Loading while words are still being looked up; nothing found once they all have been.
    UITheme::drawCenteredText(renderer, layout.body, UI_12_FONT_ID, layout.body.y + layout.body.height / 2,
                              more.next ? tr(STR_LOADING) : I18N.get(emptyMessage), true);
  }

  // Body: two-pass draw inside a prewarm scope (same pattern as the reader's
  // renderContents) so SD-card font glyphs load from SD in one batch instead
  // of one on-demand overflow read per character on every page turn.
  const int fontId = definitionFontId();
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  drawBody(fontId, layout.body.x, layout.body.y);  // scan pass: records codepoints only
  scope.endScanAndPrewarm();
  drawBody(fontId, layout.body.x, layout.body.y);
  nestedHost.drawCursor(renderer);
  DictionaryPanel::drawPopup(renderer, layout, nestedHost.popupText(miningFeedback_.popupText(millis())));

  const bool hasPrev = onLoadingPage ? !entries.empty() : currentPage > 0 || currentEntry > 0;
  const bool hasNext =
      !onLoadingPage && (currentPage + 1 < totalPages || currentEntry + 1 < entries.size() || more.next);
  if (nestedHost.cursorActive()) {
    const auto cursorLabels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_LOOKUP), "<", ">");
    DictionaryPanel::clearButtonHints(renderer);
    GUI.drawButtonHints(renderer, cursorLabels.btn1, cursorLabels.btn2, cursorLabels.btn3, cursorLabels.btn4);
    renderer.displayBuffer();
    return;
  }
  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK),
                            miningAvailable() ? (miningFeedback_.saved() ? tr(STR_MINING_SAVED) : tr(STR_MINING_SAVE))
                            : nested.lookup && showingEntry() ? tr(STR_SELECT)
                                                              : "",
                            hasPrev ? "<" : "", hasNext ? ">" : "");
  DictionaryPanel::clearButtonHints(renderer);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

// The words of the page on screen, in reading order, where drawBody puts them.
void DictionaryDefinitionActivity::collectPageWords(std::vector<PanelWord>& out) const {
  out.clear();
  if (!showingEntry() || entries[currentEntry].definitionText) return;
  const BodyArea body = bodyArea();
  const int fontId = definitionFontId();
  if (!pages.empty()) {
    out.reserve(64);
    for (const auto& element : pages[currentPage]->elements) {
      if (element->getTag() != TAG_PageLine) continue;
      const auto* line = static_cast<const PageLine*>(element.get());
      const auto* block = line->getBlock();
      if (!block || !block->valid()) continue;
      const int lineFontId = block->getBlockStyle().resolveFontId(fontId);
      for (uint16_t i = 0; i < block->wordCount(); i++) {
        const char* text = block->wordText(i);
        std::string word = trimToken(text, std::strlen(text));
        if (word.empty()) continue;
        const int32_t wordFont = block->wordFont(i);
        const int font = wordFont != 0 ? static_cast<int>(wordFont) : lineFontId;
        out.push_back({static_cast<int16_t>(body.x + line->xPos + block->wordXpos(i)),
                       static_cast<int16_t>(body.y + line->yPos),
                       static_cast<int16_t>(renderer.getTextWidth(font, text, block->wordStyle(i))),
                       static_cast<int16_t>(renderer.getLineHeight(font)), std::move(word)});
      }
    }
    return;
  }
  std::vector<PanelTextPages::Word> spans;
  spans.reserve(64);
  textPages.collectWords(renderer, fontId, body.x, body.y, definition, currentPage, spans);
  out.reserve(spans.size());
  for (const auto& span : spans) {
    std::string word = trimToken(definition.c_str() + span.start, span.len);
    if (word.empty()) continue;
    out.push_back({span.x, span.y, span.width, span.height, std::move(word)});
  }
}

bool DictionaryDefinitionActivity::startWordCursor() {
  if (!showingEntry()) return false;
  if (entries[currentEntry].definitionText) return nestedHost.startCjkCursor(renderer, definition);
  std::vector<PanelWord> words;
  {
    RenderLock lock;  // measures through the font cache render shares
    collectPageWords(words);
  }
  std::vector<DefinitionText::CharBox> boxes;
  boxes.reserve(words.size());
  cursorWords.clear();
  cursorWords.reserve(words.size());
  for (auto& word : words) {
    boxes.push_back({word.x, word.y, word.width, word.height, static_cast<uint32_t>(cursorWords.size())});
    cursorWords.push_back(std::move(word.text));
  }
  return nestedHost.startCursor(std::move(boxes));
}

std::unique_ptr<DictionaryDefinitionActivity> DictionaryDefinitionActivity::openNested(
    GfxRenderer& renderer, MappedInputManager& mappedInput, const NestedLookup& source, const char* word,
    const uint8_t chainDepth, StrId& failure) {
  if (chainDepth > MAX_NESTED_LEVELS) {
    failure = StrId::STR_LOOKUP_CHAIN_FULL;
    return nullptr;
  }
  if (ESP.getMaxAllocHeap() < NESTED_LOOKUP_MIN_HEAP) {
    LOG_ERR("DICT", "Nested lookup: maxAlloc %u below %u", ESP.getMaxAllocHeap(), NESTED_LOOKUP_MIN_HEAP);
    failure = StrId::STR_LOW_MEMORY_RETRY;
    return nullptr;
  }
  std::vector<Entry> found;
  failure = StrId::STR_DICT_NOT_FOUND;
  if (!source.lookup(source.ctx, word, found, failure) || found.empty()) return nullptr;
  auto panel = makeUniqueNoThrow<DictionaryDefinitionActivity>(renderer, mappedInput, std::move(found));
  if (!panel) {
    LOG_ERR("DICT", "OOM: nested definition view");
    failure = StrId::STR_LOW_MEMORY_RETRY;
    return nullptr;
  }
  panel->setNestedLookup(source, chainDepth);
  return panel;
}

// Opens `word`'s entries in a panel over this one. Back there returns here, on this page.
void DictionaryDefinitionActivity::lookUpNested(const std::string& word) {
  StrId failure = StrId::STR_DICT_NOT_FOUND;
  auto child = openNested(renderer, mappedInput, nested, word.c_str(), static_cast<uint8_t>(depth + 1), failure);
  if (!child) {
    nestedHost.showNotice(failure);
    requestUpdate();
    return;
  }
  child->setPageTurnReversed(pageTurnReversed);
  startActivityForResult(std::move(child), [this](const ActivityResult& result) {
    // The power click and a long Back close the whole chain; a plain Back stops here.
    if (auto chain = NestedLookupHost::chainResult(result, depth > 0)) {
      setResult(std::move(*chain));
      finish();
      return;
    }
    ensureGlyphFallback();  // the closed panel released the shared fallback font on exit
    requestUpdate();
  });
}

// One page of a DefinitionText entry, as the word-lookup panels draw it: the reading and grammar
// header, then the senses, scrolled by whole pages.
void DictionaryDefinitionActivity::drawDefinitionText(const int x, const int startY) {
  const Entry& entry = entries[currentEntry];
  const BodyArea body = bodyArea();
  const Rect area{x, startY, body.width, body.height};
  const int fontId = DefinitionText::wordLookupFontId();
  const uint16_t scale = DefinitionText::wordLookupFontScale();
  sdFontSystem.ensureWordLookupFallback(renderer, fontId, DefinitionText::wordLookupFontPointSize());
  const int lineHeight = std::max(1, renderer.getLineHeightScaled(fontId, scale));
  const DefinitionText::EntryMetadata metadata{entry.reading, entry.grammar, std::string()};
  const int scroll = currentPage * definitionTextLines;
  const int metadataLines = DefinitionText::entryMetadataLineCount(renderer, area, fontId, scale, metadata);
  const int metadataEndY =
      DefinitionText::drawEntryMetadata(renderer, area, fontId, scale, metadata, scroll, lineHeight);
  DefinitionText::WrapGeometry geometry;
  geometry.valid = true;
  geometry.fontId = fontId;
  geometry.textX = x;
  geometry.startY = metadataEndY - std::min(scroll, metadataLines) * lineHeight;
  geometry.lineHeight = lineHeight;
  geometry.maxWidth = body.width;
  geometry.maxY = startY + body.height;
  geometry.scrollOffset = std::max(0, scroll - metadataLines);
  geometry.scale = scale;
  DefinitionText::drawWrapped(renderer, fontId, definition, geometry.textX, geometry.startY, lineHeight,
                              geometry.maxWidth, geometry.maxY, geometry.scrollOffset, scale);
  nestedHost.drawnText = geometry;
}
