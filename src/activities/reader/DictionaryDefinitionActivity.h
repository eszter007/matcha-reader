#pragma once

#include <Epub/Page.h>
#include <I18n.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "DefinitionTextRenderer.h"
#include "NestedLookupHost.h"
#include "activities/Activity.h"
#include "clippings/LookupClipHandles.h"
#include "components/DictionaryPanel.h"
#include "components/PanelTextPages.h"
#include "util/SentenceMining.h"
#include "vocabulary/VocabularyStore.h"

// Paged viewer for the definitions of one word, one entry per dictionary that has it, read as a
// single flow: paging past an entry's last page opens the next. HTML definitions are laid out
// through the EPUB chapter parser into styled Pages; anything else (plain text, or HTML too
// damaged to parse) is word-wrapped on entry and each page renders spans of the original string,
// so no per-line copies are held. Only the entry on screen is laid out.
class DictionaryDefinitionActivity final : public Activity {
 public:
  struct Entry {
    std::string headword;
    std::string definition;
    bool html = false;
    std::string dictName;  // footer title; empty when the dictionary has none
    // Enables sentence mining for this entry: Select (and the + button on touch boards) saves it
    // with this entry as its definition. Invalid = no saving.
    sentencemining::Draft draft;
    uint8_t dictSlot = 0;  // which of the lookup's dictionaries gave it: two can share a title
    // A Japanese/Chinese dictionary entry, already in DefinitionText's layout (formatEntryBody),
    // with its header fields apart. Drawn as the word-lookup panels draw theirs.
    bool definitionText = false;
    std::string reading;
    std::string grammar;
    StrId kind = StrId::STR_NONE_OPT;  // tag beside the headword (Vocab, Grammar, Name); none by default
  };

  // Steps to the neighbouring word of the page when reading on or skipping runs past the first or
  // last entry, for a lookup that moves through the page's words (manga, a book page without word
  // geometry). step() puts that word's entries in `out`; false at the page's ends.
  struct WordSteps {
    void* ctx = nullptr;
    bool (*step)(void* ctx, int direction, std::vector<Entry>& out) = nullptr;
  };

  // Redraws what lies behind the panel (the page and its word highlight) at the start of each render,
  // for a lookup whose word changes while the panel is up.
  struct Backdrop {
    void* ctx = nullptr;
    void (*draw)(void* ctx) = nullptr;
  };

  // Work of the screen under the panel that must go on while the panel is up (a page still being
  // mapped). Called on idle frames; returns true while there is more to do.
  struct HostTick {
    void* ctx = nullptr;
    bool (*tick)(void* ctx) = nullptr;
  };

  // Supplies further entries while the view is up (a touch selection's other words). next() puts
  // one more word's entries in `fresh`, reading `shown` to skip repeats, and returns false once
  // nothing is left.
  struct MoreEntries {
    void* ctx = nullptr;
    bool (*next)(void* ctx, const std::vector<Entry>& shown, std::vector<Entry>& fresh) = nullptr;
    // What to say when every word has been looked up and nothing was found.
    StrId (*failure)(void* ctx) = nullptr;
  };

  // Looks up a word chosen inside a definition, in the dictionaries behind the panel. Fills `out`
  // and returns true, or returns false with the reason in `failure`.
  struct NestedLookup {
    void* ctx = nullptr;
    bool (*lookup)(void* ctx, const char* word, std::vector<Entry>& out, StrId& failure) = nullptr;
  };

