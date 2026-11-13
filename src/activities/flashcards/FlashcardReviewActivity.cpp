#include "FlashcardReviewActivity.h"

#include <FlashcardCsv.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>
#include <cstdio>
#include <utility>
#include <vector>

#include "CrossPointSettings.h"
#include "Epub/Kinsoku.h"
#include "Epub/hyphenation/Hyphenator.h"
#include "FlashcardsActivity.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "activities/reader/DefinitionTextRenderer.h"
#include "components/UITheme.h"
#include "flashcards/AnkiDeck.h"
#include "fontIds.h"
#include "util/CardHtmlPages.h"
#include "util/CjkScript.h"

namespace {
// An Anki deck carries no language: its script is read from a card. Kana means Japanese;
// Han characters without kana mean Chinese.
CjkScript detectScript(const std::string& path) {
  HalFile file;
  if (path.empty() || !Storage.openFileForRead("FLASH", path, file)) return CjkScript::None;
  char buf[1024];
  const int n = file.read(buf, sizeof(buf));
  if (n <= 0) return CjkScript::None;
  bool han = false;
  const auto* p = reinterpret_cast<const unsigned char*>(buf);
  const auto* end = p + n;
  while (p < end && *p) {
    const uint32_t cp = utf8NextCodepoint(&p);
    if (cp >= 0x3041 && cp <= 0x30FF) return CjkScript::Japanese;
    if (cp >= 0x4E00 && cp <= 0x9FFF) han = true;
  }
  return han ? CjkScript::SimplifiedChinese : CjkScript::None;
}

constexpr int PAD = 22;
constexpr int BAND_HEIGHT = 80;      // the rating boxes (60) and the gap above them
constexpr int MAX_SPINE_CHARS = 10;  // keeps drawSpine's per-character strings under 256 bytes of stack
constexpr uint16_t BOOK_SCALE = 176;

// A filled disk, one span per row, so its edge is clean on a one-bit panel.
void fillDisk(const GfxRenderer& r, const int cx, const int cy, const int rad) {
  for (int dy = -rad; dy <= rad; dy++) {
    int dx = 0;
    while ((dx + 1) * (dx + 1) + dy * dy <= rad * rad) dx++;
    r.fillRect(cx - dx, cy + dy, 2 * dx + 1, 1, true);
  }
}

// An opening quotation mark drawn as geometry rather than a scaled glyph: each mark is a round
// head with a tail that tapers up and to the right (a "6" shape). `left`/`bottom` bound the ink;
// `rad` is the head's radius, the whole mark about 3.5 * rad tall.
void drawOpeningQuote(const GfxRenderer& r, const int left, const int bottom, const int rad) {
  for (int m = 0; m < 2; m++) {
    const int cx = left + rad + m * (rad * 5 / 2);
    const int cy = bottom - rad;
    fillDisk(r, cx, cy, rad);
    // The tail: discs stamped along a quadratic curve, shrinking toward the tip.
    const int x0 = cx - rad * 6 / 10, y0 = cy - rad * 4 / 10;
    const int x1 = cx - rad * 7 / 10, y1 = cy - rad * 21 / 10;
    const int x2 = cx + rad * 9 / 10, y2 = cy - rad * 25 / 10;
    constexpr int STEPS = 48;
    for (int t = 0; t <= STEPS; t++) {
      const int u = STEPS - t;
      const int x = (u * u * x0 + 2 * u * t * x1 + t * t * x2) / (STEPS * STEPS);
      const int y = (u * u * y0 + 2 * u * t * y1 + t * t * y2) / (STEPS * STEPS);
      const int tr = std::max(1, (rad * 55 * u + rad * 12 * t) / (100 * STEPS));
      fillDisk(r, x, y, tr);
    }
  }
}

// Reads Anki's furigana syntax over a word split into characters: each base[reading] becomes one
// segment (charStart, charCount, reading, kana count). A base runs back to the previous space or
// ']'. False when there is no reading or the bases and plain text do not spell the word.
template <typename Seg>
bool parseFurigana(const std::string& furigana, const std::string* chars, const int n, Seg* segs, int& segN) {
  segN = 0;
  if (furigana.empty() || furigana.find('[') == std::string::npos) return false;
  std::string spelled;   // the word as the bases and plain text spell it
  size_t baseStart = 0;  // in `spelled`, where the current base began
  for (size_t i = 0; i < furigana.size(); i++) {
    const char c = furigana[i];
    if (c == ' ') {
      baseStart = spelled.size();
    } else if (c == '[') {
      const size_t close = furigana.find(']', i);
      if (close == std::string::npos || baseStart == spelled.size()) return false;
      // The base's characters: count the word's characters up to its start and end.
      int at = 0, first = -1, count = 0;
      size_t bytes = 0;
      while (at < n && bytes < spelled.size()) {
        if (bytes == baseStart) first = at;
        bytes += chars[at].size();
        at++;
        if (first >= 0) count++;
      }
      if (first < 0 || bytes != spelled.size() || segN >= n) return false;
      Seg& g = segs[segN++];
      g.charStart = first;
      g.charCount = count;
      g.text = furigana.substr(i + 1, close - i - 1);
      g.kana = 0;
      for (const unsigned char k : g.text) {
        if ((k & 0xC0) != 0x80) g.kana++;
      }
      i = close;
      baseStart = spelled.size();
    } else {
      spelled += c;
    }
  }
  std::string word;
  for (int i = 0; i < n; i++) word += chars[i];
  return segN > 0 && spelled == word;
}

// Lays `text` out top to bottom in columns running right to left (tategaki), the saved word (between
// \x01 and \x02) bold with a rule beside it. Columns are `cell` apart in rows and `pitch` apart,
// the first one's left edge at `firstLeft`. Kinsoku: a mark that may not start a column hangs at
// the foot of the previous one; an opening bracket never ends one. With draw = false it only
// counts. Returns the columns used; more than maxCols means it did not fit.
int drawVertical(const GfxRenderer& r, const int font, const uint16_t scale, const std::string& text,
                 const int firstLeft, const int top, const int cell, const int pitch, const int rows, const int maxCols,
                 const bool draw) {
  const int ascender = r.getFontAscenderSize(font) * scale / 256;
  const auto* p = reinterpret_cast<const unsigned char*>(text.c_str());
  bool bold = false;
  int col = 0, row = 0;
  while (*p) {
    if (*p == 0x01 || *p == 0x02) {
      bold = *p == 0x01;
      p++;
      continue;
    }
    const auto* start = p;
    const uint32_t cp = utf8NextCodepoint(&p);
    if (cp == ' ' || cp == 0x3000) continue;
    if (row >= rows) {
      const bool hang = row == rows && Kinsoku::isLineStartProhibited(cp);
      if (!hang) {
        col++;
        row = 0;
      }
    } else if (row == rows - 1 && Kinsoku::isLineEndProhibited(cp)) {
      col++;
      row = 0;
    }
    if (col >= maxCols) return maxCols + 1;
    if (draw) {
      const int x = firstLeft - col * pitch;
      const int y = top + row * cell;
      const auto style = bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
      const std::string ch(reinterpret_cast<const char*>(start), p - start);
      const int shift = Kinsoku::verticalShiftType(cp);
      // drawTextScaled takes a line's top; this puts the baseline near the foot of the cell.
      const int textTop = y + cell * 88 / 100 - ascender;
      if (Kinsoku::needsVerticalRotation(cp)) {
        r.drawCharVerticalRotatedInCell(font, x, y, cell, cp, shift, true, style, nullptr, nullptr, scale);
      } else if (shift == 1) {
        // 、 and 。 sit at the head of their cell, to the right.
        r.drawTextScaled(font, x + cell / 2, textTop - cell / 2, ch.c_str(), scale, true, style);
      } else {
        const int w = r.getTextWidthScaled(font, ch.c_str(), scale, style);
        const int dx = Kinsoku::isSmallKana(cp) ? cell / 8 : 0;
        r.drawTextScaled(font, x + (cell - w) / 2 + dx, textTop, ch.c_str(), scale, true, style);
      }
      if (bold) r.fillRect(x + cell + 3, y, 2, cell, true);
    }
    row++;
  }
  return col + 1;
}

bool isCjk(const uint32_t cp) { return cp >= 0x2E80; }

// Interval under a rating button, Anki style.
void formatInterval(const uint16_t days, char* out, const size_t size) {
  if (days == 0) {
    snprintf(out, size, "<10m");
  } else if (days < 30) {
    snprintf(out, size, "%ud", days);
  } else if (days < 365) {
    snprintf(out, size, "%umo", (days + 15) / 30);
  } else {
    snprintf(out, size, "%u.%uy", days / 365, (days % 365) * 10 / 365);
  }
}

// Wraps `text` into `width` and draws it, the saved word (between \x01 and \x02) set bold and
// underlined. Latin breaks at spaces, CJK between characters, keeping closing punctuation on its
// line. With draw = false it only counts. Returns the lines used, at most maxLines.
int drawRich(const GfxRenderer& r, const int font, const uint16_t scale, const std::string& text, const int x,
             const int y, const int width, const int lineH, const int maxLines, const bool draw,
             const bool hyphenate = false) {
  struct Token {
    size_t start;
    size_t len;
    bool bold;
    bool space;
    bool cjk;
    bool glued;  // touches the previous word (a comma after the marked word): never breaks before it
  };
  std::vector<Token> tokens;
  tokens.reserve(text.size() / 2 + 4);
  bool bold = false;
  const auto* base = reinterpret_cast<const unsigned char*>(text.c_str());
  const auto* p = base;
  while (*p) {
    if (*p == 1 || *p == 2) {
      bold = *p == 1;
      ++p;
      continue;
    }
    const auto* start = p;
    const uint32_t cp = utf8NextCodepoint(&p);
    const bool space = cp == ' ' || cp == '\n';
    const bool cjk = cp >= 0x2E80;
    Token& last = tokens.empty() ? tokens.emplace_back(Token{0, 0, bold, true, false, false}) : tokens.back();
    const bool joins = !space && !cjk && !last.space && !last.cjk && last.len > 0;
    if (joins && last.bold == bold) {
      last.len += static_cast<size_t>(p - start);  // extend the current Latin word
    } else {
      tokens.push_back({static_cast<size_t>(start - base), static_cast<size_t>(p - start), bold, space, cjk, joins});
    }
  }
  const int ascent = r.getTextHeight(font) * scale / 256;
  int line = 0, lineW = 0;
  std::string piece;
  for (const auto& t : tokens) {
    if (t.len == 0) continue;
    piece.assign(text, t.start, t.len);
    const auto style = t.bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    if (t.space && lineW == 0) continue;
    // A lone space measures 0 in some faces (a TTF's advance is only applied between glyphs).
    const int tw = t.space ? std::max(1, r.getSpaceWidth(font, style) * scale / 256)
                           : r.getTextWidthScaled(font, piece.c_str(), scale, style);
    if (!t.space && !t.cjk && !t.glued && lineW + tw > width && (hyphenate || tw > width)) {
      // A word that does not fit: hyphenate it at the language's syllable breaks (the reader's
      // Liang patterns), so a long compound runs on instead of jumping or overflowing.
      const auto drawSeg = [&](const std::string& seg, const int segW) {
        if (!draw) return;
        const int ty = y + line * lineH;
        r.drawTextScaled(font, x + lineW, ty, seg.c_str(), scale, true, style);
        if (t.bold) r.fillRect(x + lineW, ty + ascent + 4, segW, 2, true);
      };
      std::string rest = piece;
      while (true) {
        const int rw = r.getTextWidthScaled(font, rest.c_str(), scale, style);
        const int avail = width - lineW;
        if (rw <= avail) {
          drawSeg(rest, rw);
          lineW += rw;
          break;
        }
        size_t cut = 0;
        std::string head;
        // Offsets come in order: the last one that fits leaves the least for the next line.
        const auto breaks = hyphenate ? Hyphenator::breakOffsets(rest, /*includeFallback=*/rw > width)
                                      : Hyphenator::breakOffsets(rest, /*includeFallback=*/true);
        for (const auto& br : breaks) {
          if (!hyphenate && rw <= width) break;  // other text only splits a word wider than the column
          std::string h = rest.substr(0, br.byteOffset) + (br.requiresInsertedHyphen ? "-" : "");
          if (r.getTextWidthScaled(font, h.c_str(), scale, style) > avail) break;
          cut = br.byteOffset;
          head = std::move(h);
        }
        if (cut == 0 && lineW == 0) {
          drawSeg(rest, rw);  // no break fits even a whole line: let it run
          lineW += rw;
          break;
        }
        if (cut > 0) {
          const int headW = r.getTextWidthScaled(font, head.c_str(), scale, style);
          drawSeg(head, headW);
          lineW += headW;
        }
        if (line + 1 >= maxLines) {
          if (draw) r.drawTextScaled(font, x + std::min(lineW, width - 12), y + line * lineH, "\xE2\x80\xA6", scale);
          return maxLines;
        }
        ++line;
        lineW = 0;
        if (cut > 0) rest.erase(0, cut);
      }
      continue;
    }
    const bool closing = t.cjk && (piece == "。" || piece == "、" || piece == "」" || piece == "』" || piece == "！" ||
                                   piece == "？" || piece == "）");
    if (lineW > 0 && lineW + tw > width && !closing && !t.glued) {
      if (line + 1 >= maxLines) {
        // Out of room: end the last line with an ellipsis rather than cutting mid-thought.
        if (draw) r.drawTextScaled(font, x + std::min(lineW, width - 12), y + line * lineH, "\xE2\x80\xA6", scale);
        return maxLines;
      }
      ++line;
      lineW = 0;
      if (t.space) continue;
    }
    if (draw) {
      const int ty = y + line * lineH;
      if (!t.space) r.drawTextScaled(font, x + lineW, ty, piece.c_str(), scale, true, style);
      if (t.bold && !t.space) r.fillRect(x + lineW, ty + ascent + 4, tw, 2, true);
    }
    lineW += tw;
  }
  return lineW > 0 ? line + 1 : line;
}

// `text` cut to `width` with a trailing ellipsis, on a UTF-8 boundary.
std::string fitText(const GfxRenderer& r, const int font, const uint16_t scale, const std::string& text,
                    const int width, const EpdFontFamily::Style style) {
  if (r.getTextWidthScaled(font, text.c_str(), scale, style) <= width) return text;
  std::string cut = text;
  while (!cut.empty()) {
    // Drop one whole character: its continuation bytes, then its lead byte.
    while (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0xC0) == 0x80) cut.pop_back();
    if (!cut.empty()) cut.pop_back();
    if (r.getTextWidthScaled(font, (cut + "\xE2\x80\xA6").c_str(), scale, style) <= width) break;
  }
  return cut + "\xE2\x80\xA6";
}

