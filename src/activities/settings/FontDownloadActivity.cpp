#include "FontDownloadActivity.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>
#include <esp_rom_crc.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

namespace fui = freeink::ui;

namespace {
// Entry gate for the whole manifest screen: the parsed document stays live
// while families_ and its per-family strings and vectors are allocated beside
// it, and the document dominates that peak (27.7KB of JSON at sd-fonts-m1-b4
// parses to roughly 25KB). Require a little over the pair, plus a contiguous
// block for the largest single allocation. Same shape and the same reasoning as
// the styled-definition gate in DictHtmlPages.cpp. Checked before the fetch
// (issue #191: a book open on a large SD-card font can leave too little heap
// for the WiFi/TLS connect itself, which -- like the JSON build below -- runs
// std::string/std::vector growth through the throwing operator new) and, in
// onEnter(), before esp_wifi_init runs at all.
constexpr size_t FONT_SCREEN_MIN_FREE_HEAP = 48 * 1024;
constexpr size_t FONT_SCREEN_MIN_MAX_ALLOC = 12 * 1024;

// Headroom over the exact bytes the catalog build is about to allocate, checked
// once the document is parsed and its own footprint is already spent. The floor
// above cannot serve here: it covers the whole-screen peak *including* the
// document, so reusing it after the document is resident demands that memory
// twice and refuses a build that needs about 7KB. The manifest grows over
// time -- 27.7KB of JSON at sd-fonts-m1-b4, against the ~17KB this screen was
// first sized for -- so measure the requirement rather than restating it.
constexpr size_t FONT_BUILD_HEADROOM = 12 * 1024;
}  // namespace

FontDownloadActivity::FontDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("FontDownload", renderer, mappedInput), fontInstaller_(sdFontSystem.registry()) {}

void FontDownloadActivity::activateIndex(const int index) {
  switch (state_) {
    case GROUP_LIST:
      app.clearTapFlash();
      enterGroup(index);
      requestUpdate();
      return;
    case FAMILY_LIST:
      nav.selected = index;
      // Activation starts a download or opens the delete prompt; a lingering
      // flash would gray an unrelated row.
      app.clearTapFlash();
      activateSelected();  // ends with requestUpdateAndWait itself
      return;
    case WIFI_SELECTION:
    case LOADING_MANIFEST:
    case DOWNLOADING:
    case COMPLETE:
    case ERROR:
      return;
  }
}

fui::ListNav& FontDownloadActivity::activeNav() { return state_ == GROUP_LIST ? groupNav_ : nav; }

void FontDownloadActivity::onBackButton() {
  if (state_ != FAMILY_LIST || !hasGroupScreen()) {
    finish();
    return;
  }

  closeRouting();
  {
    RenderLock lock(*this);
    state_ = GROUP_LIST;
    rowsDirty_ = true;
  }
  requestUpdate();
}

// --- Lifecycle ---

