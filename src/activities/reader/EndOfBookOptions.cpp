#include "EndOfBookOptions.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <cmath>
#include <cstring>

#include "BookStats.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "ReadingStatsStore.h"
// ReaderUtils.h pulls in ActivityManager.h, which only forward-declares Activity while holding
// std::unique_ptr<Activity> members. Destroying that unique_ptr needs the complete type, so the
// definition must be visible here.
#include "activities/Activity.h"
#include "components/StatsWidgets.h"
#include "components/UITheme.h"
#include "components/icons/endOfBookCat.h"
#include "components/icons/eobIcons.h"
#include "components/icons/eobStamp.h"
#include "components/icons/tabBarIcons.h"
#include "fontIds.h"
#include "util/ButtonNavigator.h"
#include "util/CjkScript.h"
#include "util/NextBookFinder.h"

namespace {

// Display name without the file extension, mirroring the file browser rows
std::string displayName(const std::string& filename) {
  const auto pos = filename.rfind('.');
  return filename.substr(0, pos);
}

// "1st", "2nd", "3rd", "11th", "22nd" in English. Every other UI language gets the bare number:
// its milestone strings carry their own ordinal form around the %s ("%s.", "%se", "%s冊目").
void formatOrdinal(char* buf, const size_t size, const unsigned n) {
  if (I18N.getLanguage() != Language::EN) {
    snprintf(buf, size, "%u", n);
    return;
  }
  const unsigned lastTwo = n % 100;
  const char* suffix = "th";
  if (lastTwo < 11 || lastTwo > 13) {
    if (n % 10 == 1) suffix = "st";
    if (n % 10 == 2) suffix = "nd";
    if (n % 10 == 3) suffix = "rd";
  }
  snprintf(buf, size, "%u%s", n, suffix);
}

// "4h 32m", or "45m" under an hour.
void formatDuration(char* buf, const size_t size, const uint32_t minutes) {
  if (minutes >= 60) {
    snprintf(buf, size, "%luh %lum", static_cast<unsigned long>(minutes / 60),
             static_cast<unsigned long>(minutes % 60));
  } else {
    snprintf(buf, size, "%lum", static_cast<unsigned long>(minutes));
  }
}

// --- Ticket geometry -------------------------------------------------------------------------
// Each half is drawn upright, copied out, and written back tilted. A half's shape is analytic, so
// the tilted write paints exactly its own paper and never the other half under its torn edge.

constexpr int CORNER = 18;  // rounded corner radius
constexpr int TEAR = 6;     // depth of the torn zig-zag
constexpr int TEAR_PERIOD = 24;
constexpr int BORDER = 3;
// Degrees, clockwise on screen: the halves lean apart like a freshly torn ticket.
constexpr float TOP_ANGLE = 3.5f;
constexpr float STUB_ANGLE = -5.0f;
constexpr float STAMP_ANGLE = 14.0f;
constexpr int BUTTON_H = 80;
constexpr int MIN_TOP_W = 280;

enum class Shape { Top, Stub, Disc };

// Height of the torn zig-zag at x: 0..TEAR.
int tearAt(const int x) {
  const int t = ((x % TEAR_PERIOD) + TEAR_PERIOD) % TEAR_PERIOD;
  return std::abs(t - TEAR_PERIOD / 2) * TEAR * 2 / TEAR_PERIOD;
}

bool outsideCorner(const int x, const int y, const int cx, const int cy) {
  const int dx = x - cx;
  const int dy = y - cy;
  return dx * dx + dy * dy > CORNER * CORNER;
}

// Whether local (x, y) is paper of a w x h piece: the top half has round top corners and a torn
// bottom edge, the stub a torn top edge and round bottom corners.
bool inShape(const Shape shape, const int w, const int h, const int x, const int y) {
  if (x < 0 || y < 0 || x >= w || y >= h) return false;
  if (shape == Shape::Disc) {
    const int r = w / 2;
    return (x - r) * (x - r) + (y - r) * (y - r) <= r * r;
  }
  if (shape == Shape::Top) {
    if (y > h - 1 - TEAR + tearAt(x)) return false;
    if (y < CORNER && x < CORNER && outsideCorner(x, y, CORNER, CORNER)) return false;
    if (y < CORNER && x >= w - CORNER && outsideCorner(x, y, w - 1 - CORNER, CORNER)) return false;
    return true;
  }
  if (y < tearAt(x)) return false;
  if (y >= h - CORNER && x < CORNER && outsideCorner(x, y, CORNER, h - 1 - CORNER)) return false;
  if (y >= h - CORNER && x >= w - CORNER && outsideCorner(x, y, w - 1 - CORNER, h - 1 - CORNER)) return false;
  return true;
}

// Paper whose neighbourhood leaves the shape: the piece's outline.
bool onBorder(const Shape shape, const int w, const int h, const int x, const int y) {
  if (!inShape(shape, w, h, x, y)) return false;
  return !inShape(shape, w, h, x - BORDER, y) || !inShape(shape, w, h, x + BORDER, y) ||
         !inShape(shape, w, h, x, y - BORDER) || !inShape(shape, w, h, x, y + BORDER) ||
         !inShape(shape, w, h, x - 2, y - 2) || !inShape(shape, w, h, x + 2, y - 2) ||
         !inShape(shape, w, h, x - 2, y + 2) || !inShape(shape, w, h, x + 2, y + 2);
}

// Only a band CORNER + BORDER + TEAR deep along each side can hold outline, so the middle of a row
// is skipped instead of testing ~100k interior pixels against the shape nine times each.
void drawOutline(const GfxRenderer& renderer, const Shape shape, const int x0, const int y0, const int w, const int h) {
  constexpr int M = CORNER + BORDER + TEAR;
  for (int y = 0; y < h; y++) {
    const bool edgeRow = y < M || y >= h - M;
    for (int x = 0; x < w; x++) {
      if (!edgeRow && x == M && w - M > M) x = w - M;
      if (onBorder(shape, w, h, x, y)) renderer.drawPixel(x0 + x, y0 + y, true);
    }
  }
}

// A drawn region copied out of the framebuffer as 1 bpp ink, so it can be written back tilted.
struct Capture {
  int w = 0;
  int h = 0;
  std::unique_ptr<uint8_t[]> ink;
  bool at(const int x, const int y) const {
    if (x < 0 || y < 0 || x >= w || y >= h) return false;
    const int i = y * w + x;
    return (ink[i >> 3] >> (7 - (i & 7))) & 1;
  }
};

// Copies the region out and clears it. False when the buffer cannot be had: the caller then leaves
// the piece where it was drawn, upright.
bool capture(const GfxRenderer& renderer, const int x0, const int y0, const int w, const int h, Capture& out) {
  const size_t bytes = (static_cast<size_t>(w) * h + 7) / 8;
  out.ink = makeUniqueNoThrow<uint8_t[]>(bytes);
  if (!out.ink) {
    LOG_ERR("EOB", "OOM: %u bytes for a ticket piece, drawn upright", static_cast<unsigned>(bytes));
    return false;
  }
  memset(out.ink.get(), 0, bytes);
  out.w = w;
  out.h = h;
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      if (renderer.readPixel(x0 + x, y0 + y)) {
        const int i = y * w + x;
        out.ink[i >> 3] |= 0x80 >> (i & 7);
      }
    }
  }
  renderer.fillRect(x0, y0, w, h, false);
  return true;
}