const char* ratingName(const int i) {
  switch (i) {
    case 0:
      return tr(STR_FLASH_AGAIN);
    case 1:
      return tr(STR_FLASH_HARD);
    case 2:
      return tr(STR_FLASH_GOOD);
    default:
      return tr(STR_FLASH_EASY);
  }
}
}  // namespace

FlashcardReviewActivity::FlashcardReviewActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                 std::string language)
    : Activity("FlashcardReview", renderer, mappedInput), language(std::move(language)) {}

void FlashcardReviewActivity::onEnter() {
  Activity::onEnter();
  // Load the deck's CJK companion face, as a book in that script does: without it the card's
  // kanji fall back to the small built-in set.
  sdFontSystem.ensureLoaded(renderer);
  opened = session.open(language);
  script = cjk::scriptForLanguage(language);
  if (opened && ankideck::isAnkiKey(language) && session.current(card)) script = detectScript(card.frontPath);
  sdFontSystem.setCjkFallbackNeeded(renderer, script);
  Hyphenator::setPreferredLanguage(language);
  loadCard();
  requestUpdate();
}

void FlashcardReviewActivity::onExit() {
  session.save();
  sdFontSystem.setCjkFallbackNeeded(renderer, CjkScript::None);
  Activity::onExit();
}

void FlashcardReviewActivity::loadCard() {
  showBack = false;
  selected = 2;
  cardOk = opened && session.current(card);
  cardIsNew = session.currentIsNew();
  layoutHtmlSide();
  session.previews(intervals);
}

