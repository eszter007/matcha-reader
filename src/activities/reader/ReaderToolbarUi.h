#pragma once

#include <functional>
#include <string>

#include "components/UiAppHost.h"
#include "components/lists/list.h"

class GfxRenderer;
class MappedInputManager;

// FreeInkUI chrome for the toolbar reader menu (Settings -> Reader -> Reader
// Menu Style -> Toolbar), painted over the page that is already on screen:
//
//  - Toolbar (the dock): a chapter scrub row (|< track >|), the chapter title
//    with page, percent and a bookmark toggle, and the Contents / Text / Page /
//    Translate / More tools, each an icon over its name; the page above it
//    stays untouched.
//  - Panel: a taller bottom sheet with a title, a paged row list (name left,
//    value right) or the Text / Page panel's control rows, and the same tool row as
//    a switcher.
//
// EpubReaderActivity owns the state (which overlay, selection, row contents)
// and the physical-button handling; this class owns the layout, the tap
// targets FreeInkUI registers for it, and the translation of dispatched
// actions back into the small event set the reader acts on. Nothing here
// clears the screen: the page stays visible around the chrome.
class ReaderToolbarUi : public UiAppHost {
 public:
  // Option: a Text control row's option cell or stepper half, value = row * 16 + option.
  enum class Event {
    None = 0,
    Dismiss = 1,
    Tool = 2,
    PrevChapter = 3,
    NextChapter = 4,
    Scrub = 5,
    Row = 6,
    Bookmark = 7,
    Option = 8
  };

  static constexpr int kToolCount = 5;  // Contents, Text, Page, Translate, More
  static constexpr int kTranslateTool = 3;
  // Event::Option packs a control row and its option as row * kOptionStride + option.
  static constexpr int kOptionStride = 16;
  // Toolbar button cursor: the chapter steps, the bookmark, then the tools.
  static constexpr int kFocusPrevChapter = 0;
  static constexpr int kFocusNextChapter = 1;
  static constexpr int kFocusBookmark = 2;
  static constexpr int kFocusFirstTool = 3;
  static constexpr int kFocusCount = kFocusFirstTool + kToolCount;

  // One Text or Page panel row: a label and the control that changes it. On the Page panel the
  // Options rows show page pictures.
  struct TextRow {
    enum class Kind : uint8_t { Stepper, Chooser, Options, Toggle };
    // The drawn pictures of an Options row.
    enum class Picture : uint8_t { None, LineSpacing, Margins, Alignment, Orientation, Writing };
    Kind kind = Kind::Chooser;
    Picture picture = Picture::None;
    const char* label = nullptr;
    const char* value = nullptr;  // Stepper: the number; Chooser: the current choice
    uint8_t optionCount = 0;      // Options
    int8_t selected = -1;         // Options: lit cell, -1 = none; Toggle: 0/1
  };

  struct Model {
    bool panel = false;  // false = toolbar, true = a Contents/Text/More panel
    // Toolbar
    const char* chapterTitle = nullptr;
    const char* pageInfo = nullptr;  // "p. 12/40 · 51%"
    int progressPermille = 0;        // 0..1000 book progress (scrub handle)
    int toolbarFocus = -1;           // button cursor (kFocus*), -1 = none shown
    bool bookmarked = false;         // the page carries a bookmark
    // Panel
    int itemCount = 0;
    int selectedIndex = -1;  // row the buttons' cursor sits on; -1 = none shown
    std::function<std::string(int)> rowText;
    std::function<std::string(int)> rowValue;
    // -1 = regular value row, 0 = unchecked switch, 1 = checked switch.
    std::function<int(int)> rowToggleState;
    // Text panel: control rows instead of the list (rows stay alive until render() returns).
    const TextRow* textRows = nullptr;
    int textRowCount = 0;
    bool pageGrid = false;  // the Page panel: rows of page pictures
    // Tool row: the open panel, -1 = none.
    int activeTool = -1;
    // A short list (More) that should show whole rather than page.
    bool fitAllRows = false;
    // Button boards keep the theme's denser list row height (as every other
    // list does there); touch boards use FreeInkUI's finger-sized rows.
    bool denseRows = false;
  };

  struct Routed {
    Event event = Event::None;
    int value = 0;        // Tool: tool index; Row: row index
    int permille = -1;    // Scrub: 0..1000 along the track
    bool routed = false;  // the gate was open and a touch frame was routed
    int x = 0;            // touch position of the routed frame (logical px)
    int y = 0;
  };

  explicit ReaderToolbarUi(GfxRenderer& renderer);

  // Screen-entry reset + action wiring. Call once when the overlay opens.
  void begin();
  // What the next render() draws. The strings must stay alive until render()
  // returns; the row callbacks are invoked during render() for the visible rows.
  void setModel(const Model& model) { model_ = model; }
  // Paint into the framebuffer (no refresh, no clear). Run from the render task.
  void render();
  // Route one loop-task input frame; returns the action it mapped to, if any.
  Routed route(const MappedInputManager& input);

  // Panel list selection/viewport, shared with the reader's input handling.
  // The same fui::ListNav the list-menu screens use: scrollBy() pages the
  // viewport (measured page size, no-op detection), the top/selected fields
  // are the live state, and buildPanel() syncs it into the list each build.
  freeink::ui::ListNav& nav() { return nav_; }
  // Rows one page holds, measured after the first render.
  int visibleRows() const { return nav_.pageRows(); }

 private:
  static void screenFn(UiScreen& screen, void* user);
  static void onAction(const freeink::ui::ActionEvent& event, void* user);
  void buildToolbar(UiScreen& screen);
  void buildPanel(UiScreen& screen);
  void buildTextRows(UiScreen& screen, freeink::ui::Rect area);
  int textContentHeight() const;
  // Page panel geometry: full size, or shrunk to fit a landscape screen (set by buildPanel()).
  int16_t groupH_ = 0;
  int16_t thumbW_ = 0;
  int16_t thumbH_ = 0;
  void buildToolRow(UiScreen& screen, freeink::ui::LayoutAnchor anchor, int16_t sideInset);

  Model model_;
  Routed pending_;
  freeink::ui::ListNav nav_;

  // Row window materialised for the visible page only (a contents list runs to
  // hundreds of entries; labels are copied here so ListItem can point at them).
  static constexpr int kMaxWindow = 16;
  std::string windowLabels_[kMaxWindow];
  std::string windowValues_[kMaxWindow];
  freeink::ui::ListItem windowItems_[kMaxWindow];
  // fui::ButtonProps / ListProps / HeaderProps embed a 324-byte StyleSet: keep
  // them off the stack (AGENTS.md: locals stay under 256 bytes).
  freeink::ui::ButtonProps stepProps_;
  freeink::ui::ListProps listProps_;
};
