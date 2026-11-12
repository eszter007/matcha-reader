#include "FlashcardsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>

#include "AnkiSyncActivity.h"
#include "AnkiSyncStore.h"
#include "FlashcardReviewActivity.h"
#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "components/HomeTabBar.h"
#include "components/StatsWidgets.h"
#include "components/UITheme.h"
#include "components/icons/stats_icons.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {
constexpr uint16_t HERO_SCALE = 640;
constexpr int NUMERAL_GAP = 14;     // numeral to caption
constexpr int ORNAMENT_ABOVE = 30;  // caption to ornament
constexpr int ORNAMENT_BELOW = 26;  // ornament to the first row box; the row's own padding adds the rest
constexpr int ROW_PITCH = 54;       // contents rows, still a comfortable tap height
constexpr int ROW_INSET = 16;       // rows sit inside the header margin
constexpr int SEL_PAD = 8;          // as the theme pads a selected list row
constexpr int SEL_RADIUS = 6;
constexpr int TOP_GAP_MAX = 24;  // largest gap above the numeral box (its glyphs sit low in it)

// Each deck is named in its own language, like a contents page. Korean and Cantonese are left
// out: the UI font has no Hangul and no 粵, so they fall back to the UI-language name.
struct Autonym {
  const char* code;
  const char* name;
};
constexpr Autonym AUTONYMS[] = {
    {"ja", "日本語"},  {"zh", "中文"},     {"en", "English"},    {"de", "Deutsch"},    {"fr", "Français"},
    {"es", "Español"}, {"it", "Italiano"}, {"pt", "Português"},  {"nl", "Nederlands"}, {"hu", "Magyar"},
    {"pl", "Polski"},  {"ru", "Русский"},  {"uk", "Українська"},
};

struct LanguageLabel {
  const char* code;
  StrId name;
};
constexpr LanguageLabel LANGUAGES[] = {
    {"ja", StrId::STR_XLAT_LANG_JA}, {"zh", StrId::STR_XLAT_LANG_ZH}, {"yue", StrId::STR_BOOK_LANG_YUE},
    {"ko", StrId::STR_XLAT_LANG_KO}, {"en", StrId::STR_XLAT_LANG_EN}, {"de", StrId::STR_XLAT_LANG_DE},
    {"fr", StrId::STR_XLAT_LANG_FR}, {"es", StrId::STR_XLAT_LANG_ES}, {"it", StrId::STR_XLAT_LANG_IT},
    {"pt", StrId::STR_XLAT_LANG_PT}, {"nl", StrId::STR_XLAT_LANG_NL}, {"hu", StrId::STR_XLAT_LANG_HU},
    {"pl", StrId::STR_XLAT_LANG_PL}, {"ru", StrId::STR_XLAT_LANG_RU}, {"uk", StrId::STR_XLAT_LANG_UK},
};
}  // namespace

FlashcardsActivity::FlashcardsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("Flashcards", renderer, mappedInput) {}

std::string FlashcardsActivity::deckLabel(const std::string& code) {
  for (const auto& a : AUTONYMS) {
    if (code == a.code) return a.name;
  }
  return languageName(code);
}

std::string FlashcardsActivity::languageName(const std::string& code) {
  for (const auto& l : LANGUAGES) {
    if (code == l.code) return I18N.get(l.name);
  }
  std::string upper = code;
  for (char& c : upper) c = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c;
  return upper;
}

void FlashcardsActivity::onEnter() {
  UiListActivity::onEnter();
  reload();
}

void FlashcardsActivity::reload() {
  flashcards::listDecks(decks);
  // Contents order: by the name shown, not the file's language code.
  // An Anki deck goes by its own name; a subdeck by its last two levels ("HSK 1 › Meaning").
  const auto nameOf = [](const flashcards::DeckSummary& d) {
    if (d.label.empty()) return deckLabel(d.language);
    const size_t last = d.label.rfind("::");
    if (last == std::string::npos) return d.label;
    const size_t before = last == 0 ? std::string::npos : d.label.rfind("::", last - 1);
    const size_t from = before == std::string::npos ? 0 : before + 2;
    return d.label.substr(from, last - from) + " \xE2\x80\xBA " + d.label.substr(last + 2);
  };
  std::sort(decks.begin(), decks.end(), [&nameOf](const flashcards::DeckSummary& a, const flashcards::DeckSummary& b) {
    return nameOf(a) < nameOf(b);
  });
  labels_.clear();
  dueTotal_ = 0;
  values_.clear();
  rowItems_.clear();
  values_.reserve(decks.size() + 1);
  labels_.reserve(decks.size() + 1);
  rowItems_.reserve(decks.size() + 1);
  for (size_t i = 0; i < decks.size(); i++) {
    const auto& d = decks[i];
    labels_.push_back(nameOf(d));
    values_.push_back(std::to_string(d.due + d.fresh));
    dueTotal_ += d.due + d.fresh;
  }
  // With Anki set up, the last row syncs.
  syncRow_ = ANKI_STORE.isConfigured();
  if (syncRow_) {
    labels_.emplace_back(tr(STR_ANKI_SYNC_NOW));
    values_.emplace_back();
  }
  // Pointers into labels_/values_ only once both are complete: no reallocation after this.
  for (size_t i = 0; i < labels_.size(); i++) {
    fui::ListItem item;
    // Rows are drawn by drawFooter() with dot leaders; the list only lays out the tap targets.
    item.label = "";
    item.actionValue = static_cast<int16_t>(i);
    rowItems_.push_back(item);
  }
  requestUpdate();
}