void FlashcardReviewActivity::rate(const int index) {
  session.rate(static_cast<fsrs::Rating>(index + 1));
  loadCard();
  requestUpdate();
}

FlashcardReviewActivity::Rect4 FlashcardReviewActivity::cardRect() const {
  const Rect a = UITheme::getInstance().getScreenSafeArea(renderer, !mappedInput.hasTouch());
  return {a.x + PAD, a.y + PAD, a.width - 2 * PAD, a.height - PAD - 8 - BAND_HEIGHT};
}

FlashcardReviewActivity::Rect4 FlashcardReviewActivity::ratingsRect() const {
  const Rect4 c = cardRect();
  return {c.x, c.y + c.h + 18, c.w, BAND_HEIGHT - 18};
}

void FlashcardReviewActivity::loop() {
  using Button = MappedInputManager::Button;
  if (mappedInput.wasReleased(Button::Back)) {
    session.save();
    finish();
    return;
  }
  // The overview only opens decks with cards due; one that turns out empty goes straight back.
  if (opened && session.total() == 0) {
    finish();
    return;
  }
  const bool done = !opened || session.finished();
  int tx = 0, ty = 0;
  if (mappedInput.wasScreenTapped(tx, ty)) {
    if (done) {
      finish();
      return;
    }
    if (showBack) {
      const Rect4 band = ratingsRect();
      if (band.contains(tx, ty)) {
        rate(std::clamp((tx - band.x) * 4 / std::max(band.w, 1), 0, 3));
        return;
      }
    }
    if (cardRect().contains(tx, ty)) {
      showBack = !showBack;
      layoutHtmlSide();
      requestUpdate();
    }
    return;
  }
  if (done) {
    if (mappedInput.wasReleased(Button::Confirm)) finish();
    return;
  }
  // A long Anki side pages first: the side buttons move through it before they turn the card.
  if (isHtmlCard() && htmlPages.size() > 1) {
    if (mappedInput.wasReleased(Button::PageForward) && htmlPage + 1 < htmlPages.size()) {
      htmlPage++;
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(Button::PageBack) && htmlPage > 0) {
      htmlPage--;
      requestUpdate();
      return;
    }
  }
  const bool turn = mappedInput.wasReleased(Button::PageForward) || mappedInput.wasReleased(Button::PageBack);
  if (!showBack) {
    if (mappedInput.wasReleased(Button::Confirm) || turn) {
      showBack = true;
      layoutHtmlSide();
      requestUpdate();
    }
    return;
  }
  if (turn) {
    showBack = false;
    layoutHtmlSide();
    requestUpdate();
  } else if (mappedInput.wasPressed(Button::Left)) {
    selected = (selected + 3) % 4;
    requestUpdate();
  } else if (mappedInput.wasPressed(Button::Right)) {
    selected = (selected + 1) % 4;
    requestUpdate();
  } else if (mappedInput.wasReleased(Button::Confirm)) {
    rate(selected);
  }
}

