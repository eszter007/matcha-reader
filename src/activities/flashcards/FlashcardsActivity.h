#pragma once

#include <string>
#include <vector>

#include "activities/UiListActivity.h"
#include "flashcards/FlashcardStore.h"

// The Flashcards tab: one row per deck (a language's sentence-mining file) with what is due
// today. Opening a deck starts a review session (FlashcardReviewActivity).
class FlashcardsActivity final : public UiListActivity {
 public:
  FlashcardsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;

  // The deck's name for a language code, e.g. "Japanese"; the code itself when it has none.
  static std::string languageName(const std::string& code);
  // The deck's contents-page name: the language's own name for itself.
  static std::string deckLabel(const std::string& code);

 private:
  std::vector<flashcards::DeckSummary> decks;
  std::vector<freeink::ui::ListItem> rowItems_;
  std::vector<std::string> labels_;
  std::vector<std::string> values_;
  int heroTop_ = 0;
  int topRow_ = 0;
  int selectedRow_ = -1;
  int dueTotal_ = 0;
  int rowHeight_ = 0;
  bool syncRow_ = false;  // the last row is "Sync with Anki"

  void reload();
  // Height of the numeral, caption and ornament above the rows.
  int heroHeight() const;
  int listCount() const override { return static_cast<int>(decks.size()) + (syncRow_ ? 1 : 0); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onBackButton() override;
  const char* headerTitle() const override;
  void drawFooter() override;
  const char* confirmHint() const override;
  HomeTab tabBarTab() const override { return HomeTab::Flashcards; }
};