// A rotation in Q16 fixed point. The C3 has no FPU, so the per-pixel work below stays in integers;
// the one sin/cos per piece is the only floating point.
struct Turn {
  int32_t c;
  int32_t s;
};

Turn turnFor(const float degrees) {
  const float rad = degrees * 3.14159265f / 180.0f;
  return {static_cast<int32_t>(std::lround(std::cos(rad) * 65536.0f)),
          static_cast<int32_t>(std::lround(std::sin(rad) * 65536.0f))};
}

// Writes a captured piece back turned about its centre, which lands at (cx, cy). Each screen pixel
// samples the piece 3x3: inside the shape for a majority it becomes paper, and ink once a third of
// the paper samples are ink, so 1 px strokes survive the turn. Only the turned piece's bounding box
// is visited, and the source position steps along each row by addition.
void writeTilted(const GfxRenderer& renderer, const Capture& piece, const Shape shape, const Turn t, const int cx,
                 const int cy) {
  const int w = piece.w;
  const int h = piece.h;
  const int32_t ac = std::abs(t.c);
  const int32_t as = std::abs(t.s);
  const int halfX = static_cast<int>((static_cast<int64_t>(ac) * w + static_cast<int64_t>(as) * h) >> 17) + 2;
  const int halfY = static_cast<int>((static_cast<int64_t>(as) * w + static_cast<int64_t>(ac) * h) >> 17) + 2;
  // The nine sub-pixel offsets, already turned into source space.
  int32_t du[9];
  int32_t dv[9];
  for (int k = 0; k < 9; k++) {
    const int64_t ox = (k % 3 - 1) * 65536 / 3;
    const int64_t oy = (k / 3 - 1) * 65536 / 3;
    du[k] = static_cast<int32_t>((t.c * ox + t.s * oy) >> 16);
    dv[k] = static_cast<int32_t>((-t.s * ox + t.c * oy) >> 16);
  }
  const int32_t hw = w << 15;
  const int32_t hh = h << 15;
  for (int dy = -halfY; dy <= halfY; dy++) {
    // Source of the row's first pixel (dx = -halfX); each step right adds (c, -s).
    int32_t u = t.s * dy + hw - t.c * halfX;
    int32_t v = t.c * dy + hh + t.s * halfX;
    for (int dx = -halfX; dx <= halfX; dx++, u += t.c, v -= t.s) {
      const int ux = u >> 16;
      const int uy = v >> 16;
      if (ux < -1 || uy < -1 || ux > w || uy > h) continue;
      int paper = 0;
      int ink = 0;
      for (int k = 0; k < 9; k++) {
        const int sx = (u + du[k]) >> 16;
        const int sy = (v + dv[k]) >> 16;
        if (!inShape(shape, w, h, sx, sy)) continue;
        paper++;
        if (piece.at(sx, sy)) ink++;
      }
      if (paper < 5) continue;
      renderer.drawPixel(cx + dx, cy + dy, ink * 3 >= paper);
    }
  }
}

