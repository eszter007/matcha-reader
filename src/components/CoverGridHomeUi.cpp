#include "CoverGridHomeUi.h"

#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <utility>

#include "HomeTabBar.h"
#include "MappedInputManager.h"
#include "UITheme.h"
#include "icons/book.h"
#include "util/BookProgress.h"

namespace fui = freeink::ui;
namespace {
constexpr fui::ActionId SELECT = 1;
// Grid cell padding around each cover; also feeds the screen's horizontal
// inset so the cover columns land on the header chrome's inset line.
constexpr int16_t COVER_CELL_INSET = 6;
constexpr int COVER_ROW_EXTRA_HEIGHT = 8;
}  // namespace

CoverGridHomeUi::CoverGridHomeUi(GfxRenderer& renderer)
    : UiAppHost(renderer), coverCache(renderer), renderer(renderer) {}

void CoverGridHomeUi::begin(const std::vector<RecentBook>& recent, bool opds, bool continuing) {
  books = &recent;
  hasOpds = opds;
  hasContinueReading = continuing;
  if (!recent.empty()) coverCache.begin();
  resetUi();
  app.on(SELECT, &CoverGridHomeUi::onAction, this);
  app.setScreen(&CoverGridHomeUi::screenFn, this);
  refreshCoverPaths();
  // One small read per visible book so every cover can carry its badge, the same bounded cost
  // the Library grid pays for its own page (<= MAX_BOOKS entries).
  bookProgress.fill(-1);
  for (size_t i = 0; i < books->size() && i < bookProgress.size(); ++i) {
    bookProgress[i] = loadBookProgress((*books)[i].path);
  }
  progress = hasContinueReading && !books->empty() ? bookProgress[0] : -1;
  if (progress >= 0) snprintf(progressText, sizeof(progressText), "%d%%", progress);
}

void CoverGridHomeUi::refreshCoverPaths() {
  coverCache.invalidate();
  for (size_t i = 0; i < books->size() && i < coverPaths.size(); ++i) refreshCoverPath(i);
}

void CoverGridHomeUi::refreshCoverPath(size_t index) {
  if (index >= books->size() || index >= coverPaths.size()) return;
  coverCache.invalidate(index);
  coverPaths[index] =
      thumbHeight > 0 ? UITheme::getCoverThumbPath((*books)[index].coverBmpPath, thumbHeight) : std::string();
}

void CoverGridHomeUi::refreshProgress(const size_t index) {
  if (!books || index >= books->size() || index >= bookProgress.size()) return;
  bookProgress[index] = loadBookProgress((*books)[index].path);
  // The featured card shows the same number as a percentage under the title.
  if (index == 0 && hasContinueReading) {
    progress = bookProgress[0];
    if (progress >= 0) snprintf(progressText, sizeof(progressText), "%d%%", progress);
  }
  coverCache.invalidate(index);
}

int CoverGridHomeUi::thumbHeightFor() const { return thumbHeight > 0 ? thumbHeight : THUMB_HEIGHT; }

bool CoverGridHomeUi::takeThumbHeightChanged() { return std::exchange(thumbHeightChanged, false); }

void CoverGridHomeUi::noteThumbHeight(int slotHeight) {
  // One shared height for every slot (hero and grid covers are the same size), and exactly the
  // slot height: a thumb generated larger is centred and clipped by the paint, which ate the
  // author line off the bottom of covers that have one.
  const int height = std::max(1, slotHeight);
  if (thumbHeight != height) {
    thumbHeight = height;
    thumbHeightChanged = true;
    refreshCoverPaths();
  }
}

void CoverGridHomeUi::onAction(const fui::ActionEvent& event, void* user) {
  auto& self = *static_cast<CoverGridHomeUi*>(user);
  if (event.longPress) {
    self.pendingLongPress = event.value;
  } else {
    self.pending = event.value;
  }
  self.app.clearTapFlash();
}

int CoverGridHomeUi::takeLongPressedBook() {
  const int book = pendingLongPress;
  pendingLongPress = -1;
  return book >= 0 && books && book < static_cast<int>(books->size()) ? book : -1;
}