void FlashcardReviewActivity::drawSpine(const Rect4& spine) const {
  const int font = sdFontSystem.effectiveReaderFontId(cjk::scriptForLanguage(language));
  const auto* p = reinterpret_cast<const unsigned char*>(card.word.c_str());
  const uint32_t first = *p ? utf8NextCodepoint(&p) : 0;
  const int lh = std::max(1, renderer.getLineHeightScaled(font, 256));
  if (isCjk(first)) {
    // Upright, one character under the other, as a vertical book sets it.
    std::string chars[MAX_SPINE_CHARS];
    int n = 0;
    const auto* q = reinterpret_cast<const unsigned char*>(card.word.c_str());
    while (*q && n < MAX_SPINE_CHARS) {
      const auto* start = q;
      utf8NextCodepoint(&q);
      chars[n++].assign(reinterpret_cast<const char*>(start), static_cast<size_t>(q - start));
    }
    if (n == 0) return;
    const bool cut = *q != 0;  // longer than the spine shows
    // On a Japanese back the reading stands beside the word, as furigana do in vertical text.
    const bool ruby = showBack && cjk::scriptForLanguage(language) == CjkScript::Japanese && !card.reading.empty();
    const int wordW = ruby ? spine.w * 64 / 100 : spine.w;
    const int step = std::min(wordW * (ruby ? 90 : 80) / 100, (spine.h - 48) / n);
    // Sized from the glyph's own width: a CJK face's line height is far taller than its ink.
    const int em = std::max(1, renderer.getTextWidthScaled(font, chars[0].c_str(), 256, EpdFontFamily::BOLD));
    const auto scale = static_cast<uint16_t>(std::clamp(step * 256 * 86 / 100 / em, 160, 1536));
    const int top = spine.y + (spine.h - n * step) / 2;
    const int wordX = ruby ? spine.x + spine.w * 4 / 100 : spine.x;
    for (int i = 0; i < n; i++) {
      const int w = renderer.getTextWidthScaled(font, chars[i].c_str(), scale, EpdFontFamily::BOLD);
      renderer.drawTextScaled(font, wordX + (wordW - w) / 2, top + i * step, chars[i].c_str(), scale, true,
                              EpdFontFamily::BOLD);
    }
    if (ruby) {
      // Each run of kanji gets its own part of the reading: the word's kana (okurigana) are found
      // in the reading and split it, e.g. 木漏れ日 / こもれび -> 木漏:こも, 日:び.
      // About 700 bytes of working arrays: on the heap, once per drawn back, so the render task's
      // stack stays small. Without it the reading is simply not drawn.
      struct Seg {
        int charStart, charCount;
        std::string text;
        int kana;
      };
      constexpr int MAX_READING = 32;
      struct Work {
        uint32_t wordCp[MAX_SPINE_CHARS];
        uint32_t rd[MAX_READING];
        int rdOff[MAX_READING + 1];
        Seg segs[MAX_SPINE_CHARS];
      };
      auto work = makeUniqueNoThrow<Work>();
      if (!work) {
        LOG_ERR("FLASH", "OOM: furigana layout");
        return;
      }
      uint32_t* wordCp = work->wordCp;
      uint32_t* rd = work->rd;
      int* rdOff = work->rdOff;
      Seg* segs = work->segs;
      for (int i = 0; i < n; i++) {
        const auto* c = reinterpret_cast<const unsigned char*>(chars[i].c_str());
        wordCp[i] = utf8NextCodepoint(&c);
      }
      int rdLen = 0;
      const auto* r = reinterpret_cast<const unsigned char*>(card.reading.c_str());
      while (*r && rdLen < MAX_READING) {
        rdOff[rdLen] = static_cast<int>(r - reinterpret_cast<const unsigned char*>(card.reading.c_str()));
        const uint32_t cp = utf8NextCodepoint(&r);
        if (cp != ' ') rd[rdLen++] = cp;
      }
      // A word cut short on the spine, or a reading longer than the column holds, gets no reading
      // rather than one placed against the wrong characters.
      if (cut || *r) return;
      rdOff[rdLen] = static_cast<int>(r - reinterpret_cast<const unsigned char*>(card.reading.c_str()));
      const auto isKana = [](uint32_t cp) { return cp >= 0x3041 && cp <= 0x30FF; };
      // Katakana and hiragana count as the same sound.
      const auto sameKana = [](uint32_t a, uint32_t b) {
        if (a >= 0x30A1 && a <= 0x30F6) a -= 0x60;
        if (b >= 0x30A1 && b <= 0x30F6) b -= 0x60;
        return a == b;
      };
      int segN = 0;
      // The saved split when the card has one (Anki's 木[こ]漏[も]れ 日[び]); its bases must spell the word.
      bool ok = parseFurigana(card.furigana, chars, n, segs, segN);
      if (!ok) {
        segN = 0;
        ok = rdLen > 0;
        int pos = 0;
        for (int i = 0; ok && i < n;) {
          if (isKana(wordCp[i])) {
            ok = pos < rdLen && sameKana(rd[pos], wordCp[i]);
            pos++;
            i++;
            continue;
          }
          int j = i;
          while (j < n && !isKana(wordCp[j])) j++;
          int end = rdLen;
          if (j < n) {
            end = pos + 1;
            while (end < rdLen && !sameKana(rd[end], wordCp[j])) end++;
            ok = end < rdLen;
          }
          ok = ok && end > pos;
          if (ok) {
            segs[segN++] = {i, j - i, card.reading.substr(rdOff[pos], rdOff[end] - rdOff[pos]), end - pos};
          }
          pos = end;
          i = j;
        }
        ok = ok && pos == rdLen;
      }
      if (!ok) {  // the reading does not follow the word's kana: centre all of it along the word
        segN = 1;
        segs[0] = {0, n, card.reading, rdLen};
      }
      const int em256 = std::max(1, renderer.getTextWidthScaled(font, "\xE3\x81\x82", 256));  // あ
      const int wordEm = em * scale / 256;
      const int inkShift = renderer.getFontAscenderSize(font) * scale / 256 - wordEm * 38 / 100 - step / 2;
      int totalKana = 0;
      for (int k = 0; k < segN; k++) totalKana += segs[k].kana;
      const int baseCell = std::max(8, std::min(spine.w * 26 / 100, (spine.h - 48) / std::max(totalKana, 1)));
      for (int k = 0; k < segN; k++) {
        const Seg& g = segs[k];
        if (g.kana <= 0) continue;
        const int span = g.charCount * step;
        // Three kana over one kanji may run past it a little, but not into the next run's reading.
        const int cell = std::max(8, std::min(baseCell, span * 3 / 2 / g.kana));
        const auto rubyScale = static_cast<uint16_t>(std::clamp(cell * 256 / em256, 96, 512));
        // Centred on the kanji's ink, which sits below the middle of its step (baseline at the ascender).
        const int rubyTop = top + g.charStart * step + inkShift + (span - g.kana * cell) / 2;
        drawVertical(renderer, font, rubyScale, g.text, wordX + wordW, std::max(spine.y + 8, rubyTop), cell, cell,
                     g.kana, 1, true);
      }
    }
    return;
  }
  // Other scripts run down the spine, as on an English-language book's back.
  const int width256 = std::max(1, renderer.getTextWidthScaled(font, card.word.c_str(), 256, EpdFontFamily::BOLD));
  const int byHeight = spine.w * 86 / 100 * 256 / lh;
  const int byLength = (spine.h - 48) * 256 / width256;
  const auto scale = static_cast<uint16_t>(std::clamp(std::min(byHeight, byLength), 128, 1280));
  const int length = width256 * scale / 256;
  const int thickness = lh * scale / 256;
  // The anchor is the line's start on its baseline side; the ink sits about 3/10 of a line inward. The
  // extra nudge toward the descender side centres the letters' body optically: ascenders are far
  // more common than descenders, so centring the ink alone leaves the word looking off to one side.
  renderer.drawTextRotated90CCWScaled(font, spine.x + (spine.w - thickness) / 2 + thickness * 3 / 10 - thickness / 16,
                                      spine.y + (spine.h - length) / 2, card.word.c_str(), scale, true,
                                      EpdFontFamily::BOLD);
}