  explicit DictionaryDefinitionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        std::vector<Entry> entries);

  // Enables lookups inside the definition: a long press on a word, or holding Confirm for a word
  // cursor. `chainDepth` is how many panels of the chain are under this one.
  void setNestedLookup(const NestedLookup& source, const uint8_t chainDepth = 0) {
    nested = source;
    depth = chainDepth;
  }

  // The first panel of a lookup chain started from another screen: `word`'s entries, or null with
  // the reason in `failure` (no match, low memory). The caller opens it with startActivityForResult.
  static std::unique_ptr<DictionaryDefinitionActivity> openNested(GfxRenderer& renderer,
                                                                  MappedInputManager& mappedInput,
                                                                  const NestedLookup& source, const char* word,
                                                                  uint8_t chainDepth, StrId& failure);
  // Handles on the looked-up word behind the panel; a drag on one finishes with ClipStartResult.
  void setClipHandles(const LookupClipHandles& handles) { clipHandles = handles; }
  void setMoreEntries(const MoreEntries& source) { more = source; }
  // step() runs under RenderLock, which the same task may take again inside it.
  void setWordSteps(const WordSteps& source) { steps = source; }
  // draw() runs on the render task.
  void setBackdrop(const Backdrop& source) { backdrop = source; }
  void setHostTick(const HostTick& source) {
    hostTick = source;
    hostBusy = source.tick != nullptr;
  }
  // The entry the panel opens on (the grammar entry when that is the better match).
  void setStartEntry(const size_t index) { startEntry = index; }
  // Records each looked-up word to vocabulary history (/Vocabulary.txt), with where it was read.
  // Set by lookup screens only; a nested panel inherits it.
  void setVocabulary(VocabularyContext context) {
    vocabulary = std::move(context);
    recordsVocabulary = true;
  }
  bool skipLoopDelay() override { return more.next != nullptr || hostBusy; }

  // The book behind the panel reverses its page turns (Reversed Page Turn in a vertical book).
  void setPageTurnReversed(const bool reversed) { pageTurnReversed = reversed; }
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  bool pageTurnReversed = false;
  VocabularyContext vocabulary;
  bool recordsVocabulary = false;
  std::string nestedWord;  // the word a nested panel was opened for
  void recordVocabulary(const Entry& entry) const;
  NestedLookup nested;
  uint8_t depth = 0;
  // A word of the page on screen, where it is drawn.
  struct PanelWord {
    int16_t x;
    int16_t y;
    int16_t width;
    int16_t height;
    std::string text;
  };
  NestedLookupHost nestedHost;
  // StarDict cursor: the text of each word box, by tag (DefinitionText entries tag byte offsets).
  std::vector<std::string> cursorWords;
  void collectPageWords(std::vector<PanelWord>& out) const;
  bool startWordCursor();
  void lookUpNested(const std::string& word);
  void lookUpAt(int x, int y);
  LookupClipHandles clipHandles;
  MoreEntries more;
  WordSteps steps;
  Backdrop backdrop;
  HostTick hostTick;
  bool hostBusy = false;
  size_t startEntry = 0;
  // The neighbouring word's entries, when steps is set: the first page going forward, the last
  // entry's last page (atEnd) or first page going back. False at the page's ends.
  bool stepWord(int direction, bool atEnd);
  // Past the last entry while more are coming: a Loading page the reader can step onto, which the
  // next entry takes over. Also the only page while nothing has been found yet.
  bool onLoadingPage = false;
  StrId emptyMessage = StrId::STR_NO_MATCH;
  void fetchMoreEntries();
  static void prepareDraft(Entry& entry);
  DictionaryPanel::MiningFeedback miningFeedback_;
  void saveSentence();
  bool showingEntry() const { return !onLoadingPage && currentEntry < entries.size(); }
  bool miningAvailable() const { return showingEntry() && entries[currentEntry].draft.valid(); }

  // Lays out entries[index] and shows its first page, or its last when stepping back into it.
  void showEntry(size_t index, bool atLastPage);
  // One page forward (+1) or back (-1) through the whole flow. False at either end.
  bool stepPage(int direction);
  // Straight to the neighbouring entry's first page, whatever page of this one is showing.
  bool skipEntry(int direction);

  // Usable body-text area: the panel's inner rectangle.
  struct BodyArea {
    int x;
    int y;
    int width;
    int height;
  };

  // The font the definition is laid out AND drawn with: the Word Lookup Font Size setting, not
  // the reader's body font. A dictionary entry at reading size fills the panel in three lines,
  // and the Japanese panels have always used this setting -- the English one now matches.
  static int definitionFontId();
  // Point size matching definitionFontId(), for the SD-card glyph fallback.
  static uint8_t definitionPointSize();
  // Let the selected SD reader family fill glyphs the built-in serif lacks -- IPA in a
  // pronunciation, above all. The Japanese panels have always done this; without it a reader
  // who installs an IPA-bearing font still sees nothing for those codepoints.
  void ensureGlyphFallback() const;

  BodyArea bodyArea() const;
  bool layoutHtmlPages();
  void drawBody(int fontId, int x, int startY);

  std::vector<Entry> entries;
  size_t currentEntry = 0;
  // The entry on screen, laid out: a copy of its definition, with embedded NULs (StarDict
  // multi-type separators) normalized to newlines so C-string APIs see the whole text.
  std::string definition;
  // Footer title: the dictionary name, plus the entry's place when there are several.
  std::string footerLabel() const;
  // Styled path: reader-identical Pages laid out from the HTML definition.
  // Empty means the plain-text span path below is active.
  std::vector<std::unique_ptr<Page>> pages;
  PanelTextPages textPages;  // plain-text path: wrapped spans of `definition`
  // DefinitionText path (Entry::definitionText): lines per page.
  int definitionTextLines = 1;
  void drawDefinitionText(int x, int startY);
  int currentPage = 0;
  int totalPages = 1;
};