int CoverGridHomeUi::selectedAction(const MappedInputManager& input) {
  pending = -1;
  // The tab band is chrome, not part of the FreeInkUI screen, so it is hit-tested before the
  // grid: routeTouch would otherwise consume the tap as a miss on the covers.
  bool tapped = false;
  const int tab = HomeTabBar::hitTest(input, renderer, tapped);
  if (tab >= 0) {
    if (!tapped) return -1;
    return static_cast<int>(books->size()) + tab;
  }
  // withLongPress: a long press on a cover opens its stats, the same gesture the Library grid
  // uses. The SDK suppresses the rest of the contact, so the lift cannot also open the book.
  const auto touch = routeTouch(input, /*withLongPress=*/true);
  // bookCard registers its hit as InputDefault only, so the featured cover cannot carry a long
  // press through the component; test it against the rect its painter reported instead.
  if (touch.snap.longPress && pendingLongPress < 0 && heroCoverRect.width > 0 && touch.snap.touchX >= heroCoverRect.x &&
      touch.snap.touchX < heroCoverRect.right() && touch.snap.touchY >= heroCoverRect.y &&
      touch.snap.touchY < heroCoverRect.bottom()) {
    pendingLongPress = 0;
    return -1;
  }
  return touch.snap.touchReleased && !touch.snap.longPress ? pending : -1;
}

void CoverGridHomeUi::screenFn(UiScreen& screen, void* user) { static_cast<CoverGridHomeUi*>(user)->draw(screen); }

void CoverGridHomeUi::draw(UiScreen& screen) {
  coverCache.prepare();
  const auto& theme = screen.theme();
  const auto safe = UITheme::getInstance().getScreenSafeArea(renderer, true);
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y), static_cast<int16_t>(renderer.getScreenWidth() - safe.x - safe.width),
      static_cast<int16_t>(renderer.getScreenHeight() - safe.y - safe.height), static_cast<int16_t>(safe.x)});
  // Horizontal inset sized so the outer cover columns (content x plus the
  // grid's cell inset) sit on the theme's side-padding line: the heading and
  // tabs then align with the clock and battery, which tuck a few px further
  // in (headerStatusInset's optical bias).
  const int16_t hInset = std::max<int16_t>(
      0, static_cast<int16_t>(UITheme::getInstance().getMetrics().headerSidePadding - COVER_CELL_INSET - safe.x));
  // Drop the status band, hero, and grid down: touch boards get a full step
  // (no button-hint band to fit), button boards a modest nudge. The tabs stay
  // put (bottom-anchored below).
  const int16_t topInset =
      BoardConfig::hasTouch() ? static_cast<int16_t>(theme.spaceLg) : static_cast<int16_t>(theme.spaceSm + 4);
  screen.insetContent(fui::Insets{topInset, hInset, theme.spaceSm, hInset});
  const bool landscape = renderer.getScreenWidth() > renderer.getScreenHeight();
  // Reserve the band's slot in the flow (its content draws at a fixed screen
  // position in drawHeaderBand); the slot doubles as padding above the heading.
  screen.takeTop(UITheme::getInstance().getMetrics().batteryBarHeight, theme.spaceSm);
  // Button boards run tighter above the tabs to make room for the top step.
  const int16_t tabGap = BoardConfig::hasTouch() ? theme.spaceSm : static_cast<int16_t>(4);
  // Keep the band's height out of the flow; the bar itself draws as chrome at the bottom of the
  // screen, the same band every other tab screen reserves.
  screen.takeBottom(static_cast<int16_t>(HomeTabBar::height()), tabGap);
  if (books->empty()) {
    drawTabs();
    drawEmpty(screen);
    drawHeaderBand();
    return;
  }
  // Hero cover matches the grid covers: the body splits into three equal
  // cover rows (hero + two grid rows), so the featured section is one row
  // tall and every cover on screen shares one size. The metadata floor still
  // applies when the rows would be too short for the hero's text lines.
  const fui::Rect body = screen.body();
  const int rowGap = std::max<int>(4, body.width / 100);
  grid.gap = grid.rowGap = rowGap;
  const int coverRowHeight = std::max(1, (body.height - theme.spaceSm - 2 * rowGap) / 3 + COVER_ROW_EXTRA_HEIGHT);
  const int16_t featuredHeight = std::min<int>(
      body.height, std::max<int>(coverRowHeight, screen.target().lineHeight(theme.bodyText.font) * (landscape ? 1 : 2) +
                                                     screen.target().lineHeight(theme.smallText.font) * 2 + 32));
  drawCurrent(screen, screen.takeTop(featuredHeight, theme.spaceSm), coverRowHeight);
  drawGrid(screen);
  drawTabs();
  drawHeaderBand();
}

void CoverGridHomeUi::drawHeaderBand() {
  // The stock full-width band at the theme's topPadding, exactly like every
  // pushed screen's header: the battery/clock hold one position across the
  // whole UI, and the grid is widened to meet them (see the hInset above).
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.batteryBarHeight}, nullptr);
}