void FlashcardReviewActivity::drawFront(const Rect4& body) const {
  const int font = sdFontSystem.effectiveReaderFontId(cjk::scriptForLanguage(language));

  // The sentence it was saved from, the word marked, under a large opening quote.
  const std::string sentence = flashcards::stripExportHtml(card.sentence, "\x01", "\x02", " ");
  if (cjk::scriptForLanguage(language) == CjkScript::Japanese) {
    drawFrontVertical(body, font, sentence);
    return;
  }
  // The largest size at which the whole sentence fits; the smallest one ends in an ellipsis.
  static constexpr uint16_t SIZES[] = {256, 232, 208, 192};
  uint16_t scale = SIZES[0];
  int lineH = 0;
  const int bookH = renderer.getLineHeightScaled(font, BOOK_SCALE);
  const int top = body.y;
  const int bottom = body.y + body.h - bookH - 30;
  const int quoteH = renderer.getLineHeightScaled(NOTOSERIF_18_FONT_ID, 768) / 2;
  int maxLines = 1, lines = 1;
  for (const uint16_t size : SIZES) {
    scale = size;
    lineH = renderer.getLineHeightScaled(font, scale) * 112 / 100;
    maxLines = std::max(1, (bottom - top - quoteH) / lineH);
    lines = drawRich(renderer, font, scale, sentence, body.x, 0, body.w, lineH, maxLines + 1, false, true);
    if (lines <= maxLines) break;
  }
  lines = std::min(lines, maxLines);
  const int blockY = top + (bottom - top - quoteH - lines * lineH) / 2;
  drawOpeningQuote(renderer, body.x + 2, blockY + quoteH - 26, 9);
  drawRich(renderer, font, scale, sentence, body.x, blockY + quoteH, body.w, lineH, maxLines, true, true);

  if (!card.book.empty()) {
    renderer.fillRect(body.x, body.y + body.h - bookH - 14, body.w, 1, true);
    const std::string book = fitText(renderer, font, BOOK_SCALE, card.book, body.w, EpdFontFamily::ITALIC);
    renderer.drawTextScaled(font, body.x, body.y + body.h - bookH, book.c_str(), BOOK_SCALE, true,
                            EpdFontFamily::ITALIC);
  }
}

