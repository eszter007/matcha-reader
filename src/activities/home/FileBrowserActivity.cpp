#include "FileBrowserActivity.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <MangaPanel.h>
#include <Memory.h>

#include <algorithm>
#include <iterator>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"  // recency-ordered book listing (see loadFiles)
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"
#include "util/DeleteUtils.h"

namespace fui = freeink::ui;

namespace {
constexpr unsigned long GO_HOME_MS = 1000;
constexpr size_t NAME_BUFFER_SIZE = 500;
}  // namespace

std::string getFileName(std::string filename);
std::string getFileExtension(const std::string& filename);

FileBrowserActivity::FileBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                         std::string initialPath, const Mode mode)
    : UiListActivity("FileBrowser", renderer, mappedInput, /*wantsTouchLongPress=*/true),
      mode(mode),
      basepath(initialPath.empty() ? "/" : std::move(initialPath)) {}

void FileBrowserActivity::loadFiles() {
  files.clear();

  auto root = Storage.open(basepath.c_str());
  if (!root || !root.isDirectory()) {
    rebuildRowItems();  // files is empty; also drops any now-stale cached rows
    return;
  }

  root.rewindDirectory();

  if (!fileNameBuffer) {
    LOG_ERR("FileBrowser", "fileNameBuffer not allocated");
    root.close();
    rebuildRowItems();
    return;
  }

  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(fileNameBuffer.get(), NAME_BUFFER_SIZE);
    const bool isDirectory = file.isDirectory();
    // "System Volume Information" is hidden alongside the dot entries rather
    // than unconditionally: an item that never lists can never be deleted, and
    // the filesystem leftovers are exactly what users want to clear off a card.
    if (!SETTINGS.showHiddenFiles &&
        (fileNameBuffer[0] == '.' || strcmp(fileNameBuffer.get(), "System Volume Information") == 0)) {
      continue;
    }

    if (isDirectory) {
      files.emplace_back(std::string(fileNameBuffer.get()) + "/");
    } else {
      std::string_view filename{fileNameBuffer.get()};
      if (mode == Mode::PickFirmware) {
        // Firmware picker: only show .bin files.
        if (FsHelpers::checkFileExtension(filename, ".bin")) {
          files.emplace_back(filename);
        }
      } else {
        // Show every file, even unsupported formats -- render() dims entries
        // isSupportedFile() rejects, so users can see what's on the card
        // without a format silently vanishing from the listing.
        files.emplace_back(filename);
      }
    }
  }
  root.close();

  // Sort: folders first (alphabetical), then books sorted by recency.
  // Books that appear in the recent list come first (in recent order),
  // followed by never-opened books alphabetically.
  const auto& recents = RECENT_BOOKS.getBooks();

  std::vector<std::string> folders;
  std::vector<std::string> recentFiles;
  std::vector<std::string> otherFiles;
  folders.reserve(files.size());
  otherFiles.reserve(files.size());

  for (auto& f : files) {
    if (!f.empty() && f.back() == '/') {
      folders.push_back(std::move(f));
    } else {
      std::string fullPath = basepath;
      if (fullPath.back() != '/') fullPath += '/';
      fullPath += f;
      const bool isRecent =
          std::any_of(recents.begin(), recents.end(), [&fullPath](const auto& r) { return r.path == fullPath; });
      if (isRecent) {
        recentFiles.push_back(std::move(f));
      } else {
        otherFiles.push_back(std::move(f));
      }
    }
  }

  FsHelpers::sortFileList(folders);
  FsHelpers::sortFileList(otherFiles);

  // Sort recent files by their position in the recents list (most recent first).
  std::sort(recentFiles.begin(), recentFiles.end(), [&](const std::string& a, const std::string& b) {
    std::string pathA = basepath + (basepath.back() != '/' ? "/" : "") + a;
    std::string pathB = basepath + (basepath.back() != '/' ? "/" : "") + b;
    size_t idxA = recents.size(), idxB = recents.size();
    for (size_t i = 0; i < recents.size(); i++) {
      if (recents[i].path == pathA) idxA = i;
      if (recents[i].path == pathB) idxB = i;
    }
    return idxA < idxB;
  });

  files.clear();
  files.reserve(folders.size() + recentFiles.size() + otherFiles.size());
  std::move(folders.begin(), folders.end(), std::back_inserter(files));
  std::move(recentFiles.begin(), recentFiles.end(), std::back_inserter(files));
  std::move(otherFiles.begin(), otherFiles.end(), std::back_inserter(files));
  rebuildRowItems();
}

