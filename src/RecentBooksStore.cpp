#include "RecentBooksStore.h"

#include <BufferedFile.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Xtc.h>

#include <algorithm>

namespace {
// [HEIGHT] is the ONLY placeholder this build knows how to fill (UITheme::getCoverThumbPath).
// Older firmware wrote a [WIDTH]x[HEIGHT] template; substituting just [HEIGHT] leaves a literal
// "[WIDTH]" in the filename, so the path can never exist and every Home and Library pass re-probes
// it forever (X3 device log: ".../epub_5391773237008108432/thumb_[WIDTH]x231.bmp", alongside a
// 19-digit 64-bit hash this build also no longer produces).
//
// Only the PATH is dropped, never the book: with no cover path the ordinary generator writes a
// current-format thumb on the next pass, which is also what repairs the stale directory name.
bool coverPathResolvable(const std::string& coverBmpPath) {
  std::string rest = coverBmpPath;
  for (size_t at = rest.find("[HEIGHT]"); at != std::string::npos; at = rest.find("[HEIGHT]")) {
    rest.erase(at, 8);
  }
  return rest.find('[') == std::string::npos && rest.find(']') == std::string::npos;
}

constexpr size_t LIBRARY_CACHE_MAX_BOOKS = 2048;

class JsonFileReader {
 public:
  explicit JsonFileReader(HalFile& file) : input_(file, 512) {}

  int read() {
    uint8_t value = 0;
    return input_.read(&value, 1) == 1 ? value : -1;
  }

  size_t readBytes(char* output, size_t length) { return input_.read(output, length); }

 private:
  serialization::BufferedFileReader input_;
};

class JsonFileWriter final : public Print {
 public:
  explicit JsonFileWriter(HalFile& file) : output_(file, 512) {}

  size_t write(uint8_t value) override { return write(&value, 1); }

  size_t write(const uint8_t* input, size_t length) override {
    output_.write(input, length);
    return length;
  }

  bool finish() { return output_.flush(); }

 private:
  serialization::BufferedFileWriter output_;
};
}  // namespace

void RecentBooksStore::toJson(JsonDocument& doc) const {
  JsonArray arr = doc["books"].to<JsonArray>();
  for (const auto& book : recentBooks) {
    JsonObject obj = arr.add<JsonObject>();
    obj["path"] = book.path;
    obj["title"] = book.title;
    obj["author"] = book.author;
    obj["coverBmpPath"] = book.coverBmpPath;
  }
}

bool RecentBooksStore::fromJson(JsonVariantConst doc) { return fromJson(doc, MAX_RECENT_BOOKS); }

bool RecentBooksStore::fromJson(JsonVariantConst doc, const size_t maxBooks) {
  // Tolerate a missing/invalid 'books' key (treat as empty list); only a
  // JSON parse error is fatal. A null JsonArray iterates zero times.
  recentBooks.clear();
  JsonArrayConst arr = doc["books"].as<JsonArrayConst>();
  recentBooks.reserve(std::min(arr.size(), maxBooks));
  for (JsonObjectConst obj : arr) {
    if (recentBooks.size() >= maxBooks) break;
    RecentBook book;
    book.path = obj["path"] | "";
    book.title = obj["title"] | "";
    book.author = obj["author"] | "";
    book.coverBmpPath = obj["coverBmpPath"] | "";
    if (!coverPathResolvable(book.coverBmpPath)) {
      LOG_DBG("RBS", "Dropping unresolvable cover path %s", book.coverBmpPath.c_str());
      book.coverBmpPath.clear();
    }
    recentBooks.push_back(std::move(book));
  }

  LOG_DBG("RBS", "Recent books loaded from file (%d entries)", getCount());
  return true;
}

void RecentBooksStore::addBook(const std::string& path, const std::string& title, const std::string& author,
                               const std::string& coverBmpPath) {
  // Drop stale entries first so a new add can't evict a valid book in their stead.
  pruneMissing();

  // Remove existing entry if present
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it != recentBooks.end()) {
    recentBooks.erase(it);
  }

  // Add to front
  recentBooks.insert(recentBooks.begin(), {path, title, author, coverBmpPath});

  // Trim to max size
  if (recentBooks.size() > MAX_RECENT_BOOKS) {
    recentBooks.resize(MAX_RECENT_BOOKS);
  }

  saveToFile();
}

void RecentBooksStore::updateBook(const std::string& path, const std::string& title, const std::string& author,
                                  const std::string& coverBmpPath) {
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it != recentBooks.end()) {
    RecentBook& book = *it;
    book.title = title;
    book.author = author;
    book.coverBmpPath = coverBmpPath;
    saveToFile();
  }
}

bool RecentBooksStore::removeByPath(const std::string& path) {
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it == recentBooks.end()) {
    return false;
  }
  recentBooks.erase(it);
  if (!saveToFile()) {
    LOG_ERR("RBS", "Failed to persist removal of recent book: %s", path.c_str());
  }
  return true;
}

