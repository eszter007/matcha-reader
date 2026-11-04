#include "DictionaryDefinitionActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>

#include "BookStats.h"
#include "CrossPointSettings.h"
#include "HapticFeedback.h"
#include "PanelTouch.h"
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
  DictionaryPanel::setAvoid(clipHandles.band(renderer));  // before the entries are laid out
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
  DictionaryPanel::setAvoid(Rect{});
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
  // Back steps up to the word selection. The power click leaves the dictionary outright: from
  // the selection it opened this view, so from here it closes the whole flow -- two clicks in
  // and back out, without the reading hand moving.
  if (ReaderUtils::wordLookupPowerClick(mappedInput)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (ClipStartResult clip; clipHandles.pressed(renderer, mappedInput, clip)) {
    ActivityResult result;
    result.data = clip;
    setResult(std::move(result));
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    saveSentence();
    return;
  }
  // A side button bound to Word Lookup closes the dictionary, as the power click does.
  if (ReaderUtils::wordLookupSideToggle(mappedInput)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }
  const auto panelStep = ReaderUtils::lookupPanelStep(mappedInput);
  if (panelStep != ReaderUtils::PanelStep::None) {
    skipEntry(panelStep == ReaderUtils::PanelStep::Next ? 1 : -1);
    return;
  }

  switch (const auto touch = PanelTouch::read(renderer, mappedInput, miningAvailable())) {
    case PanelTouch::Action::Close: {
      // A tap outside the card means "back to the book", as in the Japanese panel.
      ActivityResult result;
      result.isCancelled = true;
      setResult(std::move(result));
      finish();
      return;
    }
    case PanelTouch::Action::AddButton:
      saveSentence();
      return;
    case PanelTouch::Action::ScrollDown:
    case PanelTouch::Action::ScrollUp:
      if (stepPage(touch == PanelTouch::Action::ScrollDown ? 1 : -1)) haptic_feedback::touchAction();
      return;
    case PanelTouch::Action::Next:
    case PanelTouch::Action::Previous:
      if (skipEntry(touch == PanelTouch::Action::Next ? 1 : -1)) haptic_feedback::touchAction();
      return;
    case PanelTouch::Action::None:
      break;
  }

  // Every pair reads the same way: a press reads on through the entry and onto the next word at
  // its end, a hold skips to the next word.
  int delta = 0;
  switch (ReaderUtils::lookupPressAny(mappedInput, delta)) {
    case ReaderUtils::LookupPress::Press:
      stepPage(delta);
      break;
    case ReaderUtils::LookupPress::Hold:
      skipEntry(delta);
      break;
    case ReaderUtils::LookupPress::None:
      break;
  }
}

// Draws the current page: a styled Page when the HTML layout succeeded,
// otherwise the wrapped line spans (copied into a stack buffer for NUL
// termination). Called twice per render: once in font-cache scan mode, once
// for the real paint.
void DictionaryDefinitionActivity::drawBody(const int fontId, const int x, const int startY) const {
  if (!showingEntry()) return;
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
  DictionaryPanel::drawPopup(renderer, layout, miningFeedback_.popupText(millis()));

  const bool hasPrev = onLoadingPage ? !entries.empty() : currentPage > 0 || currentEntry > 0;
  const bool hasNext =
      !onLoadingPage && (currentPage + 1 < totalPages || currentEntry + 1 < entries.size() || more.next);
  const auto labels = mappedInput.mapLabels(
      tr(STR_BACK), miningAvailable() ? (miningFeedback_.saved() ? tr(STR_MINING_SAVED) : tr(STR_MINING_SAVE)) : "",
      hasPrev ? "<" : "", hasNext ? ">" : "");
  DictionaryPanel::clearButtonHints(renderer);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