// Japanese reads in columns on the front, as in a book: the sentence starts against the spine on
// the right, and the book's title stands in its own column at the bottom left. The back stays horizontal.
void FlashcardReviewActivity::drawFrontVertical(const Rect4& body, const int font, const std::string& sentence) const {
  static constexpr uint16_t SIZES[] = {256, 232, 208, 192};
  const int bookCell = std::max(1, renderer.getTextWidthScaled(font, "\xE5\x9B\xBD", BOOK_SCALE));
  const int bookW = card.book.empty() ? 0 : bookCell + 16;
  const int top = body.y;
  const int bottom = body.y + body.h;
  uint16_t scale = SIZES[0];
  int cell = 0, pitch = 0, rows = 1, maxCols = 1;
  for (const uint16_t size : SIZES) {
    scale = size;
    cell = std::max(1, renderer.getTextWidthScaled(font, "\xE5\x9B\xBD", scale));  // 国: one em
    pitch = cell * 150 / 100;
    rows = std::max(1, (bottom - top) / cell);
    maxCols = std::max(1, (body.w - bookW - cell) / pitch + 1);
    if (drawVertical(renderer, font, scale, sentence, 0, top, cell, pitch, rows, maxCols, false) <= maxCols) break;
  }
  drawVertical(renderer, font, scale, sentence, body.x + body.w - cell, top, cell, pitch, rows, maxCols, true);

  if (!card.book.empty()) {
    // One column only: a title longer than the card is cut.
    const int bookRows = std::max(1, (bottom - top) / bookCell);
    std::string book;
    int n = 0;
    const auto* q = reinterpret_cast<const unsigned char*>(card.book.c_str());
    while (*q && n < bookRows) {
      const auto* start = q;
      const uint32_t cp = utf8NextCodepoint(&q);
      book.append(reinterpret_cast<const char*>(start), static_cast<size_t>(q - start));
      if (cp != ' ') n++;
    }
    drawVertical(renderer, font, BOOK_SCALE, book, body.x, bottom - n * bookCell, bookCell, bookCell, n, 1, true);
  }
}

void FlashcardReviewActivity::drawBack(const Rect4& body) const {
  const int font = sdFontSystem.effectiveReaderFontId(cjk::scriptForLanguage(language));
  int y = body.y;
  // Japanese shows its reading beside the word on the spine.
  if (!card.reading.empty() && cjk::scriptForLanguage(language) != CjkScript::Japanese) {
    // A long reading shrinks to fit the line, and wraps only past the smallest size.
    uint16_t scale = 256;
    while (scale > 176 && renderer.getTextWidthScaled(font, card.reading.c_str(), scale) > body.w) {
      scale -= 16;
    }
    const int readingH = renderer.getLineHeightScaled(font, scale);
    if (renderer.getTextWidthScaled(font, card.reading.c_str(), scale) <= body.w) {
      renderer.drawTextScaled(font, body.x, y, card.reading.c_str(), scale, true);
      y += readingH + 8;
    } else {
      y += drawRich(renderer, font, scale, card.reading, body.x, y, body.w, readingH, 2, true) * readingH + 8;
    }
    renderer.fillRect(body.x, y, body.w, 1, true);
    y += 18;
  }

  // The foot holds the sentence and book. The senses get the room above three sentence lines;
  // the sentence then grows into whatever they leave free.
  const std::string sentence = flashcards::stripExportHtml(card.sentence, "\x01", "\x02", " ");
  constexpr uint16_t SENT = 192;
  constexpr int MIN_SENT_LINES = 3;
  const int sentLineH = renderer.getLineHeightScaled(font, SENT) * 110 / 100;
  const int bookH = card.book.empty() ? 0 : renderer.getLineHeightScaled(font, BOOK_SCALE) + 6;
  const int footBottom = body.y + body.h - bookH - 22;
  const int sensesTop = y;

  // One numbered row per sense: the numbers in their own bold column. A dictionary that numbers
  // its senses also carries examples, translations and cross-references between them; the card
  // keeps the senses, the part-of-speech line and the "→" grammar notes.
  const std::string definition = flashcards::stripExportHtml(card.definition, "", "", "\n");
  constexpr uint16_t DEF = 184;
  constexpr uint16_t NOTE = 160;
  const int defLineH = renderer.getLineHeightScaled(font, DEF) * 108 / 100;
  const int noteLineH = renderer.getLineHeightScaled(font, NOTE);
  const int numW = renderer.getTextWidthScaled(font, "00", DEF, EpdFontFamily::BOLD) + 8;
  const auto isNumbered = [](const std::string& row) {
    const size_t dot = row.find(". ");
    return dot != std::string::npos && dot >= 1 && dot <= 2 && isdigit(static_cast<unsigned char>(row[0]));
  };
  bool structured = false;
  for (size_t p = 0; p < definition.size();) {
    size_t e = definition.find('\n', p);
    if (e == std::string::npos) e = definition.size();
    if (isNumbered(definition.substr(p, std::min<size_t>(e - p, 5)))) {
      structured = true;
      break;
    }
    p = e + 1;
  }
  // Lays the senses out from sensesTop down to limit; returns the y below the last one.
  const auto drawSenses = [&](const int limit, const bool draw) {
    int y = sensesTop;
    size_t pos = 0;
    bool seenSense = false;
    while (pos <= definition.size() && y + noteLineH <= limit) {
      size_t end = definition.find('\n', pos);
      if (end == std::string::npos) end = definition.size();
      std::string row = definition.substr(pos, end - pos);
      pos = end + 1;
      if (row.empty()) continue;
      if (structured && isNumbered(row)) {
        if (y + defLineH > limit) break;
        seenSense = true;
        const size_t dot = row.find(". ");
        if (draw) renderer.drawTextScaled(font, body.x, y, row.substr(0, dot).c_str(), DEF, true, EpdFontFamily::BOLD);
        const int rows = (limit - y) / defLineH;
        y += drawRich(renderer, font, DEF, row.substr(dot + 2), body.x + numW, y, body.w - numW, defLineH, rows, draw) *
                 defLineH +
             4;
      } else if (structured && row.compare(0, 4, "\xE2\x86\x92 ") == 0) {
        // "→ NOUN": a grammar note under its sense.
        if (draw)
          renderer.drawTextScaled(
              font, body.x + numW, y,
              fitText(renderer, font, NOTE, row.substr(4), body.w - numW, EpdFontFamily::ITALIC).c_str(), NOTE, true,
              EpdFontFamily::ITALIC);
        y += noteLineH + 2;
      } else if (structured && !seenSense) {
        // The part-of-speech line ahead of the first sense.
        if (draw)
          renderer.drawTextScaled(font, body.x, y,
                                  fitText(renderer, font, NOTE, row, body.w, EpdFontFamily::BOLD).c_str(), NOTE, true,
                                  EpdFontFamily::BOLD);
        y += noteLineH + 4;
      } else if (!structured) {
        const int rows = (limit - y) / defLineH;
        if (rows <= 0) break;
        y += drawRich(renderer, font, DEF, row, body.x, y, body.w, defLineH, rows, draw) * defLineH + 4;
      }
    }
    return y;
  };
  const int sensesEnd = drawSenses(footBottom - MIN_SENT_LINES * sentLineH - 26, false);
  const int roomLines = std::max(MIN_SENT_LINES, (footBottom - sensesEnd - 26) / sentLineH);
  const int sentLines = drawRich(renderer, font, SENT, sentence, body.x, 0, body.w, sentLineH, roomLines, false, true);
  const int footTop = footBottom - sentLines * sentLineH;
  drawSenses(footTop - 12, true);

  renderer.fillRect(body.x, footTop, body.w, 1, true);
  drawRich(renderer, font, SENT, sentence, body.x, footTop + 14, body.w, sentLineH, sentLines, true, true);
  if (bookH) {
    const std::string book = fitText(renderer, font, BOOK_SCALE, card.book, body.w, EpdFontFamily::ITALIC);
    renderer.drawTextScaled(font, body.x, body.y + body.h - bookH + 6, book.c_str(), BOOK_SCALE, true,
                            EpdFontFamily::ITALIC);
  }
}