// Derives rowNames/rowExtensions/rowItems from `files`. Called whenever
// `files` changes (end of loadFiles()) so buildScreen() can reuse the cached
// rows on every repaint instead of re-deriving a name/extension string (and a
// ListItem) per file each time it's called.
void FileBrowserActivity::rebuildRowItems() {
  rowsUseFileIcons = UITheme::getInstance().getTheme().showsFileIcons();
  rowNames.resize(files.size());
  rowExtensions.resize(files.size());
  rowItems.clear();
  rowItems.reserve(files.size());
  for (size_t i = 0; i < files.size(); i++) {
    rowNames[i] = getFileName(files[i]);
    rowExtensions[i] = getFileExtension(files[i]);
    fui::ListItem item;
    item.label = rowNames[i].c_str();
    if (!rowExtensions[i].empty()) item.value = rowExtensions[i].c_str();
    item.icon = listIconFor(UITheme::getFileIcon(files[i]));
    item.actionValue = static_cast<int16_t>(i);
    rowItems.push_back(item);
  }

  // One SD pass for every CJK filename in the folder; repaints then hit the
  // resident tables instead of re-reading per-string. Getter form: no
  // concatenated copy (a bare-new string append aborts under heap pressure).
  // The last index covers the bottom path band: basepath (possibly a CJK
  // folder name) draws in the same small font, so it must live in the same
  // batch or it would evict the rows' glyphs when the heap gate disables
  // union merging. (prewarmFallbackText appends the truncation ellipsis.)
  struct PrewarmCtx {
    const std::vector<std::string>* names;
    const std::string* path;
  } prewarmCtx{&rowNames, &basepath};
  renderer.prewarmFallbackText(
      uiScaleSpec().smallFontId,
      [](const void* ctx, uint32_t i) -> const char* {
        const auto* c = static_cast<const PrewarmCtx*>(ctx);
        return i < c->names->size() ? (*c->names)[i].c_str() : c->path->c_str();
      },
      &prewarmCtx, static_cast<uint32_t>(rowNames.size()) + 1);
}

void FileBrowserActivity::onEnter() {
  UiListActivity::onEnter();

  fileNameBuffer = makeUniqueNoThrow<char[]>(NAME_BUFFER_SIZE);
  if (!fileNameBuffer) {
    LOG_ERR("FileBrowser", "malloc failed for name buffer");
    return;
  }

  auto root = Storage.open(basepath.c_str());
  if (!root) {
    basepath = "/";
    loadFiles();
  } else if (!root.isDirectory()) {
    const std::string oldPath = basepath;
    basepath = FsHelpers::extractFolderPath(basepath);
    loadFiles();

    const auto pos = oldPath.find_last_of('/');
    const std::string fileName = oldPath.substr(pos + 1);
    // The first screen build pulls the viewport to it (ListNav follow-on-build).
    nav.selected = static_cast<int>(findEntry(fileName));
  } else {
    loadFiles();
  }
}

void FileBrowserActivity::onExit() {
  Activity::onExit();
  files.clear();
  rowNames.clear();
  rowExtensions.clear();
  rowItems.clear();
  fileNameBuffer.reset();
}

void FileBrowserActivity::activateIndex(const int index) {
  (void)index;  // base already synced nav.selected to the tapped row
  // Activation navigates or opens; a lingering flash would gray an unrelated
  // row on the next list.
  app.clearTapFlash();
  activateSelected();
}