void RecentBooksStore::updatePath(const std::string& oldPath, const std::string& newPath,
                                  const std::string& oldCachePath, const std::string& newCachePath) {
  auto it = std::find_if(recentBooks.begin(), recentBooks.end(),
                         [&](const RecentBook& book) { return book.path == oldPath; });
  if (it == recentBooks.end()) {
    return;
  }
  it->path = newPath;
  if (!oldCachePath.empty() && !it->coverBmpPath.empty() && it->coverBmpPath.rfind(oldCachePath, 0) == 0) {
    it->coverBmpPath = newCachePath + it->coverBmpPath.substr(oldCachePath.size());
  }
  saveToFile();
}

bool RecentBooksStore::isMissing(const RecentBook& book) { return !Storage.exists(book.path.c_str()); }

bool RecentBooksStore::pruneMissing() {
  const size_t before = recentBooks.size();
  recentBooks.erase(std::remove_if(recentBooks.begin(), recentBooks.end(), &isMissing), recentBooks.end());
  return recentBooks.size() != before;
}

RecentBook RecentBooksStore::getDataFromBook(std::string path) const {
  std::string lastBookFileName = "";
  const size_t lastSlash = path.find_last_of('/');
  if (lastSlash != std::string::npos) {
    lastBookFileName = path.substr(lastSlash + 1);
  }

  LOG_DBG("RBS", "Loading recent book: %s", path.c_str());

  // If epub, try to load the metadata for title/author and cover.
  // Use buildIfMissing=false to avoid heavy epub loading on boot; getTitle()/getAuthor() may be
  // blank until the book is opened, and entries with missing title are omitted from recent list.
  if (FsHelpers::hasEpubExtension(lastBookFileName)) {
    Epub epub(path, "/.crosspoint");
    epub.load(false, true);
    return RecentBook{path, epub.getTitle(), epub.getAuthor(), epub.getThumbBmpPath()};
  } else if (FsHelpers::hasXtcExtension(lastBookFileName)) {
    // Handle XTC file
    Xtc xtc(path, "/.crosspoint");
    if (xtc.load()) {
      return RecentBook{path, xtc.getTitle(), xtc.getAuthor(), xtc.getThumbBmpPath()};
    }
  } else if (FsHelpers::hasTxtExtension(lastBookFileName) || FsHelpers::hasMarkdownExtension(lastBookFileName)) {
    return RecentBook{path, lastBookFileName, "", ""};
  }
  return RecentBook{path, "", "", ""};
}

bool RecentBooksStore::saveBooksToPath(const std::vector<RecentBook>& books, const char* path) {
  Storage.mkdir("/.crosspoint");
  const std::string tmpPath = std::string(path) + ".tmp";
  bool written = false;
  {
    HalFile file;
    if (!Storage.openFileForWrite("RBS", tmpPath, file)) return false;
    JsonFileWriter output(file);

    constexpr char PREFIX[] = "{\"books\":[";
    output.write(reinterpret_cast<const uint8_t*>(PREFIX), sizeof(PREFIX) - 1);

    JsonDocument record;
    bool first = true;
    for (const auto& book : books) {
      if (!first) output.write(static_cast<uint8_t>(','));
      first = false;
      record.clear();
      record["path"] = book.path;
      record["title"] = book.title;
      record["author"] = book.author;
      record["coverBmpPath"] = book.coverBmpPath;
      serializeJson(record, output);
    }

    constexpr char SUFFIX[] = "]}";
    output.write(reinterpret_cast<const uint8_t*>(SUFFIX), sizeof(SUFFIX) - 1);
    written = output.finish();
  }
  if (!written) {
    Storage.remove(tmpPath.c_str());
    return false;
  }

  const std::string backupPath = std::string(path) + ".bak";
  if (!Storage.exists(path) && Storage.exists(backupPath.c_str()) && !Storage.rename(backupPath.c_str(), path)) {
    LOG_ERR("RBS", "Failed to restore %s before replacing it", path);
    Storage.remove(tmpPath.c_str());
    return false;
  }
  Storage.remove(backupPath.c_str());
  if (Storage.exists(path) && !Storage.rename(path, backupPath.c_str())) {
    LOG_ERR("RBS", "Failed to back up %s before replacing it", path);
    Storage.remove(tmpPath.c_str());
    return false;
  }
  if (!Storage.rename(tmpPath.c_str(), path)) {
    LOG_ERR("RBS", "Failed to replace %s", path);
    if (Storage.exists(backupPath.c_str()) && !Storage.rename(backupPath.c_str(), path)) {
      LOG_ERR("RBS", "Previous cache remains at %s", backupPath.c_str());
    }
    Storage.remove(tmpPath.c_str());
    return false;
  }
  Storage.remove(backupPath.c_str());
  return true;
}

bool RecentBooksStore::loadFromPath(const char* path) {
  if (!Storage.exists(path)) return false;
  HalFile file;
  if (!Storage.openFileForRead("RBS", path, file)) return false;
  JsonFileReader input(file);
  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, input);
  if (error) {
    LOG_ERR("RBS", "JSON parse error in %s: %s", path, error.c_str());
    return false;
  }
  return fromJson(doc.as<JsonVariantConst>(), LIBRARY_CACHE_MAX_BOOKS);
}