const char* FlashcardsActivity::headerTitle() const { return tr(STR_FLASHCARDS); }

int FlashcardsActivity::heroHeight() const {
  const int numH = renderer.getLineHeightScaled(NOTOSERIF_18_FONT_ID, HERO_SCALE);
  // With nothing due there is no numeral, only the caption.
  return (dueTotal_ > 0 ? numH * 3 / 4 + NUMERAL_GAP : 0) + renderer.getLineHeight(UI_12_FONT_ID) + ORNAMENT_ABOVE +
         ORNAMENT_BELOW;
}

void FlashcardsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(HomeTabBar::bottomInset()), 0});
  if (rowItems_.empty()) return;
  rowHeight_ = ROW_PITCH;
  // Space above the title block is capped so it stays attached to the header.
  const int top = metrics.topPadding + metrics.headerHeight;
  const int avail = renderer.getScreenHeight() - HomeTabBar::bottomInset() - top;
  const int block = heroHeight() + static_cast<int>(rowItems_.size()) * rowHeight_;
  heroTop_ = top + std::max(static_cast<int>(metrics.verticalSpacing), std::min(TOP_GAP_MAX, (avail - block) * 2 / 5));
  screen.spacer(static_cast<int16_t>(heroTop_ - top + heroHeight()));
  fui::ListProps props;
  props.items = rowItems_.data();
  props.count = static_cast<uint16_t>(rowItems_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  // Fixed pitch so drawFooter() knows where each row is.
  props.rowHeight = static_cast<int16_t>(rowHeight_);
  props.rowGap = 0;
  // Transparent rows, so the text drawFooter() paints stays readable. The list still owns the
  // selection (so it keeps the selected row in view); drawFooter() draws its highlight.
  props.rowStyles.explicitlySet = true;
  syncListViewport(screen, props);
  topRow_ = activeNav().top;
  selectedRow_ = props.selectedIndex;
  screen.list(props);
}