void FileBrowserActivity::onRowLongPress(const int index) {
  (void)index;  // base already synced nav.selected to the pressed row
  // Activation navigates or opens; a lingering flash would gray an unrelated
  // row on the next list.
  app.clearTapFlash();
  activateSelected(/*forceDelete=*/true);
}

void FileBrowserActivity::activateSelected(const bool forceDelete) {
  if (files.empty()) return;
  // A touch activation can carry a row index captured before a delete/reload
  // shrank the list; the next render re-registers the rows.
  if (nav.selected < 0 || nav.selected >= listCount()) return;

  const std::string& entry = files[nav.selected];
  bool isDirectory = (entry.back() == '/');

  // Firmware picker: select file -> return path; navigate into directories normally.
  if (mode == Mode::PickFirmware && !isDirectory) {
    std::string cleanBasePath = basepath;
    if (cleanBasePath.back() != '/') cleanBasePath += "/";
    ActivityResult res{FilePathResult{cleanBasePath + entry}};
    res.isCancelled = false;
    setResult(std::move(res));
    finish();
    return;
  }

  if (mode == Mode::Books && (forceDelete || mappedInput.getHeldTime() >= GO_HOME_MS)) {
    // --- LONG PRESS ACTION: DELETE FILE OR DIRECTORY ---
    std::string cleanBasePath = basepath;
    if (cleanBasePath.back() != '/') cleanBasePath += "/";
    const std::string fullPath = cleanBasePath + entry;

    auto handler = [this, fullPath](const ActivityResult& res) {
      if (!res.isCancelled) {
        LOG_DBG("FileBrowser", "Attempting to delete: %s", fullPath.c_str());
        if (deletePathRecursive(fullPath)) {
          LOG_DBG("FileBrowser", "Deleted successfully");
          {
            // buildScreen() reads the row caches on the render task; see loop().
            RenderLock lock(*this);
            loadFiles();
            if (files.empty()) {
              nav.selected = 0;
            } else if (nav.selected >= listCount()) {
              // Move selection to the new "last" item
              nav.selected = listCount() - 1;
            }
            nav.follow(listCount());
          }

          requestUpdate(true);
        } else {
          LOG_ERR("FileBrowser", "Failed to delete: %s", fullPath.c_str());
        }
      } else {
        LOG_DBG("FileBrowser", "Delete cancelled by user");
      }
    };

    std::string heading = tr(STR_DELETE) + std::string("? ");

    startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, entry), handler);
    return;
  } else {
    // --- SHORT PRESS ACTION: OPEN/NAVIGATE ---
    // buildScreen() runs on the render task and reads basepath plus the
    // ListItem label/value pointers into rowNames/rowExtensions that
    // rebuildRowItems() frees; mutate only under the render lock.
    RenderLock lock(*this);
    if (basepath.back() != '/') basepath += "/";

    if (isDirectory) {
      // A manga book IS a directory (page images + panels.idx), so it must open as a book
      // rather than be navigated into. Upstream has no manga and navigates every directory.
      const std::string dirPath = basepath + entry.substr(0, entry.length() - 1);
      if (manga::MangaBook::isMangaFolder(dirPath)) {
        onSelectBook(dirPath);
        return;
      }
      basepath = dirPath;
      loadFiles();
      nav.selected = 0;
      nav.top = 0;
      lock.unlock();
      requestUpdate();
    } else {
      const std::string fullPath = basepath + entry;
      lock.unlock();  // onSelectBook launches an activity; don't hold the lock across it
      onSelectBook(fullPath);
    }
  }
  return;
}