// The stamp is ink only: it is written tilted with a 2 px white halo around its strokes, so it
// stays legible over a busy cover without blanking the cover under its open middle.
void writeStamp(const GfxRenderer& renderer, const Capture& stamp, const Turn t, const int cx, const int cy) {
  const int size = stamp.w;
  const int reach = size * 71 / 100 + 3;  // half the diagonal, and the halo
  const int side = reach * 2 + 1;
  Capture turned;
  turned.w = side;
  turned.h = side;
  const size_t bytes = (static_cast<size_t>(side) * side + 7) / 8;
  turned.ink = makeUniqueNoThrow<uint8_t[]>(bytes);
  if (!turned.ink) {
    LOG_ERR("EOB", "OOM: %u bytes for the stamp, skipped", static_cast<unsigned>(bytes));
    return;
  }
  memset(turned.ink.get(), 0, bytes);
  const int32_t half = size << 15;
  for (int dy = -reach; dy <= reach; dy++) {
    int32_t u = t.s * dy + half - t.c * reach;
    int32_t v = t.c * dy + half + t.s * reach;
    for (int dx = -reach; dx <= reach; dx++, u += t.c, v -= t.s) {
      if (stamp.at(u >> 16, v >> 16)) {
        const int i = (dy + reach) * side + (dx + reach);
        turned.ink[i >> 3] |= 0x80 >> (i & 7);
      }
    }
  }
  // Halo first, around ink only; then the ink over it.
  const int x0 = cx - reach;
  const int y0 = cy - reach;
  for (int y = 0; y < side; y++) {
    for (int x = 0; x < side; x++) {
      if (!turned.at(x, y)) continue;
      for (int oy = -2; oy <= 2; oy++) {
        for (int ox = -2; ox <= 2; ox++) {
          if (!turned.at(x + ox, y + oy)) renderer.drawPixel(x0 + x + ox, y0 + y + oy, false);
        }
      }
    }
  }
  for (int y = 0; y < side; y++) {
    for (int x = 0; x < side; x++) {
      if (turned.at(x, y)) renderer.drawPixel(x0 + x, y0 + y, true);
    }
  }
}

// Turns local point (x, y) of a w x h piece by `degrees` about its centre, placed at (cx, cy).
void turnPoint(const float x, const float y, const int w, const int h, const float degrees, const int cx, const int cy,
               int& outX, int& outY) {
  const float rad = degrees * 3.14159265f / 180.0f;
  const float px = x - w / 2.0f;
  const float py = y - h / 2.0f;
  outX = cx + static_cast<int>(std::lround(std::cos(rad) * px - std::sin(rad) * py));
  outY = cy + static_cast<int>(std::lround(std::sin(rad) * px + std::cos(rad) * py));
}