void FlashcardReviewActivity::drawCard(const Rect4& c) const {
  renderer.drawRect(c.x, c.y, c.w, c.h, 2, true);
  const Rect4 inner{c.x + 6, c.y + 6, c.w - 12, c.h - 12};
  renderer.drawRect(inner.x, inner.y, inner.w, inner.h, true);
  if (isHtmlCard()) {
    drawHtmlCard(inner);
    return;
  }
  const int spineW = std::clamp(inner.w * 26 / 100, 90, 160);
  // A Japanese card is bound on the right, as a vertical book is; it stays there when turned.
  const bool rightSpine = cjk::scriptForLanguage(language) == CjkScript::Japanese;
  const int spineX = rightSpine ? inner.x + inner.w - spineW : inner.x;
  const int ruleX = rightSpine ? spineX - 1 : inner.x + spineW;
  renderer.drawLine(ruleX, inner.y, ruleX, inner.y + inner.h - 1, true);
  if (!cardOk) {
    UITheme::drawCenteredText(renderer, Rect{inner.x, inner.y, inner.w, inner.h}, UI_12_FONT_ID, inner.y + inner.h / 2,
                              tr(STR_FLASH_CARD_ERROR));
    return;
  }
  drawSpine({spineX, inner.y, spineW, inner.h});
  const Rect4 body{rightSpine ? inner.x + 22 : inner.x + spineW + 24, inner.y + 26, inner.w - spineW - 46,
                   inner.h - 48};
  if (showBack) {
    drawBack(body);
  } else {
    drawFront(body);
  }
}

int FlashcardReviewActivity::htmlFont() const { return sdFontSystem.effectiveReaderFontId(script); }

FlashcardReviewActivity::Rect4 FlashcardReviewActivity::htmlBodyRect() const {
  const Rect4 c = cardRect();
  // Inside the double rule, with room at the foot for the page count.
  return {c.x + 6 + 18, c.y + 6 + 16, c.w - 12 - 36, c.h - 12 - 32 - 18};
}

void FlashcardReviewActivity::layoutHtmlSide() {
  htmlPages.clear();
  htmlPage = 0;
  if (!cardOk || !isHtmlCard() || card.tooLarge) return;
  const Rect4 body = htmlBodyRect();
  const std::string& path = showBack ? card.backPath : card.frontPath;
  if (!buildCardHtmlPages(renderer, path, card.cssPath, static_cast<uint16_t>(body.w), static_cast<uint16_t>(body.h),
                          htmlFont(), /*furigana=*/true, htmlPages)) {
    LOG_ERR("FLASH", "Card side %s not laid out", path.c_str());
  }
}

void FlashcardReviewActivity::drawHtmlCard(const Rect4& inner) const {
  const Rect area{inner.x, inner.y, inner.w, inner.h};
  if (!cardOk || card.tooLarge || htmlPages.empty()) {
    constexpr int PAD = 20;
    UITheme::drawCenteredWrappedText(renderer, Rect{area.x + PAD, area.y, area.width - 2 * PAD, area.height},
                                     UI_12_FONT_ID,
                                     card.tooLarge ? tr(STR_FLASH_CARD_TOO_LARGE) : tr(STR_FLASH_CARD_ERROR), 6);
    return;
  }
  const Rect4 body = htmlBodyRect();
  htmlPages[std::min(htmlPage, htmlPages.size() - 1)]->render(renderer, htmlFont(), body.x, body.y);
  if (htmlPages.size() > 1) {
    char count[16];
    snprintf(count, sizeof(count), "%u / %u", static_cast<unsigned>(htmlPage + 1),
             static_cast<unsigned>(htmlPages.size()));
    const int w = renderer.getTextWidth(SMALL_FONT_ID, count);
    renderer.drawText(SMALL_FONT_ID, inner.x + inner.w - 18 - w, inner.y + inner.h - 26, count, true);
  }
}