void CoverGridHomeUi::drawEmpty(UiScreen& screen) {
  const auto& theme = screen.theme();
  const auto body = screen.body();
  auto title = theme.titleText;
  title.bold = true;
  title.align = fui::TextAlign::Center;
  auto message = theme.bodyText;
  message.align = fui::TextAlign::Center;
  constexpr int16_t ICON_SIZE = 32;
  const int16_t titleHeight = screen.target().lineHeight(title.font);
  const int16_t messageHeight = screen.target().lineHeight(message.font);
  const int16_t contentHeight = ICON_SIZE + theme.spaceLg + titleHeight + theme.spaceSm + messageHeight;
  int16_t y = body.y + std::max(0, (body.height - contentHeight) / 2);
  renderer.drawIcon(BookIcon, body.x + (body.width - ICON_SIZE) / 2, y, ICON_SIZE);
  y += ICON_SIZE + theme.spaceLg;
  screen.target().text(fui::Rect{body.x, y, body.width, titleHeight}, tr(STR_NO_OPEN_BOOK), title);
  y += titleHeight + theme.spaceSm;
  screen.target().text(fui::Rect{body.x, y, body.width, messageHeight}, tr(STR_START_READING), message);
}

void CoverGridHomeUi::drawCurrent(UiScreen& screen, fui::Rect rect, const int coverRowHeight) {
  const auto& theme = screen.theme();
  const auto& book = books->front();
  card.title = book.title.c_str();
  card.author = book.author.empty() ? nullptr : book.author.c_str();
  card.meta = nullptr;
  card.progressLabel = progress >= 0 ? progressText : nullptr;
  card.centerTextOnCover = true;
  card.progress = std::max(0, progress);
  card.progressMax = progress >= 0 ? 100 : 0;
  card.action = SELECT;
  // Focus outlines the whole card, the way Lyra boxes its continue-reading tile -- cover,
  // heading and progress together, since the card is one target. Only where a cursor exists:
  // on a touch board selection sits on the card by default and a permanent box would read as a
  // state rather than a cursor.
  card.selectionIndicator = fui::BookCardSelectionIndicator::Card;
  card.state = selected == 0 && !BoardConfig::hasTouch() ? fui::StateSelected : fui::StateNormal;
  card.styles = theme.listRow;
  // The same dithered grey as every other focus ring, and an outline rather than a wash: a
  // dithered fill behind a cover and two lines of text muddies both.
  card.styles.selected.background = fui::Paint::solid(fui::Color::White);
  // No border from FreeInkUI: its stroke honours a dithered paint only at radius 0, so a rounded
  // one comes out solid black. paintFramedCover() draws the shared ring over heroCardRect instead.
  card.styles.selected.border = fui::Paint::none();
  card.styles.selected.borderWidth = 0;
  card.styles.selected.foreground = fui::Paint::solid(fui::Color::Black);
  card.styles.selected.radius = 0;
  card.styles.active = card.styles.selected;
  card.titleText = theme.bodyText;
  card.titleText.maxLines = renderer.getScreenWidth() > renderer.getScreenHeight() ? 1 : 2;
  card.authorText = theme.smallText;
  card.progressText = theme.smallText;
  card.progressHeight = 6;
  card.padding = fui::Insets{6, 6, 6, 6};
  card.gap = theme.spaceLg + theme.spaceSm;
  // Fit the shared 2:3 cover box within both the row and a grid column. No
  // gap reserve: the cell insets already keep adjacent covers apart, and the
  // SpaceBetween layout absorbs whatever slack remains.
  const int maxCoverWidth = rect.width / GRID_COLUMNS - 2 * COVER_CELL_INSET;
  card.coverSize.height = std::max(1, std::min(std::min<int>(rect.height, coverRowHeight) - 12, maxCoverWidth * 3 / 2));
  card.coverSize.width = std::max(1, card.coverSize.height * 2 / 3);
  noteThumbHeight(card.coverSize.height);
  gridBounds = layoutGrid(screen.body());
  rect.x = gridBounds.x;
  rect.width = gridBounds.width;
  card.coverPainterUserData = this;
  card.coverPainter = [](fui::DrawTarget& target, fui::Rect cover, const fui::BookCardProps&, void* user) {
    return static_cast<CoverGridHomeUi*>(user)->paintFramedCover(target, cover, 0);
  };
  heroCardRect = rect;
  selectedCoverRect = fui::Rect{};
  fui::bookCard(screen.frame(), rect, card);
  // The featured card's ring frames the whole card, not just its cover.
  if (selectedCoverRect.width > 0) {
    selectedCoverRect = heroCardRect;
    paintSelectionRing(0);
  }
}