// The stamp's wording for the book's language: 読了 for Japanese and every non-Chinese book, 读完
// (simplified) and 讀完 (traditional) for Chinese, 睇完 for Cantonese.
const uint8_t* stampFor(const char* language) {
  const std::string_view tag = language ? language : "";
  const std::string_view primary = tag.substr(0, tag.find_first_of("-_"));
  if (cjk::subtagIs(primary, "yue")) return EobStampYue;
  switch (cjk::scriptForLanguage(tag)) {
    case CjkScript::SimplifiedChinese:
      return EobStampZhHans;
    case CjkScript::TraditionalChinese:
      return EobStampZhHant;
    default:
      return EobStampJa;
  }
}

// Draws a run-length coded stamp (see eobStamp.h) upright at (x, y).
void drawStamp(const GfxRenderer& renderer, const uint8_t* rle, const int x, const int y) {
  int i = 0;
  for (size_t b = 0; i < EOB_STAMP_SIZE * EOB_STAMP_SIZE; b++) {
    const bool ink = rle[b] & 0x80;
    const int run = (rle[b] & 0x7F) + 1;
    for (int k = 0; k < run; k++, i++) {
      if (ink) renderer.drawPixel(x + i % EOB_STAMP_SIZE, y + i / EOB_STAMP_SIZE, true);
    }
  }
}

// A 32 px icon in white, for a selected (black) button. drawIcon only draws ink.
void drawIconWhite(const GfxRenderer& renderer, const uint8_t* bitmap, const int x, const int y, const int size) {
  const int rowBytes = (size + 7) / 8;
  for (int row = 0; row < size; row++) {
    for (int col = 0; col < size; col++) {
      if (((bitmap[row * rowBytes + (col >> 3)] >> (7 - (col & 7))) & 1) == 0) {
        renderer.drawPixel(x + (size - 1 - row), y + col, false);
      }
    }
  }
}

}  // namespace

EndOfBookOptions::EndOfBookOptions(GfxRenderer& renderer) : renderer(renderer) {}