bool FileBrowserActivity::handleCustomInput() {
  // Long press BACK (1s+) goes to root folder (Books mode only).
  // In firmware-pick mode we keep navigation simple: short Back = up dir / cancel.
  if (mode == Mode::Books && mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() >= GO_HOME_MS && basepath != "/") {
    {
      // buildScreen() runs on the render task and reads basepath plus the
      // row caches rebuildRowItems() frees; mutate only under the render lock.
      RenderLock lock(*this);
      basepath = "/";
      loadFiles();
      nav.selected = 0;
      nav.top = 0;
    }
    requestUpdate();
    return true;
  }

  return false;
}

bool FileBrowserActivity::handleButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateSelected();
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // Short press: go up one directory, or go home if at root
    if (mappedInput.getHeldTime() < GO_HOME_MS) {
      if (basepath != "/") {
        const std::string oldPath = basepath;

        {
          // buildScreen() runs on the render task and reads basepath plus the
          // row caches rebuildRowItems() frees; mutate only under the render lock.
          RenderLock lock(*this);
          basepath.replace(basepath.find_last_of('/'), std::string::npos, "");
          if (basepath.empty()) basepath = "/";
          loadFiles();

          const auto pos = oldPath.find_last_of('/');
          const std::string dirName = oldPath.substr(pos + 1) + "/";
          nav.selected = static_cast<int>(findEntry(dirName));
          nav.top = 0;
          nav.follow(listCount());
        }

        requestUpdate();
      } else if (mode == Mode::PickFirmware) {
        // Firmware picker at root: cancel back to caller instead of going home.
        ActivityResult res;
        res.isCancelled = true;
        setResult(std::move(res));
        finish();
      } else {
        onGoHome();
      }
    }
    return true;
  }

  return false;
}

std::string getFileName(std::string filename) {
  if (filename.empty()) return filename;
  if (filename.back() == '/') {
    filename.pop_back();
    if (!UITheme::getInstance().getTheme().showsFileIcons()) {
      return "[" + filename + "]";
    }
    return filename;
  }
  const auto pos = filename.rfind('.');
  if (pos == std::string::npos) return filename;
  return filename.substr(0, pos);
}

std::string getFileExtension(const std::string& filename) {
  if (filename.empty() || filename.back() == '/') {
    return "";
  }
  const auto pos = filename.rfind('.');
  // A name without a dot has no extension. substr(npos) throws out_of_range,
  // and with -fno-exceptions that lands in abort().
  if (pos == std::string::npos) return "";
  return filename.substr(pos);
}

bool FileBrowserActivity::isSupportedFile(std::string_view filename) {
  return FsHelpers::hasEpubExtension(filename) || FsHelpers::hasXtcExtension(filename) ||
         FsHelpers::hasTxtExtension(filename) || FsHelpers::hasMarkdownExtension(filename) ||
         FsHelpers::hasBmpExtension(filename);
}

void FileBrowserActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the GUI.drawHeader band, above the button hints.
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                      static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  // Full path band at the bottom: separator on top, left-truncated so the
  // deepest directory stays visible.
  {
    const int pathLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
    const fui::Rect band = screen.takeBottom(static_cast<int16_t>(pathLineHeight + metrics.verticalSpacing));
    screen.target().fill(fui::Rect{band.x, band.y, band.width, 3}, fui::Paint::solid(fui::Color::Black));
    const int pathY =
        band.y + metrics.verticalSpacing / 2 + (band.height - metrics.verticalSpacing / 2 - pathLineHeight) / 2;
    const int pathMaxWidth = band.width - metrics.contentSidePadding * 2;
    const char* pathStr = basepath.c_str();
    const char* pathDisplay = pathStr;
    char leftTruncBuf[256];
    if (renderer.getTextWidth(SMALL_FONT_ID, pathStr) > pathMaxWidth) {
      const char ellipsis[] = "\xe2\x80\xa6";  // UTF-8 ellipsis (…)
      const int ellipsisWidth = renderer.getTextWidth(SMALL_FONT_ID, ellipsis);
      const int available = pathMaxWidth - ellipsisWidth;
      // Walk forward from the start until the suffix fits, skipping UTF-8 continuation bytes
      const char* p = pathStr;
      while (*p) {
        if (renderer.getTextWidth(SMALL_FONT_ID, p) <= available) break;
        ++p;
        while (*p && (static_cast<unsigned char>(*p) & 0xC0) == 0x80) ++p;
      }
      snprintf(leftTruncBuf, sizeof(leftTruncBuf), "%s%s", ellipsis, p);
      pathDisplay = leftTruncBuf;
    }
    renderer.drawText(SMALL_FONT_ID, band.x + metrics.contentSidePadding, pathY, pathDisplay);
  }

  if (files.empty()) {
    screen.centeredText(mode == Mode::PickFirmware ? tr(STR_NO_BIN_FILES) : tr(STR_NO_FILES_FOUND),
                        screen.theme().bodyText);
    return;
  }

  // rowNames/rowExtensions/rowItems are built once per loadFiles() call (see
  // rebuildRowItems()) and reused here. getFileName()'s folder-bracket format
  // depends on the theme, so a theme change picked up while this activity was
  // paused underneath another screen invalidates the cache before it's read.
  if (rowsUseFileIcons != UITheme::getInstance().getTheme().showsFileIcons()) {
    rebuildRowItems();
  }

  fui::ListProps props;
  props.items = rowItems.data();
  props.count = static_cast<uint16_t>(rowItems.size());
  props.action = ACTION_ROW;
  // Tap opens/navigates; long-press prompts delete (physical buttons stay in loop()).
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  props.valueInset = 8;  // air between the extension and the row edge
  // Keep file names on one line: long names are cropped rather than making
  // only their selection row taller. Zero renders as one line and also marks
  // the small-font style as caller-owned (FONT_SLOT_SMALL itself is 0).
  fui::TextStyle label = screen.theme().smallText;
  label.maxLines = 0;
  props.labelText = label;
  // The trailing value here is just the short extension: skip the balanced
  // 60%-band wrap cap and let both name lines run the full width before it.
  props.balanceWrappedLabelWithValue = false;
  // Wrapped two-line names shrink how many rows fit a page, so the last row
  // of a page can end up in leftover space: draw it as a partial preview so
  // files past the fold are visibly present, not silently absent.
  props.partialTrailingRow = true;
  syncListViewport(screen, props);
  screen.list(props);
}

void FileBrowserActivity::drawChrome() {
  const auto pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();

  std::string folderName =
      (mode == Mode::PickFirmware)
          ? std::string(tr(STR_SELECT_FIRMWARE_FILE))
          : ((basepath == "/") ? std::string(tr(STR_SD_CARD)) : basepath.substr(basepath.rfind('/') + 1));
  // Header via GUI.drawHeader (already FreeInkUI-themed) for the battery
  // indicator; the rest of the screen renders through the app.
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, folderName.c_str());
}

void FileBrowserActivity::drawFooter() {
  const char* backLabel = (basepath == "/") ? (mode == Mode::PickFirmware ? tr(STR_BACK) : tr(STR_HOME)) : tr(STR_BACK);
  // In PickFirmware mode, Confirm on a .bin returns the path to the caller (not "open"); show
  // STR_SELECT instead. Directories in the same picker still descend, so keep STR_OPEN there.
  const bool selectingFirmwareFile = mode == Mode::PickFirmware && !files.empty() && nav.selected >= 0 &&
                                     nav.selected < listCount() && files[nav.selected].back() != '/';
  const char* confirmLabel = files.empty() ? "" : (selectingFirmwareFile ? tr(STR_SELECT) : tr(STR_OPEN));
  const auto labels = mappedInput.mapLabels(backLabel, confirmLabel, files.empty() ? "" : tr(STR_DIR_UP),
                                            files.empty() ? "" : tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

size_t FileBrowserActivity::findEntry(const std::string& name) const {
  for (size_t i = 0; i < files.size(); i++)
    if (files[i] == name) return i;
  return 0;
}
