#pragma once

#include <string>

#include "activities/Activity.h"
#include "flashcards/FlashcardStore.h"

// One review session over a deck, drawn as a ruled index card: the word runs along a spine on the
// left (upright down the spine for Chinese and Japanese, turned for other scripts), the sentence
// it was saved from on the right. Turning the card shows the meaning and the four FSRS ratings.
class FlashcardReviewActivity final : public Activity {
 public:
  FlashcardReviewActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string language);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct Rect4 {
    int x, y, w, h;
    bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
  };

  void loadCard();
  void rate(int index);
  void drawGrayPass();
  void drawFrontVertical(const Rect4& body, int font, const std::string& sentence) const;
  void drawCard(const Rect4& card) const;
  void drawSpine(const Rect4& spine) const;
  void drawFront(const Rect4& body) const;
  void drawBack(const Rect4& body) const;
  void drawRatings(const Rect4& band) const;
  void drawDone(const Rect4& area) const;
  Rect4 cardRect() const;
  Rect4 ratingsRect() const;

  std::string language;
  flashcards::Session session;
  flashcards::CardText card;
  bool cardOk = false;
  bool cardIsNew = false;
  bool opened = false;
  bool showBack = false;
  int selected = 2;  // Good
  uint16_t intervals[4] = {0, 0, 0, 0};
};