void EndOfBookOptions::loadSummary(const std::string& bookPath, const char* bookLanguage) {
  ReadingStatsStore::Change update;
  update.finishedPath = bookPath.c_str();
  READING_STATS_STORE.record(update);

  BookStats stats;
  if (stats.load(bookPath.c_str())) {
    summary.minutes = stats.getTotalMinutes();
    summary.daySpan = stats.getDaySpan();
    summary.lookups = stats.getLookups();
    summary.sentences = stats.getSentencesSaved();
  }
  const StatsWidgets::Today today = StatsWidgets::getToday();
  // Streamed from the file: the reader is still in memory, so no tables are built here.
  const ReadingStatsStore::Summary totals =
      READING_STATS_STORE.summarize(today.year, today.month, today.day, bookLanguage);
  summary.streak = totals.streak;
  summary.booksFinished = totals.booksFinished;
  stamp_ = stampFor(bookLanguage);
  snprintf(summary.finishedOn, sizeof(summary.finishedOn), "%04u.%02u.%02u", static_cast<unsigned>(today.year),
           static_cast<unsigned>(today.month), static_cast<unsigned>(today.day));

  // Primary subtag, lowercased, as the stats store buckets it ("ja-JP" -> "ja").
  char code[4] = {};
  for (size_t i = 0; bookLanguage && i < 3 && bookLanguage[i] && bookLanguage[i] != '-' && bookLanguage[i] != '_';
       i++) {
    const char c = bookLanguage[i];
    code[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
  }
  if (code[0]) {
    // The per-language count only sees books with reading time on record; the one just finished is
    // in this language whatever its history, so it is at least the first.
    summary.languageBooksFinished = std::max<uint16_t>(totals.languageBooksFinished, 1);
    if (const char* name = I18n::languageNameForCode(code)) summary.languageName = name;
  }
}

void EndOfBookOptions::loadOnce(const std::string& currentBookPath, const std::string& bookTitle,
                                const char* bookLanguage, const std::string& coverThumbPath) {
  if (isLoaded.load(std::memory_order_acquire)) {
    return;
  }
  loadSummary(currentBookPath, bookLanguage);
  folder = FsHelpers::extractFolderPath(currentBookPath);
  coverPath = coverThumbPath;
  if (!bookTitle.empty()) {
    title = bookTitle;
  } else {
    const auto slash = currentBookPath.find_last_of('/');
    title = displayName(slash == std::string::npos ? currentBookPath : currentBookPath.substr(slash + 1));
  }
  const std::vector<std::string> next = NextBookFinder::findNextBooks(currentBookPath, 1);
  if (!next.empty()) {
    nextName = next.front();
    // The constructor only derives the cache paths; nothing is parsed or generated.
    if (FsHelpers::hasReflowableBookExtension(nextName)) {
      if (auto epub = makeUniqueNoThrow<Epub>(nextPath(), "/.crosspoint")) nextCoverTemplate = epub->getThumbBmpPath();
    }
  }
  // Focus starts on Home; Continue is one press away.
  selector.store(itemCount() - 1, std::memory_order_relaxed);
  // Release-publish so the main task, which gates all access on isLoaded, never observes a
  // partially built screen.
  isLoaded.store(true, std::memory_order_release);
}

bool EndOfBookOptions::menuActive() const { return isLoaded.load(std::memory_order_acquire); }

std::string EndOfBookOptions::nextPath() const {
  if (nextName.empty()) return {};
  return folder == "/" ? "/" + nextName : folder + "/" + nextName;
}

EndOfBookOptions::Action EndOfBookOptions::handleMenuInput(const MappedInputManager& input, std::string* openPath) {
  const auto activate = [&](const int index) {
    if (isHomeIndex(index)) return Action::GoHome;
    if (openPath) *openPath = nextPath();
    return Action::OpenBook;
  };

  int tapX = 0;
  int tapY = 0;
  if (input.hasTouch() && input.wasScreenTapped(tapX, tapY)) {
    if (!nextName.empty() && continueRect.contains(tapX, tapY)) return activate(0);
    if (homeRect.contains(tapX, tapY)) return Action::GoHome;
  }

  const int selectedIndex = selector.load(std::memory_order_relaxed);
  if (input.wasReleased(MappedInputManager::Button::Confirm)) {
    return activate(selectedIndex);
  }

  // Short-press Back returns to the last page; a long press falls through to the
  // reader's own handler (file browser).
  if (input.wasReleased(MappedInputManager::Button::Back) && input.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    return Action::LastPage;
  }

  // Selection movement follows the reader's page-turn semantics (press-triggered by default,
  // release-triggered when a long-press behavior is configured, same rule as
  // ReaderUtils::detectPageTurn). This matters on entry: with press-triggered turns, the press
  // that turned the final page already fired in the reader, and its release must not double-fire.
  const bool usePress = SETTINGS.longPressButtonBehavior == CrossPointSettings::OFF;
  const auto triggered = [&](const MappedInputManager::Button button) {
    return usePress ? input.wasPressed(button) : input.wasReleased(button);
  };
  if (triggered(MappedInputManager::Button::NavPrevious)) {
    selector.store(ButtonNavigator::previousIndex(selectedIndex, itemCount()), std::memory_order_relaxed);
    selectionMoved.store(true, std::memory_order_release);
    return Action::Redraw;
  }
  if (triggered(MappedInputManager::Button::NavNext)) {
    selector.store(ButtonNavigator::nextIndex(selectedIndex, itemCount()), std::memory_order_relaxed);
    selectionMoved.store(true, std::memory_order_release);
    return Action::Redraw;
  }
  return Action::None;
}

int EndOfBookOptions::stubHeight() const {
  const int small = renderer.getLineHeight(SMALL_FONT_ID);
  int h = TEAR + 10 + (summary.booksFinished > 0 ? small + 8 : 0) + renderer.getLineHeight(UI_12_FONT_ID) + small + 14;
  if (summary.lookups > 0 || summary.sentences > 0) h += 9 + 2 * renderer.getLineHeight(UI_10_FONT_ID) + 6;
  return h + CORNER / 2;
}

// The stub: the read in numbers. "No. 14" in the corner, the three headline stats, then the
// dictionary counts and the language milestone.
void EndOfBookOptions::drawStub(const int x, const int y, const int w, const int h) const {
  drawOutline(renderer, Shape::Stub, x, y, w, h);
  const int pad = 18;

  // Milestone across the top: "Your 14th book · 6th in 日本語".
  if (summary.booksFinished > 0) {
    char nth[16];
    formatOrdinal(nth, sizeof(nth), summary.booksFinished);
    char line[96];
    snprintf(line, sizeof(line), tr(STR_EOB_NTH_BOOK), nth);
    if (summary.languageBooksFinished > 0 && !summary.languageName.empty()) {
      char nthLang[16];
      formatOrdinal(nthLang, sizeof(nthLang), summary.languageBooksFinished);
      const size_t used = strlen(line);
      snprintf(line + used, sizeof(line) - used, tr(STR_EOB_NTH_IN_LANGUAGE), nthLang, summary.languageName.c_str());
    }
    const std::string shown = renderer.truncatedText(SMALL_FONT_ID, line, w - 2 * pad);
    renderer.drawText(SMALL_FONT_ID, x + pad, y + TEAR + 10, shown.c_str(), true);
  }

  struct Cell {
    char value[16];
    const char* label;
  };
  Cell cells[3];
  int count = 0;
  formatDuration(cells[count].value, sizeof(cells[0].value), summary.minutes);
  cells[count++].label = tr(STR_EOB_READING_TIME);
  // The three headline stats always show, so the stub keeps its shape after a short read. A book
  // finished the day it was started still took a day.
  snprintf(cells[count].value, sizeof(cells[0].value), "%d", std::max(summary.daySpan, 1));
  cells[count++].label = tr(STR_EOB_DAYS_TO_FINISH);
  // Finishing the book today is reading today, so the streak is at least one day.
  snprintf(cells[count].value, sizeof(cells[0].value), "%d", std::max(summary.streak, 1));
  cells[count++].label = tr(STR_EOB_DAY_STREAK);
  const int valueY = y + TEAR + 10 + (summary.booksFinished > 0 ? renderer.getLineHeight(SMALL_FONT_ID) + 8 : 0);
  const int valueH = renderer.getLineHeight(UI_12_FONT_ID);
  const int colW = (w - 2 * pad) / count;
  for (int i = 0; i < count; i++) {
    const int cx = x + pad + colW * i + colW / 2;
    const int vw = renderer.getTextWidth(UI_12_FONT_ID, cells[i].value, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, cx - vw / 2, valueY, cells[i].value, true, EpdFontFamily::BOLD);
    const std::string label = renderer.truncatedText(SMALL_FONT_ID, cells[i].label, colW - 6);
    const int lw = renderer.getTextWidth(SMALL_FONT_ID, label.c_str());
    renderer.drawText(SMALL_FONT_ID, cx - lw / 2, valueY + valueH, label.c_str(), true);
  }

  if (summary.lookups == 0 && summary.sentences == 0) return;
  const int ruleY = valueY + valueH + renderer.getLineHeight(SMALL_FONT_ID) + 10;
  renderer.drawLine(x + pad, ruleY, x + w - pad, ruleY, true);
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  char line[64];
  snprintf(line, sizeof(line), "%lu %s", static_cast<unsigned long>(summary.lookups), tr(STR_STAT_WORDS_LOOKED_UP));
  renderer.drawText(UI_10_FONT_ID, x + pad, ruleY + 8, line, true);
  snprintf(line, sizeof(line), "%lu %s", static_cast<unsigned long>(summary.sentences), tr(STR_STAT_SENTENCES_SAVED));
  renderer.drawText(UI_10_FONT_ID, x + pad, ruleY + 8 + lineH, line, true);
}

void EndOfBookOptions::drawTicket(const Rect& safe, const int bottom) const {
  const int lineH = renderer.getLineHeight(UI_12_FONT_ID);
  const int stubH = stubHeight();
  const int stubW = std::min(380, safe.width - 40);

  // The cover fills what is left once the stub, the title and the tilt's overhang are placed.
  // Without a cover the cat stands in, at its own size.
  int coverW = END_OF_BOOK_CAT_SIZE;
  int coverH = END_OF_BOOK_CAT_SIZE;
  int srcW = 0;
  int srcH = 0;
  const bool hasCover = !coverPath.empty() && UITheme::getCoverThumbSize(coverPath, &srcW, &srcH);
  // Wrapped to the narrowest the top half gets, so the lines fit whatever the cover's width.
  const auto titleLines = renderer.wrappedText(UI_12_FONT_ID, title.c_str(), MIN_TOP_W - 24, 2, EpdFontFamily::BOLD);
  const int titleH = lineH * static_cast<int>(std::max<size_t>(1, titleLines.size()));
  const int topChrome = 12 + 8 + titleH + 10 + TEAR;
  constexpr int TEAR_GAP = 16;  // the halves part a little at the tear
  const int maxCoverH = bottom - safe.y - 8 - topChrome - TEAR_GAP - stubH - 28;
  if (hasCover) {
    coverH = std::min(maxCoverH, COVER_HEIGHT);
    coverW = srcW * coverH / srcH;
    const int maxCoverW = safe.width - 90;
    if (coverW > maxCoverW) {
      coverW = maxCoverW;
      coverH = srcH * coverW / srcW;
    }
  }
  const int topW = std::max(coverW + 24, MIN_TOP_W);
  const int topH = 12 + coverH + 8 + titleH + 10 + TEAR;
  // The two halves as one group, centred in the space above the buttons.
  const int groupH = topH + TEAR_GAP + stubH;
  const int topY = safe.y + std::max(8, (bottom - safe.y - groupH) / 2);
  const int topX = safe.x + (safe.width - topW) / 2 - 12;
  const int stubX = safe.x + (safe.width - stubW) / 2 + 14;
  const int stubY = topY + topH + TEAR_GAP;
  const int coverX = topX + (topW - coverW) / 2;
  const int coverY = topY + 12;

  // The stamp first, in a corner of the still blank screen: it is captured and put back last,
  // on top of the cover.
  Capture stamp;
  drawStamp(renderer, stamp_, safe.x, safe.y);
  // The date in the space under 読了, kept inside the inner ring.
  const int dateW = renderer.getTextWidth(SMALL_FONT_ID, summary.finishedOn, EpdFontFamily::BOLD);
  renderer.drawText(SMALL_FONT_ID, safe.x + (EOB_STAMP_SIZE - dateW) / 2, safe.y + EOB_STAMP_SIZE * 60 / 100,
                    summary.finishedOn, true, EpdFontFamily::BOLD);
  const bool stampCaptured = capture(renderer, safe.x, safe.y, EOB_STAMP_SIZE, EOB_STAMP_SIZE, stamp);

  // Top half: the cover (or the cat when there is none) and the title.
  drawOutline(renderer, Shape::Top, topX, topY, topW, topH);
  if (hasCover) {
    UITheme::drawCoverThumb(renderer, coverPath, coverX, coverY, coverH);
    renderer.drawRect(coverX, coverY, coverW, coverH, true);
  } else {
    renderer.drawIcon(EndOfBookCat, coverX, coverY, END_OF_BOOK_CAT_SIZE);
  }
  int titleY = coverY + coverH + 8;
  for (const auto& line : titleLines) {
    const int titleW = renderer.getTextWidth(UI_12_FONT_ID, line.c_str(), EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, topX + (topW - titleW) / 2, titleY, line.c_str(), true, EpdFontFamily::BOLD);
    titleY += lineH;
  }
  Capture top;
  const bool topCaptured = capture(renderer, topX, topY, topW, topH, top);

  drawStub(stubX, stubY, stubW, stubH);
  Capture stub;
  const bool stubCaptured = capture(renderer, stubX, stubY, stubW, stubH, stub);

  const int topCx = topX + topW / 2;
  const int topCy = topY + topH / 2;
  if (topCaptured) writeTilted(renderer, top, Shape::Top, turnFor(TOP_ANGLE), topCx, topCy);
  if (stubCaptured) {
    writeTilted(renderer, stub, Shape::Stub, turnFor(STUB_ANGLE), stubX + stubW / 2, stubY + stubH / 2);
  }

  // Pressed on the cover's lower right corner, wherever the tilt carried it.
  if (stampCaptured) {
    int sx = 0;
    int sy = 0;
    turnPoint(coverX - topX + coverW - 40, coverY - topY + coverH - EOB_STAMP_SIZE / 2, topW, topH,
              topCaptured ? TOP_ANGLE : 0, topCx, topCy, sx, sy);
    // Hanging off the cover's right edge, but never off the screen.
    const int stampCx = std::min(sx + 52, safe.x + safe.width - EOB_STAMP_SIZE / 2 - 4);
    writeStamp(renderer, stamp, turnFor(STAMP_ANGLE), stampCx, sy);
  }
}

void EndOfBookOptions::drawButtons(const int x, const int y, const int w, const int h, const int selected) const {
  constexpr int RADIUS = 10;
  constexpr int ICON = 32;
  const bool homeSelected = isHomeIndex(selected);
  const int homeW = nextName.empty() ? w : h;  // a square beside Continue
  homeRect = {x, y, homeW, h};
  if (homeSelected) {
    renderer.fillRoundedRect(x, y, homeW, h, RADIUS, Color::Black);
  } else {
    renderer.drawRoundedRect(x, y, homeW, h, 2, RADIUS, true);
  }
  int iconX = x + (homeW - ICON) / 2;
  if (nextName.empty()) {
    // Home alone: the house and its label, centred together.
    const int labelW = renderer.getTextWidth(UI_12_FONT_ID, tr(STR_EOB_HOME));
    iconX = x + (homeW - ICON - 12 - labelW) / 2;
    const int labelY = y + (h - renderer.getLineHeight(UI_12_FONT_ID)) / 2;
    renderer.drawText(UI_12_FONT_ID, iconX + ICON + 12, labelY, tr(STR_EOB_HOME), !homeSelected);
  }
  if (homeSelected) {
    drawIconWhite(renderer, HomeIcon, iconX, y + (h - ICON) / 2, ICON);
  } else {
    renderer.drawIcon(HomeIcon, iconX, y + (h - ICON) / 2, ICON);
  }
  if (nextName.empty()) {
    continueRect = {};
    return;
  }

  const int cx = x + homeW + 12;
  const int cw = w - homeW - 12;
  continueRect = {cx, y, cw, h};
  const bool sel = !homeSelected;
  if (sel) {
    renderer.fillRoundedRect(cx, y, cw, h, RADIUS, Color::Black);
  } else {
    renderer.drawRoundedRect(cx, y, cw, h, 2, RADIUS, true);
  }
  int textX = cx + 16;
  if (!nextCoverTemplate.empty()) {
    const int thumbH = h - 14;
    const std::string thumb = UITheme::getCoverThumbPath(nextCoverTemplate, thumbH);
    int thumbW = 0;
    int thumbSrcH = 0;
    if (UITheme::getCoverThumbSize(thumb, &thumbW, &thumbSrcH) && thumbSrcH > 0) {
      // White behind it: on the selected (black) button the cover's own ink would vanish.
      const int thumbDrawW = thumbW * thumbH / thumbSrcH;
      renderer.fillRect(cx + 10, y + 7, thumbDrawW, thumbH, false);
      const int drawn = UITheme::drawCoverThumb(renderer, thumb, cx + 10, y + 7, thumbH);
      if (drawn > 0) textX = cx + 10 + drawn + 14;
    }
  }
  constexpr int CHEVRON = 32;
  const int arrowW = CHEVRON;
  const int smallH = renderer.getLineHeight(SMALL_FONT_ID);
  const int bigH = renderer.getLineHeight(UI_12_FONT_ID);
  const int textTop = y + (h - smallH - bigH) / 2;
  renderer.drawText(SMALL_FONT_ID, textX, textTop, tr(STR_EOB_CONTINUE_WITH), !sel);
  const std::string name = renderer.truncatedText(UI_12_FONT_ID, displayName(nextName).c_str(),
                                                  cx + cw - 16 - arrowW - 10 - textX, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, textX, textTop + smallH, name.c_str(), !sel, EpdFontFamily::BOLD);
  const int chevronX = cx + cw - 10 - CHEVRON;
  const int chevronY = y + (h - CHEVRON) / 2;
  if (sel) {
    drawIconWhite(renderer, ChevronRightIcon, chevronX, chevronY, CHEVRON);
  } else {
    renderer.drawIcon(ChevronRightIcon, chevronX, chevronY, CHEVRON);
  }
}

void EndOfBookOptions::render(GfxRenderer& renderer, const MappedInputManager& input) {
  if (!menuActive()) {
    renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() * 3 / 8, tr(STR_END_OF_BOOK), true,
                              EpdFontFamily::BOLD);
    return;
  }

  // The hints are drawn at the physical front buttons, which is a logical side/top edge in
  // the rotated orientations: lay out inside the safe area so nothing hides behind them.
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  buttonsX = safe.x + 16;
  buttonsY = safe.y + safe.height - BUTTON_H - 10;
  buttonsW = safe.width - 32;
  // A full render supersedes any pending selection-only repaint.
  selectionMoved.store(false, std::memory_order_relaxed);
  const unsigned long started = millis();
  drawTicket(safe, buttonsY - 12);
  LOG_DBG("EOB", "Ticket drawn in %lu ms", millis() - started);
  drawButtons(buttonsX, buttonsY, buttonsW, BUTTON_H, selector.load(std::memory_order_relaxed));
  frameDrawn = true;

  const auto labels = input.mapLabels(tr(STR_BACK), tr(STR_OPEN), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

bool EndOfBookOptions::repaintSelection(const GfxRenderer& renderer) {
  if (!frameDrawn || !selectionMoved.exchange(false, std::memory_order_acquire)) return false;
  // The tilted ticket is the expensive part (a capture and a turned rewrite per half); a selection
  // move changes only the button row, so that is all that gets cleared and drawn again.
  renderer.fillRect(buttonsX, buttonsY, buttonsW, BUTTON_H, false);
  drawButtons(buttonsX, buttonsY, buttonsW, BUTTON_H, selector.load(std::memory_order_relaxed));
  return true;
}
