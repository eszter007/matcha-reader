#include "DictionaryDefinitionActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>

#include "BookStats.h"
#include "CrossPointSettings.h"
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
  // The card keeps the definition as it came from the dictionary; the layout frees its copy.
  for (auto& entry : this->entries) {
    if (!entry.draft.valid()) continue;
    entry.draft.card.definition =
        entry.html ? sentencemining::capHtml(entry.definition) : sentencemining::definitionHtml(entry.definition);
  }
}

void DictionaryDefinitionActivity::onEnter() {
  ensureGlyphFallback();
  Activity::onEnter();
  if (entries.empty()) {
    finish();
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
  dictLabel = entry.dictName;
  if (entries.size() > 1) {
    char place[16];
    snprintf(place, sizeof(place), " (%u/%u)", static_cast<unsigned>(index + 1), static_cast<unsigned>(entries.size()));
    dictLabel += place;
  }
  currentPage = atLastPage ? totalPages - 1 : 0;
  miningStatus_ = MiningStatus::None;
}

bool DictionaryDefinitionActivity::stepPage(const int direction) {
  if (direction > 0) {
    if (currentPage + 1 < totalPages) {
      currentPage++;
    } else if (currentEntry + 1 < entries.size()) {
      RenderLock lock;  // the render task draws from the pages this replaces
      showEntry(currentEntry + 1, false);
    } else {
      return false;
    }
  } else {
    if (currentPage > 0) {
      currentPage--;
    } else if (currentEntry > 0) {
      RenderLock lock;
      showEntry(currentEntry - 1, true);
    } else {
      return false;
    }
  }
  miningStatus_ = MiningStatus::None;
  requestUpdate();
  return true;
}

void DictionaryDefinitionActivity::onExit() {
  Activity::onExit();
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->releaseAllFontMemory();
  }
}

DictionaryDefinitionActivity::BodyArea DictionaryDefinitionActivity::bodyArea() const {
  const auto body = DictionaryPanel::compute(renderer).body;
  return {body.width, body.height};
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
  auto& draft = entries[currentEntry].draft;
  if (!draft.valid()) return;
  draft.card.date = sentencemining::today();  // the day of the save, not of the lookup
  miningStatus_ = sentencemining::append(draft.card, draft.language) ? MiningStatus::Saved : MiningStatus::Failed;
  if (miningStatus_ == MiningStatus::Saved) BookStats::addCounts(draft.bookPath.c_str(), 0, 1);
  requestUpdate();
}

void DictionaryDefinitionActivity::loop() {
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
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    saveSentence();
    return;
  }

  switch (PanelTouch::read(renderer, mappedInput, miningDraft().valid())) {
    case PanelTouch::Action::Close:
      finish();
      return;
    case PanelTouch::Action::AddButton:
      saveSentence();
      return;
    case PanelTouch::Action::Next:
    case PanelTouch::Action::ScrollDown:
      stepPage(1);
      return;
    case PanelTouch::Action::Previous:
    case PanelTouch::Action::ScrollUp:
      stepPage(-1);
      return;
    case PanelTouch::Action::None:
      break;
  }

  buttonNavigator.onNext([this] { stepPage(1); });
  buttonNavigator.onPrevious([this] { stepPage(-1); });
}

// Draws the current page: a styled Page when the HTML layout succeeded,
// otherwise the wrapped line spans (copied into a stack buffer for NUL
// termination). Called twice per render: once in font-cache scan mode, once
// for the real paint.
void DictionaryDefinitionActivity::drawBody(const int fontId, const int x, const int startY) const {
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
  if (totalPages > 1) {
    snprintf(counter, sizeof(counter), "%d/%d", currentPage + 1, totalPages);
  }
  const auto& entry = entries[currentEntry];
  const bool showStatus = miningStatus_ != MiningStatus::None;
  const char* statusText = miningStatus_ == MiningStatus::Saved ? tr(STR_MINING_SAVED) : tr(STR_MINING_SAVE_FAILED);
  const auto layout =
      DictionaryPanel::draw(renderer, entry.headword.c_str(), showStatus ? nullptr : dictLabel.c_str(), counter,
                            showStatus ? statusText : nullptr, miningDraft().valid() && mappedInput.hasTouch());

  // Body: two-pass draw inside a prewarm scope (same pattern as the reader's
  // renderContents) so SD-card font glyphs load from SD in one batch instead
  // of one on-demand overflow read per character on every page turn.
  const int fontId = definitionFontId();
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  drawBody(fontId, layout.body.x, layout.body.y);  // scan pass: records codepoints only
  scope.endScanAndPrewarm();
  drawBody(fontId, layout.body.x, layout.body.y);

  const bool hasPrev = currentPage > 0 || currentEntry > 0;
  const bool hasNext = currentPage + 1 < totalPages || currentEntry + 1 < entries.size();
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), miningDraft().valid() ? tr(STR_MINING_SAVE) : "",
                                            hasPrev ? "<" : "", hasNext ? ">" : "");
  DictionaryPanel::clearButtonHints(renderer);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