void FontDownloadActivity::onEnter() {
  UiListActivity::onEnter();

  // Reclaim before the WiFi stack comes up, not after (this screen draws in the
  // built-in UI fonts; ensureLoaded() restores the reader's selection when it
  // resumes). Same ordering as CrossPointWebServerActivity::onEnter().
  //
  // Releasing only the glyph caches is not enough: reached from a book by way of
  // Text Settings, that path leaves ~41KB free where the manifest build needs
  // ~48KB, and the screen refuses itself. The resident SD font families are the
  // rest of the difference, so release those as well.
  {
    RenderLock lock(*this);
    sdFontSystem.releaseAllResidentFonts(renderer);
  }

  // esp_wifi_init claims tens of KB and reports OOM by returning an error that
  // nothing here can act on late: once the heap is drained, the next newlib
  // stdio lock (a single ~80-byte FreeRTOS mutex, taken on any log line) hits
  // the abort() in lock_init_generic and panics the device. The manifest gates
  // below all run after WiFi is already up, so they cannot catch this. Refuse
  // the screen instead, while refusing is still possible.
  if (ESP.getFreeHeap() < std::max<size_t>(FONT_SCREEN_MIN_FREE_HEAP, HttpDownloader::MIN_TLS_FREE_HEAP) ||
      ESP.getMaxAllocHeap() < std::max<size_t>(FONT_SCREEN_MIN_MAX_ALLOC, HttpDownloader::MIN_TLS_MAX_ALLOC)) {
    LOG_ERR("FONT", "Low heap before WiFi start (%u free, %u max block)", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    {
      RenderLock lock(*this);
      errorMessage_ = tr(STR_LOW_MEMORY_RETRY);
      state_ = ERROR;
    }
    // UiListActivity::onEnter() already requested a render, which may have drawn
    // the empty list before ERROR was set. Ask again so the error screen shows.
    requestUpdate();
    return;
  }

  WiFi.mode(WIFI_STA);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void FontDownloadActivity::onExit() {
  Activity::onExit();

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void FontDownloadActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    finish();
    return;
  }

  {
    RenderLock lock(*this);
    state_ = LOADING_MANIFEST;
  }
  requestUpdateAndWait();

  // The reader that opened this screen keeps its EPUB, layout and SD-font caches
  // resident underneath (activities are pushed on a stack, not replaced -- see
  // ActivityManager), and the WiFi selection screen just shown may have rendered
  // a CJK SSID and repopulated them further. That is exactly the setup
  // CrossPointWebServerActivity and CalibreConnectActivity already release
  // before their own WiFi-heavy work, for the same reason: reclaim what a
  // Japanese book's SD font caches hold (tens of KB at a large size) before the
  // TLS handshake and manifest build below, both of which run
  // std::string/std::vector growth that aborts on OOM (issue #191). Fonts
  // reload lazily once the reader resumes. Full release, matching onEnter():
  // a CJK SSID reloads the JP fallback family, not just its glyph slabs. Under
  // RenderLock for the same reason onEnter() is: unloading the families retires
  // renderer font registrations the render task walks, so a concurrent update
  // would read them as they are freed.
  {
    RenderLock lock(*this);
    sdFontSystem.releaseAllResidentFonts(renderer);
  }

  if (!fetchAndParseManifest()) {
    // Drop whatever was parsed before the failure: it would otherwise sit in
    // the heap behind the error screen, and leave the retry path pointing at a
    // half-built family table.
    clearManifest();
    {
      RenderLock lock(*this);
      state_ = ERROR;
    }
    return;
  }

  if (!hasGroupScreen()) buildFilteredIndices(0);

  {
    RenderLock lock(*this);
    rowsDirty_ = true;  // families_ just loaded
    if (hasGroupScreen()) {
      groupNav_.reset();
      state_ = GROUP_LIST;
    } else {
      nav.reset();
      state_ = FAMILY_LIST;
    }
  }
}

// --- Manifest fetching ---

void FontDownloadActivity::clearManifest() {
  // Swap rather than clear: clear() keeps the capacity, and this runs to hand
  // the heap back while the error screen is up. Reverse allocation order.
  std::vector<int>().swap(filteredIndices_);
  std::vector<ManifestFamily>().swap(families_);
  std::vector<StrRef>().swap(scriptGroupLabels_);
  files_.reset();
  fileEntryCount_ = 0;
  stringArena_.reset();
  arenaUsed_ = 0;
  arenaCapacity_ = 0;
}

bool FontDownloadActivity::internString(const char* text, StrRef& outRef) {
  if (text == nullptr || *text == '\0') {
    outRef = 0;
    return true;
  }
  const size_t length = std::strlen(text) + 1;
  if (arenaUsed_ + length > arenaCapacity_) {
    LOG_ERR("FONT", "Manifest string arena overflow at %u/%u bytes", arenaUsed_, arenaCapacity_);
    return false;
  }
  outRef = arenaUsed_;
  std::memcpy(stringArena_.get() + arenaUsed_, text, length);
  arenaUsed_ = static_cast<uint32_t>(arenaUsed_ + length);
  return true;
}

bool FontDownloadActivity::fetchAndParseManifest() {
  // The reclaim that issue #191 put here now happens in the sole caller, which
  // releases the resident families as well as their glyph slabs and does it
  // under RenderLock. Nothing repopulates either between there and here.

  // Refuse before even opening the connection: the WiFi/TLS handshake and the
  // manual HTTP client (SecureClient/SecureHttpClient) run their own
  // std::string/std::vector growth with no heap gate of their own. Failing
  // fast here also skips a WiFi/TLS round trip that would only end in the
  // same low-memory error once the build gate below is reached anyway. Uses
  // the stricter of the screen's own floor and the TLS floor, since this one
  // check now covers both the connection and the transfer.
  if (ESP.getFreeHeap() < std::max<size_t>(FONT_SCREEN_MIN_FREE_HEAP, HttpDownloader::MIN_TLS_FREE_HEAP) ||
      ESP.getMaxAllocHeap() < std::max<size_t>(FONT_SCREEN_MIN_MAX_ALLOC, HttpDownloader::MIN_TLS_MAX_ALLOC)) {
    LOG_ERR("FONT", "Low heap before manifest fetch (%u free, %u max block)", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    errorMessage_ = tr(STR_LOW_MEMORY_RETRY);
    return false;
  }

  // Download manifest to a temp file on SD card to avoid holding both
  // TLS buffers and the full JSON string in RAM simultaneously.
  static constexpr const char* MANIFEST_TMP = "/fonts_manifest.tmp";

  auto result = HttpDownloader::downloadToFile(FONT_MANIFEST_URL, MANIFEST_TMP, nullptr);
  if (result != HttpDownloader::OK) {
    LOG_ERR("FONT", "Failed to fetch manifest from %s", FONT_MANIFEST_URL);
    errorMessage_ = tr(STR_FONT_LIST_FETCH_FAILED);
    Storage.remove(MANIFEST_TMP);
    return false;
  }

  // HTTP client is now closed — TLS buffers freed. Parse JSON from file.
  HalFile manifestFile;
  if (!Storage.openFileForRead("FONT", MANIFEST_TMP, manifestFile)) {
    LOG_ERR("FONT", "Failed to open temp manifest");
    Storage.remove(MANIFEST_TMP);
    errorMessage_ = "Failed to read font list";
    return false;
  }

  JsonDocument doc;
  DeserializationError err;
  {
    // "styles" is the only key the catalog never reads. Dropping it keeps the
    // DOM about 1KB smaller while it coexists with the arena allocated below.
    JsonDocument filter;
    filter["version"] = true;
    filter["baseUrl"] = true;
    filter["scriptGroups"][0]["tag"] = true;
    filter["scriptGroups"][0]["label"] = true;
    filter["families"][0]["name"] = true;
    filter["families"][0]["description"] = true;
    filter["families"][0]["scripts"] = true;
    filter["families"][0]["files"][0]["name"] = true;
    filter["families"][0]["files"][0]["size"] = true;
    filter["families"][0]["files"][0]["crc32"] = true;
    err = deserializeJson(doc, manifestFile, DeserializationOption::Filter(filter));
  }
  manifestFile.close();
  Storage.remove(MANIFEST_TMP);

  if (err) {
    LOG_ERR("FONT", "Manifest parse error: %s (%u free, %u max block)", err.c_str(), ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
    // ArduinoJson reports an exhausted heap as NoMemory rather than aborting, so
    // name it: "invalid manifest" would send the user hunting a server-side fault.
    errorMessage_ = err == DeserializationError::NoMemory ? tr(STR_LOW_MEMORY_RETRY) : tr(STR_INVALID_FONT_MANIFEST);
    return false;
  }

  int version = doc["version"] | 0;
  if (version != FONTS_MANIFEST_VERSION) {
    LOG_ERR("FONT", "Unsupported manifest version: %d", version);
    errorMessage_ = "Unsupported manifest version";
    return false;
  }

  JsonArray groupsArr = doc["scriptGroups"].as<JsonArray>();
  JsonArray familiesArr = doc["families"].as<JsonArray>();

  // Size the arena and the file table in one pass so neither reallocates while
  // the catalog is built: a mid-build growth would both fragment the heap and
  // invalidate arena pointers already handed out below. Allocates nothing, so
  // it can run ahead of the gate and tell it what the build actually costs.
  const size_t groupCount = std::min(groupsArr.size(), MAX_SCRIPT_GROUPS);
  size_t arenaBytes = 1;  // leading terminator makes offset 0 the empty string
  size_t manifestFileCount = 0;
  for (size_t groupIndex = 0; groupIndex < groupCount; groupIndex++) {
    arenaBytes += std::strlen(groupsArr[groupIndex]["label"] | "") + 1;
  }
  for (JsonObject fObj : familiesArr) {
    arenaBytes += std::strlen(fObj["name"] | "") + 1;
    arenaBytes += std::strlen(fObj["description"] | "") + 1;
    for (JsonObject fileObj : fObj["files"].as<JsonArray>()) {
      arenaBytes += std::strlen(fileObj["name"] | "") + 1;
      manifestFileCount++;
    }
  }

  // Release the previous catalog before measuring, so a retry is judged on the
  // heap the build will really see rather than on one still holding the state
  // it is about to replace.
  clearManifest();

  // Everything below builds std::string/std::vector members while the parsed
  // document stays live. Those allocations go through the throwing operator new,
  // which under -fno-exceptions calls abort() instead of returning null, so
  // exhausting the heap here panics to the boot screen instead of reporting a
  // failure. Refuse up front, while refusing is still possible -- against what
  // this manifest costs, since the document's share is already spent.
  // Every allocation this function makes that scales with the manifest, so the
  // check keeps matching as the manifest grows. The row caches count too: they
  // are reserved at the end of this function, while the document is still live.
  // sizeof the element types rather than constants, so the arithmetic follows
  // the structs.
  const size_t rowCapacity = std::max(familiesArr.size() + 2, groupCount + 1);
  const size_t fileTableBytes = manifestFileCount * sizeof(ManifestFile);
  const size_t familyTableBytes = familiesArr.size() * sizeof(ManifestFamily);
  const size_t groupLabelBytes = groupCount * sizeof(StrRef);
  const size_t filteredIndexBytes = familiesArr.size() * sizeof(int);
  const size_t rowLabelBytes = rowCapacity * sizeof(decltype(rowLabels_)::value_type);
  const size_t rowItemBytes = rowCapacity * sizeof(decltype(rowItems_)::value_type);
  const size_t buildBytes = arenaBytes + fileTableBytes + familyTableBytes + groupLabelBytes + filteredIndexBytes +
                            rowLabelBytes + rowItemBytes;
  // The largest single block decides whether a fragmented heap can serve the
  // build at all, however much total free it reports.
  const size_t largestBlock = std::max(
      {arenaBytes, fileTableBytes, familyTableBytes, groupLabelBytes, filteredIndexBytes, rowLabelBytes, rowItemBytes});
  if (ESP.getFreeHeap() < buildBytes + FONT_BUILD_HEADROOM || ESP.getMaxAllocHeap() < largestBlock) {
    LOG_ERR("FONT", "Low heap for manifest build (%u free, %u max block; need %zu + %zu headroom, %zu block)",
            ESP.getFreeHeap(), ESP.getMaxAllocHeap(), buildBytes, FONT_BUILD_HEADROOM, largestBlock);
    errorMessage_ = tr(STR_LOW_MEMORY_RETRY);
    return false;
  }

  baseUrl_ = doc["baseUrl"] | "";
  downloadUrl_.reserve(baseUrl_.size() + 128);
  fontInstaller_.refreshRegistry();

  stringArena_ = makeUniqueNoThrow<char[]>(arenaBytes);
  if (!stringArena_) {
    LOG_ERR("FONT", "OOM: %zu byte string arena", arenaBytes);
    errorMessage_ = tr(STR_MEMORY_ERROR);
    return false;
  }
  stringArena_[0] = '\0';
  arenaUsed_ = 1;
  arenaCapacity_ = static_cast<uint32_t>(arenaBytes);
  files_ = makeUniqueNoThrow<ManifestFile[]>(manifestFileCount);
  if (!files_) {
    LOG_ERR("FONT", "OOM: %zu manifest file entries", manifestFileCount);
    errorMessage_ = tr(STR_MEMORY_ERROR);
    return false;
  }

  scriptGroupLabels_.reserve(groupCount);
  if (groupsArr.size() > MAX_SCRIPT_GROUPS) {
    LOG_ERR("FONT", "Manifest declares more than %zu script groups; extra groups ignored", MAX_SCRIPT_GROUPS);
  }
  for (size_t groupIndex = 0; groupIndex < groupCount; groupIndex++) {
    JsonObject groupObj = groupsArr[groupIndex].as<JsonObject>();
    const char* tag = groupObj["tag"] | "";
    const char* label = groupObj["label"] | "";
    if (*tag == '\0' || *label == '\0') {
      LOG_ERR("FONT", "Malformed script group at index %zu", groupIndex);
      errorMessage_ = tr(STR_INVALID_FONT_MANIFEST);
      return false;
    }
    StrRef labelRef = 0;
    if (!internString(label, labelRef)) {
      errorMessage_ = tr(STR_INVALID_FONT_MANIFEST);
      return false;
    }
    scriptGroupLabels_.push_back(labelRef);
  }

  families_.reserve(familiesArr.size());
  filteredIndices_.reserve(familiesArr.size());

  for (JsonObject fObj : familiesArr) {
    ManifestFamily family;
    if (!internString(fObj["name"] | "", family.name) || !internString(fObj["description"] | "", family.description)) {
      errorMessage_ = tr(STR_INVALID_FONT_MANIFEST);
      return false;
    }

    for (JsonVariant script : fObj["scripts"].as<JsonArray>()) {
      const char* familyTag = script.as<const char*>();
      if (!familyTag) continue;
      for (size_t groupIndex = 0; groupIndex < scriptGroupLabels_.size(); groupIndex++) {
        JsonObject groupObj = groupsArr[groupIndex].as<JsonObject>();
        const char* groupTag = groupObj["tag"] | "";
        if (std::strcmp(familyTag, groupTag) == 0) {
          family.scriptMask |= uint32_t{1} << groupIndex;
          break;
        }
      }
    }

    family.fileStart = fileEntryCount_;
    for (JsonObject fileObj : fObj["files"].as<JsonArray>()) {
      ManifestFile file;
      if (!internString(fileObj["name"] | "", file.name)) {
        errorMessage_ = tr(STR_INVALID_FONT_MANIFEST);
        return false;
      }
      file.size = fileObj["size"] | 0u;

      if (!fileObj["crc32"].is<uint32_t>()) {
        LOG_ERR("FONT", "Malformed manifest file entry: missing or invalid crc32 for %s", str(file.name));
        errorMessage_ = tr(STR_INVALID_FONT_MANIFEST);
        return false;
      }
      file.crc32 = fileObj["crc32"].as<uint32_t>();

      family.totalSize += file.size;
      files_[fileEntryCount_++] = file;
    }
    family.fileCount = fileEntryCount_ - family.fileStart;

    family.installed = fontInstaller_.isFamilyInstalled(str(family.name));

    // Detect updates by comparing manifest file sizes with files on disk.
    // Not a checksum, but a size mismatch reliably indicates a rebuild in practice.
    if (family.installed) {
      for (uint32_t i = 0; i < family.fileCount; i++) {
        const ManifestFile& file = files_[family.fileStart + i];
        char path[128];
        FontInstaller::buildFontPath(str(family.name), str(file.name), path, sizeof(path));
        HalFile f;
        if (Storage.openFileForRead("FONT", path, f)) {
          size_t actual = f.fileSize();
          f.close();
          if (actual != file.size) {
            family.hasUpdate = true;
            break;
          }
        } else {
          // File missing on disk but family dir exists — treat as update
          family.hasUpdate = true;
          break;
        }
      }
    }

    families_.push_back(family);
  }

  // rowCapacity is the figure the gate above budgeted for; reuse it rather than
  // recomputing, so the two cannot drift apart.
  rowLabels_.reserve(rowCapacity);
  rowItems_.reserve(rowCapacity);

  LOG_DBG("FONT", "Manifest loaded: %zu families, %zu script groups", families_.size(), scriptGroupLabels_.size());
  return true;
}

// --- Download ---

void FontDownloadActivity::downloadAll() {
  cancelRequested_ = false;
  for (const int familyIndex : filteredIndices_) {
    if (families_[familyIndex].installed) continue;
    downloadFamily(families_[familyIndex]);
    if (state_ == ERROR || cancelRequested_) return;
  }

  {
    RenderLock lock(*this);
    state_ = COMPLETE;
  }
}

void FontDownloadActivity::updateAll() {
  cancelRequested_ = false;
  for (const int familyIndex : filteredIndices_) {
    if (!families_[familyIndex].hasUpdate) continue;
    downloadFamily(families_[familyIndex]);
    if (state_ == ERROR || cancelRequested_) return;
  }

  {
    RenderLock lock(*this);
    state_ = COMPLETE;
  }
}

bool FontDownloadActivity::showDownloadAllRow() const {
  for (const int familyIndex : filteredIndices_) {
    if (!families_[familyIndex].installed) return true;
  }
  return false;
}

bool FontDownloadActivity::showUpdateAllRow() const {
  for (const int familyIndex : filteredIndices_) {
    if (families_[familyIndex].hasUpdate) return true;
  }
  return false;
}

int FontDownloadActivity::specialRowCount() const {
  return (showDownloadAllRow() ? 1 : 0) + (showUpdateAllRow() ? 1 : 0);
}

bool FontDownloadActivity::isDownloadAllRow(int index) const { return showDownloadAllRow() && index == 0; }

bool FontDownloadActivity::isUpdateAllRow(int index) const {
  return showUpdateAllRow() && index == (showDownloadAllRow() ? 1 : 0);
}

int FontDownloadActivity::listItemCount() const {
  return filteredIndices_.empty() ? 0 : static_cast<int>(filteredIndices_.size()) + specialRowCount();
}

int FontDownloadActivity::listCount() const {
  switch (state_) {
    case GROUP_LIST:
      return groupListItemCount();
    case FAMILY_LIST:
      return listItemCount();
    case WIFI_SELECTION:
    case LOADING_MANIFEST:
    case DOWNLOADING:
    case COMPLETE:
    case ERROR:
      return 0;
  }
  return 0;
}

int FontDownloadActivity::familyIndexFromList(const int listIndex) const {
  const int filteredIndex = listIndex - specialRowCount();
  if (filteredIndex < 0 || filteredIndex >= static_cast<int>(filteredIndices_.size())) return -1;
  return filteredIndices_[filteredIndex];
}

int FontDownloadActivity::groupMemberCount(const int scriptGroupIndex) const {
  if (scriptGroupIndex < 0 || scriptGroupIndex >= static_cast<int>(scriptGroupLabels_.size())) return 0;
  const uint32_t groupBit = uint32_t{1} << scriptGroupIndex;
  int count = 0;
  for (const auto& family : families_) {
    if (family.scriptMask & groupBit) count++;
  }
  return count;
}

void FontDownloadActivity::buildFilteredIndices(const int groupListIndex) {
  filteredIndices_.clear();
  filteredIndices_.reserve(families_.size());
  if (groupListIndex <= 0) {
    for (int familyIndex = 0; familyIndex < static_cast<int>(families_.size()); familyIndex++) {
      filteredIndices_.push_back(familyIndex);
    }
    return;
  }

  const uint32_t groupBit = uint32_t{1} << (groupListIndex - 1);
  for (int familyIndex = 0; familyIndex < static_cast<int>(families_.size()); familyIndex++) {
    if (families_[familyIndex].scriptMask & groupBit) filteredIndices_.push_back(familyIndex);
  }
}

void FontDownloadActivity::enterGroup(const int groupListIndex) {
  closeRouting();
  buildFilteredIndices(groupListIndex);
  {
    RenderLock lock(*this);
    nav.reset();
    state_ = FAMILY_LIST;
    rowsDirty_ = true;
  }
}

size_t FontDownloadActivity::totalDownloadSize() const {
  size_t total = 0;
  for (const int familyIndex : filteredIndices_) {
    if (!families_[familyIndex].installed) total += families_[familyIndex].totalSize;
  }
  return total;
}

size_t FontDownloadActivity::totalUpdateSize() const {
  size_t total = 0;
  for (const int familyIndex : filteredIndices_) {
    if (families_[familyIndex].hasUpdate) total += families_[familyIndex].totalSize;
  }
  return total;
}

// Standard CRC32 matching zlib/Python zlib.crc32().
bool FontDownloadActivity::computeFileCrc32(const char* path, uint32_t& outCrc) {
  HalFile f;
  if (!Storage.openFileForRead("FONT", path, f)) {
    return false;
  }
  constexpr size_t BUF_SIZE = 128;
  uint8_t buf[BUF_SIZE];
  uint32_t crc = 0;
  while (f.available()) {
    const int n = f.read(buf, BUF_SIZE);
    if (n <= 0) break;
    crc = esp_rom_crc32_le(crc, buf, static_cast<uint32_t>(n));
  }
  outCrc = crc;
  return true;
}

void FontDownloadActivity::downloadFamily(ManifestFamily& family) {
  {
    RenderLock lock(*this);
    state_ = DOWNLOADING;
    downloadingFamilyIndex_ = static_cast<int>(&family - families_.data());
    fileProgress_ = 0;
    fileTotal_ = 0;
    cancelRequested_ = false;
    goHomeRequested_ = false;
  }
  requestUpdateAndWait();

  // Rebuildable SD-font caches (glyph/kern arenas, CJK fallback tables) can
  // hold tens of KB the TLS session needs; release them up front rather than
  // starving the transfer. They repopulate on demand after the download. Under
  // RenderLock like the other release sites: the slabs it frees are the ones the
  // render task reads, and the progress callback below renders as the body
  // streams.
  {
    RenderLock lock(*this);
    if (auto* fcm = renderer.getFontCacheManager()) {
      fcm->releaseAllFontMemory();
      LOG_DBG("FONT", "Free heap after SD font cache release: %u bytes", ESP.getFreeHeap());
    }
  }

  // Check before touching the family directory so a failed update leaves the
  // installed family unchanged.
  if (ESP.getFreeHeap() < HttpDownloader::MIN_TLS_FREE_HEAP ||
      ESP.getMaxAllocHeap() < HttpDownloader::MIN_TLS_MAX_ALLOC) {
    LOG_ERR("FONT", "Low heap for download (%u free, %u max block)", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    RenderLock lock(*this);
    state_ = ERROR;
    errorMessage_ = tr(STR_LOW_MEMORY_RETRY);
    return;
  }

  if (!fontInstaller_.ensureFamilyDir(str(family.name))) {
    RenderLock lock(*this);
    state_ = ERROR;
    errorMessage_ = "Failed to create font directory";
    return;
  }

  for (uint32_t i = 0; i < family.fileCount; i++) {
    const ManifestFile& file = files_[family.fileStart + i];

    {
      RenderLock lock(*this);
      fileProgress_ = 0;
      fileTotal_ = file.size;
    }
    requestUpdateAndWait();

    char destPath[128];
    FontInstaller::buildFontPath(str(family.name), str(file.name), destPath, sizeof(destPath));

    downloadUrl_.assign(baseUrl_).append(str(file.name));

    auto result = HttpDownloader::downloadToFile(
        downloadUrl_, destPath,
        [this](size_t downloaded, size_t total) {
          fileProgress_ = downloaded;
          fileTotal_ = total;
          mappedInput.update();
          if (mappedInput.isPressed(MappedInputManager::Button::Back) ||
              mappedInput.wasPressed(MappedInputManager::Button::Back)) {
            cancelRequested_ = true;
          }
          // This update() consumes the one-shot home event before the central
          // ActivityManager dispatch can see it, so honor it here: abort the
          // download, then exit to home once the abort unwinds.
          if (mappedInput.wasHomeGesture()) {
            cancelRequested_ = true;
            goHomeRequested_ = true;
          }
          requestUpdate(true);
        },
        // Redirects stay on HTTPS: CRC32 (below) catches transmission errors
        // but not a deliberate substitution by an on-path attacker, who could
        // serve a malicious .cpfont over a downgraded HTTP hop with a forged
        // CRC32 to match. HAVE_MAX_FRAGMENT's 2KB TLS records already remove
        // most of the second TLS session's heap cost, so the C3 doesn't need
        // the HTTP downgrade to stay out of MEMORY_E territory here.
        &cancelRequested_, "", "", /*downgradeRedirectsToHttp=*/false);

    if (result == HttpDownloader::ABORTED) {
      fontInstaller_.deleteFamily(str(family.name));
      family.installed = false;
      family.hasUpdate = false;
      if (goHomeRequested_) {
        onGoHome();
        return;
      }
      {
        RenderLock lock(*this);
        state_ = FAMILY_LIST;
        rowsDirty_ = true;  // installed/hasUpdate just changed above
      }
      return;
    }

    if (result != HttpDownloader::OK) {
      LOG_ERR("FONT", "Download failed: %s (%d)", str(file.name), result);
      fontInstaller_.deleteFamily(str(family.name));
      family.installed = false;
      family.hasUpdate = false;
      RenderLock lock(*this);
      state_ = ERROR;
      errorMessage_ = std::string("Download failed: ") + str(file.name);
      return;
    }

    uint32_t actualCrc = 0;
    if (!computeFileCrc32(destPath, actualCrc)) {
      LOG_ERR("FONT", "Failed to open file for CRC check: %s", destPath);
      fontInstaller_.deleteFamily(str(family.name));
      family.installed = false;
      family.hasUpdate = false;
      RenderLock lock(*this);
      state_ = ERROR;
      errorMessage_ = std::string("Failed to compute checksum: ") + str(file.name);
      return;
    }
    if (actualCrc != file.crc32) {
      LOG_ERR("FONT", "CRC32 mismatch for %s: got %08x expected %08x", str(file.name), actualCrc, file.crc32);
      fontInstaller_.deleteFamily(str(family.name));
      family.installed = false;
      family.hasUpdate = false;
      RenderLock lock(*this);
      state_ = ERROR;
      errorMessage_ = std::string("Checksum mismatch: ") + str(file.name);
      return;
    }
    LOG_DBG("FONT", "Downloaded %s (size=%u crc32=%08x)", str(file.name), file.size, actualCrc);

    if (!fontInstaller_.validateCpfontFile(destPath)) {
      LOG_ERR("FONT", "Invalid .cpfont: %s", destPath);
      fontInstaller_.deleteFamily(str(family.name));
      family.installed = false;
      family.hasUpdate = false;
      RenderLock lock(*this);
      state_ = ERROR;
      errorMessage_ = std::string("Invalid font file: ") + str(file.name);
      return;
    }
    currentFileIndex_++;
  }

  fontInstaller_.refreshRegistry();
  family.installed = true;
  family.hasUpdate = false;

  {
    RenderLock lock(*this);
    state_ = COMPLETE;
  }
}

void FontDownloadActivity::promptDeleteSelectedFamily() {
  const int pendingDeleteFamilyIndex = familyIndexFromList(nav.selected);
  if (pendingDeleteFamilyIndex < 0 || pendingDeleteFamilyIndex >= static_cast<int>(families_.size())) {
    return;
  }

  std::string heading = tr(STR_DELETE);
  const auto& family = families_[pendingDeleteFamilyIndex];
  std::string body = str(family.name);
  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, body),
                         [this](const ActivityResult& result) { onDeleteConfirmationResult(result); });
}

void FontDownloadActivity::onDeleteConfirmationResult(const ActivityResult& result) {
  if (result.isCancelled) {
    requestUpdate();
    return;
  }

  const int familyIndex = familyIndexFromList(nav.selected);
  if (familyIndex < 0) {
    requestUpdate();
    return;
  }
  auto& family = families_[familyIndex];

  if (fontInstaller_.deleteFamily(str(family.name)) != FontInstaller::Error::OK) {
    RenderLock lock(*this);
    state_ = ERROR;
    errorMessage_ = "Failed to delete font";
  } else {
    fontInstaller_.refreshRegistry();
    family.installed = false;
    family.hasUpdate = false;
    // Unlike the other family_ mutations, this one stays in FAMILY_LIST (no
    // state_ transition to hang the rebuild off), so it must set the flag
    // directly.
    rowsDirty_ = true;
  }

  requestUpdate();
}

bool FontDownloadActivity::isSelectedFamilyDeletable() const {
  if (isDownloadAllRow(nav.selected) || isUpdateAllRow(nav.selected)) return false;
  if (nav.selected < specialRowCount() || nav.selected >= listItemCount()) return false;
  const auto& family = families_[familyIndexFromList(nav.selected)];
  return family.installed && !family.hasUpdate;
}

void FontDownloadActivity::activateSelected() {
  if (filteredIndices_.empty()) return;
  if (isDownloadAllRow(nav.selected)) {
    currentFileIndex_ = 0;
    currentFileTotal_ = 0;
    for (const int familyIndex : filteredIndices_) {
      if (!families_[familyIndex].installed) currentFileTotal_ += families_[familyIndex].fileCount;
    }
    downloadAll();
  } else if (isUpdateAllRow(nav.selected)) {
    currentFileIndex_ = 0;
    currentFileTotal_ = 0;
    for (const int familyIndex : filteredIndices_) {
      if (families_[familyIndex].hasUpdate) currentFileTotal_ += families_[familyIndex].fileCount;
    }
    updateAll();
  } else {
    // The special rows disappear when a download starts, so a stale selection
    // can map past the family table.
    const int familyIndex = familyIndexFromList(nav.selected);
    if (familyIndex < 0 || familyIndex >= static_cast<int>(families_.size())) return;
    auto& family = families_[familyIndex];
    if (!family.installed || family.hasUpdate) {
      currentFileIndex_ = 0;
      currentFileTotal_ = family.fileCount;
      downloadFamily(family);
    } else {
      promptDeleteSelectedFamily();
      return;
    }
  }
  requestUpdateAndWait();
}

void FontDownloadActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the GUI.drawHeader band, above the button hints.
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (state_ == FAMILY_LIST && filteredIndices_.empty()) {
    screen.centeredText(tr(STR_NO_FONTS_AVAILABLE), screen.theme().bodyText);
    return;
  }

  if (rowsDirty_) {
    rebuildRowItems();
    rowsDirty_ = false;
  }

  fui::ListProps props;
  props.items = rowItems_.data();
  props.count = static_cast<uint16_t>(rowItems_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;               // air between the status and the row edge
  syncListViewport(screen, props, /*hasSubtitle=*/state_ == FAMILY_LIST);
  screen.list(props);
}

void FontDownloadActivity::rebuildRowItems() {
  switch (state_) {
    case GROUP_LIST:
      rebuildGroupRowItems();
      return;
    case FAMILY_LIST:
      rebuildFamilyRowItems();
      return;
    case WIFI_SELECTION:
    case LOADING_MANIFEST:
    case DOWNLOADING:
    case COMPLETE:
    case ERROR:
      rowLabels_.clear();
      rowItems_.clear();
      return;
  }
}

void FontDownloadActivity::rebuildGroupRowItems() {
  const int listSize = groupListItemCount();
  rowLabels_.assign(listSize, std::string());
  rowItems_.clear();
  rowItems_.reserve(listSize);
  for (int rowIndex = 0; rowIndex < listSize; rowIndex++) {
    fui::ListItem item;
    item.label = rowIndex == 0 ? tr(STR_ALL_FONTS) : str(scriptGroupLabels_[rowIndex - 1]);
    const int memberCount = rowIndex == 0 ? static_cast<int>(families_.size()) : groupMemberCount(rowIndex - 1);
    rowLabels_[rowIndex] = std::to_string(memberCount);
    item.value = rowLabels_[rowIndex].c_str();
    item.actionValue = static_cast<int16_t>(rowIndex);
    rowItems_.push_back(item);
  }
}

void FontDownloadActivity::rebuildFamilyRowItems() {
  const int listSize = listItemCount();
  rowLabels_.assign(listSize, std::string());
  rowItems_.clear();
  rowItems_.reserve(listSize);
  for (int i = 0; i < listSize; i++) {
    fui::ListItem item;
    if (isDownloadAllRow(i)) {
      rowLabels_[i] = std::string(tr(STR_DOWNLOAD_ALL)) + " (" + formatSize(totalDownloadSize()) + ")";
      item.label = rowLabels_[i].c_str();
    } else if (isUpdateAllRow(i)) {
      rowLabels_[i] = std::string(tr(STR_UPDATE_ALL)) + " (" + formatSize(totalUpdateSize()) + ")";
      item.label = rowLabels_[i].c_str();
    } else {
      const auto& family = families_[familyIndexFromList(i)];
      item.label = str(family.name);
      if (family.description != 0) item.subtitle = str(family.description);
      if (family.hasUpdate) {
        item.value = tr(STR_UPDATE_AVAILABLE);
      } else if (family.installed) {
        item.value = tr(STR_INSTALLED);
        // Dimmed but still tappable (opens the delete prompt): visual-only
        // disabled state, the row stays enabled for hit registration.
        item.state = fui::StateDisabled;
      }
    }
    item.actionValue = static_cast<int16_t>(i);
    rowItems_.push_back(item);
  }
}

// --- Input handling ---

bool FontDownloadActivity::handleCustomInput() {
  if (state_ == GROUP_LIST || state_ == FAMILY_LIST) {
    // The base list protocol (Back/Confirm, touch routing, swipe scroll,
    // button navigation) handles both list states.
    return false;
  }

  if (state_ == COMPLETE) {
    int x = 0;
    int y = 0;
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
      {
        RenderLock lock(*this);
        state_ = FAMILY_LIST;
        rowsDirty_ = true;  // the completed download changed installed/hasUpdate
      }
      requestUpdate();
    }
  } else if (state_ == ERROR) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      {
        RenderLock lock(*this);
        state_ = FAMILY_LIST;
        rowsDirty_ = true;  // the failed download reset installed/hasUpdate
      }
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (downloadingFamilyIndex_ >= 0 && downloadingFamilyIndex_ < static_cast<int>(families_.size())) {
        downloadFamily(families_[downloadingFamilyIndex_]);
        requestUpdateAndWait();
        return true;
      } else {
        {
          RenderLock lock(*this);
          state_ = FAMILY_LIST;
          rowsDirty_ = true;
        }
        requestUpdate();
      }
    } else {
      int x = 0;
      int y = 0;
      if (mappedInput.wasScreenTapped(x, y)) {
        if (downloadingFamilyIndex_ >= 0 && downloadingFamilyIndex_ < static_cast<int>(families_.size())) {
          downloadFamily(families_[downloadingFamilyIndex_]);
          requestUpdateAndWait();
          return true;
        }
        {
          RenderLock lock(*this);
          state_ = FAMILY_LIST;
          rowsDirty_ = true;
        }
        requestUpdate();
      }
    }
  }

  return true;
}