void FlashcardReviewActivity::drawRatings(const Rect4& band) const {
  // Four ruled boxes, each the rating's name over when the card comes back.
  constexpr int GAP = 8;
  constexpr int BOX_H = 60;
  const int boxW = (band.w - 3 * GAP) / 4;
  const bool cursor = MappedInputManager::buttonCursorShown();
  const int nameH = renderer.getLineHeightScaled(UI_10_FONT_ID, 256);
  const int ivH = renderer.getLineHeightScaled(SMALL_FONT_ID, 256);
  const int textTop = band.y + (BOX_H - nameH - ivH - 2) / 2;
  for (int i = 0; i < 4; i++) {
    const int bx = band.x + i * (boxW + GAP);
    const bool filled = cursor && i == selected;
    if (filled) {
      renderer.fillRoundedRect(bx, band.y, boxW, BOX_H, 10, Color::Black);
    } else {
      renderer.drawRoundedRect(bx, band.y, boxW, BOX_H, 2, 10, true);
    }
    const char* name = ratingName(i);
    const int nw = renderer.getTextWidth(UI_10_FONT_ID, name);
    renderer.drawText(UI_10_FONT_ID, bx + (boxW - nw) / 2, textTop, name, !filled);
    char interval[12];
    formatInterval(intervals[i], interval, sizeof(interval));
    const int iw = renderer.getTextWidth(SMALL_FONT_ID, interval);
    renderer.drawText(SMALL_FONT_ID, bx + (boxW - iw) / 2, textTop + nameH + 2, interval, !filled);
  }
}

void FlashcardReviewActivity::drawDone(const Rect4& a) const {
  const Rect area{a.x, a.y, a.w, a.h};
  const int cx = a.x + a.w / 2;
  int y = a.y + a.h / 2 - 190;
  // A ruled circle with a tick: the session's seal.
  constexpr int SEAL = 84;
  renderer.drawRoundedRect(cx - SEAL / 2, y, SEAL, SEAL, 2, SEAL / 2, true);
  renderer.drawLine(cx - 20, y + 44, cx - 6, y + 58, 3, true);
  renderer.drawLine(cx - 6, y + 58, cx + 22, y + 28, 3, true);
  y += SEAL + 30;
  UITheme::drawCenteredText(renderer, area, NOTOSERIF_18_FONT_ID, y, tr(STR_FLASH_DONE), true, EpdFontFamily::BOLD);
  y += renderer.getLineHeightScaled(NOTOSERIF_18_FONT_ID, 256) + 6;
  {
    char line[48];
    snprintf(line, sizeof(line), tr(STR_FLASH_REVIEWED), static_cast<int>(session.reviewed()));
    UITheme::drawCenteredText(renderer, area, UI_10_FONT_ID, y, line);
    y += 48;
    // Four ruled boxes: how often each rating was given.
    const int gap = 10;
    const int boxW = (a.w - 3 * gap) / 4;
    constexpr int BOX_H = 96;
    for (int i = 0; i < 4; i++) {
      const int bx = a.x + i * (boxW + gap);
      renderer.drawRoundedRect(bx, y, boxW, BOX_H, 2, 10, true);
      char n[8];
      snprintf(n, sizeof(n), "%u", session.ratingCount(static_cast<fsrs::Rating>(i + 1)));
      const int nw = renderer.getTextWidth(NOTOSERIF_18_FONT_ID, n, EpdFontFamily::BOLD);
      renderer.drawText(NOTOSERIF_18_FONT_ID, bx + (boxW - nw) / 2, y + 10, n, true, EpdFontFamily::BOLD);
      const int lw = renderer.getTextWidth(UI_10_FONT_ID, ratingName(i));
      renderer.drawText(UI_10_FONT_ID, bx + (boxW - lw) / 2, y + BOX_H - 30, ratingName(i));
    }
    y += BOX_H;
  }
  // The way out, as a full-width dark button.
  constexpr int BTN_H = 56;
  const int by = a.y + a.h - BTN_H - 8;
  renderer.fillRoundedRect(a.x, by, a.w, BTN_H, 12, Color::Black);
  const char* back = tr(STR_FLASHCARDS);
  const int bw = renderer.getTextWidth(UI_12_FONT_ID, back, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, cx - bw / 2, by + (BTN_H - renderer.getLineHeightScaled(UI_12_FONT_ID, 256)) / 2,
                    back, false, EpdFontFamily::BOLD);
}

void FlashcardReviewActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const Rect a = UITheme::getInstance().getScreenSafeArea(renderer, !mappedInput.hasTouch());
  const bool done = !opened || session.finished();
  if (done) {
    drawDone({a.x + 40, a.y + 30, a.width - 80, a.height - 60});
  } else {
    drawCard(cardRect());
    const Rect4 band = ratingsRect();
    if (showBack) {
      drawRatings(band);
    } else {
      char counter[24];
      snprintf(counter, sizeof(counter), "%d / %d", static_cast<int>(session.reviewed() + 1),
               static_cast<int>(session.reviewed() + session.remaining()));
      const Rect r{band.x, band.y, band.w, band.h};
      UITheme::drawCenteredText(renderer, r, SMALL_FONT_ID, band.y + 10, counter);
      const char* hint = mappedInput.hasTouch() ? tr(STR_FLASH_TAP_TO_TURN) : tr(STR_FLASH_TURN);
      UITheme::drawCenteredText(renderer, r, UI_10_FONT_ID, band.y + 34, hint);
    }
  }
  if (!mappedInput.hasTouch()) {
    const char* confirm = done ? "" : (showBack ? tr(STR_FLASH_RATE) : tr(STR_FLASH_TURN));
    const auto labels =
        mappedInput.mapLabels(tr(STR_BACK), confirm, showBack && !done ? "<" : "", showBack && !done ? ">" : "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer();
  if (!done && SETTINGS.textAntiAliasing) drawGrayPass();
}

// The card's text again into the two gray planes, as the reader does for a page. Only the card:
// the rating boxes draw white-on-black text, which would mark gray in a plane.
void FlashcardReviewActivity::drawGrayPass() {
  if (!renderer.storeBwBuffer()) {
    LOG_ERR("FLASH", "Gray pass skipped: no room to park the page");
    return;
  }
  for (int plane = 0; plane < 2; plane++) {
    renderer.clearScreen(0x00);
    renderer.setRenderMode(plane == 0 ? GfxRenderer::GRAYSCALE_LSB : GfxRenderer::GRAYSCALE_MSB);
    drawCard(cardRect());
    if (plane == 0) {
      renderer.copyGrayscaleLsbBuffers();
    } else {
      renderer.copyGrayscaleMsbBuffers();
    }
  }
  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.restoreBwBuffer();
}