void FlashcardsActivity::drawFooter() {
  if (!labels_.empty()) {
    const int due = dueTotal_;
    const int w = renderer.getScreenWidth();
    // The title block is centred; the rows sit just inside the header's margins.
    const int pad = UITheme::getInstance().getMetrics().contentSidePadding;
    const int left = pad + ROW_INSET;
    const int right = w - pad - ROW_INSET;
    const int numH = renderer.getLineHeightScaled(NOTOSERIF_18_FONT_ID, HERO_SCALE);
    const char* caption = tr(STR_FLASH_TO_REVIEW);
    int y = heroTop_;
    if (due > 0) {
      char n[12];
      snprintf(n, sizeof(n), "%d", due);
      // The numeral's line box has a deep descent; the caption tucks up under the digits.
      const int nw = renderer.getTextWidthScaled(NOTOSERIF_18_FONT_ID, n, HERO_SCALE);
      renderer.drawTextScaled(NOTOSERIF_18_FONT_ID, (w - nw) / 2, y - numH / 8, n, HERO_SCALE, true);
    } else {
      caption = flashcards::onVacation() ? tr(STR_FLASH_ON_VACATION) : tr(STR_FLASH_NOTHING_DUE);
    }
    if (due > 0) y += numH * 3 / 4 + NUMERAL_GAP;
    const int cw = renderer.getTextWidth(UI_12_FONT_ID, caption);
    renderer.drawText(UI_12_FONT_ID, (w - cw) / 2, y, caption, true);
    y += renderer.getLineHeight(UI_12_FONT_ID) + ORNAMENT_ABOVE;

    // Ornament: two short rules meeting a small diamond, as wide as the caption.
    const int half = std::max(cw / 2, 60);
    renderer.fillRect(w / 2 - half, y, half - 12, 1, true);
    renderer.fillRect(w / 2 + 12, y, half - 12, 1, true);
    for (int i = 0; i <= 5; i++) renderer.fillRect(w / 2 - i, y - 5 + i, 2 * i + 1, 1, true);
    for (int i = 0; i < 5; i++) renderer.fillRect(w / 2 - i, y + 5 - i, 2 * i + 1, 1, true);

    // Contents rows: name, dot leaders, count.
    const int listTop = heroTop_ + heroHeight();
    const int bottom = renderer.getScreenHeight() - HomeTabBar::bottomInset();
    for (size_t i = static_cast<size_t>(std::max(0, topRow_)); i < labels_.size(); i++) {
      const int rowY = listTop + (static_cast<int>(i) - topRow_) * rowHeight_;
      if (rowY + rowHeight_ > bottom) break;
      if (syncRow_ && i == decks.size()) {
        // Laid out like Insights' Refresh row: icon and small text, outlined only while focused.
        constexpr int ICON = 24;
        constexpr int GAP = 8;
        const char* label = tr(STR_ANKI_SYNC_NOW);
        const int rowW = ICON + GAP + renderer.getTextWidth(SMALL_FONT_ID, label);
        const int rx = (left + right - rowW) / 2;
        if (static_cast<int>(i) == selectedRow_) {
          renderer.drawRoundedRect(rx - 16, rowY + 2, rowW + 32, rowHeight_ - 4, 2, StatsWidgets::CARD_RADIUS, true);
        }
        renderer.drawIcon(RefreshIcon24, rx, rowY + (rowHeight_ - ICON) / 2, ICON);
        renderer.drawText(SMALL_FONT_ID, rx + ICON + GAP,
                          rowY + (rowHeight_ - renderer.getLineHeight(SMALL_FONT_ID)) / 2, label, true);
        continue;
      }
      const int lh = renderer.getLineHeight(UI_12_FONT_ID);
      const int ty = rowY + (rowHeight_ - lh) / 2;
      const int baseline = ty + renderer.getFontAscenderSize(UI_12_FONT_ID);
      if (static_cast<int>(i) == selectedRow_) {
        // The theme's selected-row pill, padded around the name and count.
        renderer.fillRoundedRect(left - SEL_PAD, rowY + 4, right - left + 2 * SEL_PAD, rowHeight_ - 8, SEL_RADIUS,
                                 Color::LightGray);
      }
      const int vw = values_[i].empty() ? 0 : renderer.getTextWidth(UI_12_FONT_ID, values_[i].c_str());
      // A long name stops short of its count.
      const std::string name = renderer.truncatedText(UI_12_FONT_ID, labels_[i].c_str(), right - left - vw - 24);
      renderer.drawText(UI_12_FONT_ID, left, ty, name.c_str(), true);
      if (!values_[i].empty()) {  // the sync row has no count and no leaders
        renderer.drawText(UI_12_FONT_ID, right - vw, ty, values_[i].c_str(), true);
        const int from = left + renderer.getTextWidth(UI_12_FONT_ID, name.c_str()) + 12;
        for (int x = from; x < right - vw - 12; x += 5) renderer.fillRect(x, baseline - 1, 1, 1, true);
      }
    }
  }
  if (decks.empty() && !syncRow_) {
    const Rect area = UITheme::getContentArea(renderer);
    const int mid = area.y + area.height / 2;
    UITheme::drawCenteredText(renderer, area, UI_12_FONT_ID, mid - 40, tr(STR_FLASH_EMPTY), true, EpdFontFamily::BOLD);
    UITheme::drawCenteredWrappedText(renderer, Rect{area.x + 40, mid - 8, area.width - 80, 120}, UI_10_FONT_ID,
                                     tr(STR_FLASH_EMPTY_HINT), 4);
  }
  UiListActivity::drawFooter();
}

const char* FlashcardsActivity::confirmHint() const {
  const int i = nav.selected;
  if (syncRow_ && i == static_cast<int>(decks.size())) return tr(STR_SELECT);
  const bool due = i >= 0 && i < static_cast<int>(decks.size()) && decks[i].due + decks[i].fresh > 0;
  return due ? tr(STR_SELECT) : "";
}

void FlashcardsActivity::activateIndex(const int index) {
  if (syncRow_ && index == static_cast<int>(decks.size())) {
    app.clearTapFlash();
    auto sync = makeUniqueNoThrow<AnkiSyncActivity>(renderer, mappedInput, AnkiSyncActivity::Mode::Sync);
    if (!sync) {
      LOG_ERR("FLASH", "OOM: Anki sync");
      return;
    }
    startActivityForResult(std::move(sync), [this](const ActivityResult&) { reload(); });
    return;
  }
  if (index < 0 || index >= static_cast<int>(decks.size())) return;
  // A deck with nothing due has no session to open.
  if (decks[index].due + decks[index].fresh == 0) return;
  app.clearTapFlash();
  nav.selected = index;
  auto review = makeUniqueNoThrow<FlashcardReviewActivity>(renderer, mappedInput, decks[index].language);
  if (!review) {
    LOG_ERR("FLASH", "OOM: review activity");
    return;
  }
  startActivityForResult(std::move(review), [this](const ActivityResult&) { reload(); });
}

void FlashcardsActivity::onBackButton() { activityManager.goHome(); }