// --- Rendering ---

std::string FontDownloadActivity::formatSize(size_t bytes) {
  char buf[32];
  if (bytes >= 1024 * 1024) {
    snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else if (bytes >= 1024) {
    snprintf(buf, sizeof(buf), "%.0f KB", static_cast<double>(bytes) / 1024.0);
  } else {
    snprintf(buf, sizeof(buf), "%zu B", bytes);
  }
  return buf;
}

void FontDownloadActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  const char* headerSubtitle = nullptr;
  if (state_ == FAMILY_LIST && hasGroupScreen()) {
    const int scriptGroupIndex = groupNav_.selected - 1;
    headerSubtitle = scriptGroupIndex >= 0 && scriptGroupIndex < static_cast<int>(scriptGroupLabels_.size())
                         ? str(scriptGroupLabels_[scriptGroupIndex])
                         : tr(STR_ALL_FONTS);
  }
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_FONT_BROWSER),
                 headerSubtitle);

  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const auto centerY = (pageHeight - lineHeight) / 2;

  if (state_ == LOADING_MANIFEST) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_LOADING_FONT_LIST));
  } else if (state_ == GROUP_LIST) {
    renderUi();
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == FAMILY_LIST) {
    renderUi();

    const bool hasVisibleFamilies = !filteredIndices_.empty();
    const char* confirmLabel = !hasVisibleFamilies            ? ""
                               : isSelectedFamilyDeletable()  ? tr(STR_DELETE)
                               : isUpdateAllRow(nav.selected) ? tr(STR_UPDATE)
                                                              : tr(STR_DOWNLOAD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, hasVisibleFamilies ? tr(STR_DIR_UP) : "",
                                              hasVisibleFamilies ? tr(STR_DIR_DOWN) : "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == DOWNLOADING) {
    const auto& family = families_[downloadingFamilyIndex_];

    std::string statusText = std::string(tr(STR_DOWNLOADING)) + " " + str(family.name) + " (" +
                             std::to_string(currentFileIndex_ + 1) + "/" + std::to_string(currentFileTotal_) + ")";
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, statusText.c_str());

    float progress = 0;
    if (fileTotal_ > 0) {
      progress = static_cast<float>(fileProgress_) / static_cast<float>(fileTotal_);
    }

    int barY = centerY + metrics.verticalSpacing;
    GUI.drawProgressBar(
        renderer,
        Rect{metrics.contentSidePadding, barY, pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
        static_cast<int>(progress * 100), 100);

    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == COMPLETE) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_FONT_INSTALLED), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, tr(STR_FONT_INSTALL_FAILED), true,
                              EpdFontFamily::BOLD);
    if (!errorMessage_.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, centerY + metrics.verticalSpacing, errorMessage_.c_str());
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