fui::Rect CoverGridHomeUi::layoutGrid(fui::Rect rect) {
  // Exactly the hero's box: the three-equal-rows split in draw() already
  // guarantees it fits, and sharing the size keeps one cached thumb per book.
  grid.cellInset = fui::Insets{COVER_CELL_INSET, COVER_CELL_INSET, COVER_CELL_INSET, COVER_CELL_INSET};
  grid.coverSize = card.coverSize;
  grid.rowHeight = grid.coverSize.height + 12;
  // Full content width: the SpaceBetween column layout pins the outer covers
  // to the rect edges, so the grid reaches the chrome's inset line instead of
  // centering at its natural width.
  rect.height = GRID_ROWS * grid.rowHeight + (GRID_ROWS - 1) * grid.rowGap;
  return rect;
}

void CoverGridHomeUi::drawGrid(UiScreen& screen) {
  const auto rect = gridBounds;
  grid.count = books->size() > 1 ? books->size() - 1 : 0;
  grid.columns = GRID_COLUMNS;
  grid.columnLayout = fui::CoverGridColumnLayout::SpaceBetween;
  grid.action = SELECT;
  grid.inputMask = fui::InputTouch | fui::InputLongPress;
  grid.selectedIndex = selected > 0 && selected < static_cast<int>(books->size()) ? selected - 1 : -1;
  // Same thick cover ring as the featured card; the dithered Cell background
  // was easy to miss behind a dark cover.
  // The ring comes from paintFramedCover(), not from FreeInkUI: only the shared painter can draw
  // a dithered ring with rounded corners. CoverFrame with zero width draws nothing, and Cell
  // would wash the whole cell.
  grid.selectionIndicator = fui::CoverGridSelectionIndicator::CoverFrame;
  grid.selectedCoverFrameWidth = 0;
  grid.selectedCoverFrameGap = 0;
  grid.cellStyles = card.styles;
  grid.labelHeight = 0;
  grid.labelGap = 0;
  grid.scrollIndicator = false;
  grid.itemProvider = [](uint16_t index, void*) { return fui::coverGridItem(nullptr, index + 1); };
  grid.coverPainterUserData = this;
  grid.coverPainter = [](fui::DrawTarget& target, fui::Rect cover, const fui::CoverGridItem&, uint16_t index,
                         void* user) {
    return static_cast<CoverGridHomeUi*>(user)->paintFramedCover(target, cover, index + 1);
  };
  selectedCoverRect = fui::Rect{};
  fui::coverGrid(screen.frame(), rect, grid);
  paintSelectionRing(UITheme::FOCUS_RING_WIDTH);
}

// The cursor ring, once everything under it is down. `gap` is how far outside the cover it sits.
void CoverGridHomeUi::paintSelectionRing(const int gap) {
  if (selectedCoverRect.width <= 0) return;
  UITheme::drawFocusRing(
      renderer, Rect{static_cast<int16_t>(selectedCoverRect.x - gap), static_cast<int16_t>(selectedCoverRect.y - gap),
                     static_cast<int16_t>(selectedCoverRect.width + 2 * gap),
                     static_cast<int16_t>(selectedCoverRect.height + 2 * gap)});
  selectedCoverRect = fui::Rect{};
}

void CoverGridHomeUi::drawTabs() {
  // Home is the tab we are in. A button board's cursor lands in the band as selected >= books,
  // and that slot gets the focus wash; on touch boards selected never leaves the covers.
  const int focused = selected - static_cast<int>(books->size());
  HomeTabBar::draw(renderer, HomeTab::Home, focused >= 0 && focused < HomeTabBar::COUNT ? focused : -1);
}

bool CoverGridHomeUi::paintFramedCover(fui::DrawTarget& target, fui::Rect rect, size_t index) {
  (void)target;  // the shared painter draws straight to the renderer, outline and shadow included
  // The false spine draws inside the cover paint (HomeCoverCache), glued to
  // the art's left edge, so it stays aligned whatever each cover's margin is.
  if (index == 0) heroCoverRect = rect;
  // Touch boards have no cursor, so nothing is permanently outlined there.
  const bool ring = !BoardConfig::hasTouch() && static_cast<int>(index) == selected;
  bool painted = false;
  if (index < coverPaths.size() && books && index < books->size()) {
    // No badge on the featured card: its percentage already sits under the title, and a second
    // copy in the corner of the big cover is just noise.
    const int badge = index == 0 && hasContinueReading ? -1 : bookProgress[index];
    painted = coverCache.paint(rect, index, coverPaths[index], (*books)[index].title, badge);
  }
  // Remembered, not drawn here: coverGrid() fills each cell's background as it reaches it, so a
  // ring in the gap around one cover was painted over by the next cell. drawGrid() paints it
  // once the whole grid is down.
  if (ring) selectedCoverRect = rect;
  return painted;
}
