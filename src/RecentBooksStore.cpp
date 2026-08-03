#include "RecentBooksStore.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Xtc.h>

#include <algorithm>
#include <array>
#include <cstring>

namespace {
constexpr size_t LIBRARY_CACHE_MAX_BOOKS = 2048;

class BufferedHalFileReader {
 public:
  explicit BufferedHalFileReader(HalFile& file) : file_(file) {}

  int read() {
    if (position_ == size_ && !refill()) return -1;
    return buffer_[position_++];
  }

  size_t readBytes(char* output, size_t length) {
    size_t copied = 0;
    while (copied < length) {
      if (position_ == size_ && !refill()) break;
      const size_t available = size_ - position_;
      const size_t count = std::min(available, length - copied);
      std::memcpy(output + copied, buffer_.data() + position_, count);
      position_ += count;
      copied += count;
    }
    return copied;
  }

 private:
  bool refill() {
    const int count = file_.read(buffer_.data(), buffer_.size());
    if (count <= 0) return false;
    position_ = 0;
    size_ = static_cast<size_t>(count);
    return true;
  }

  HalFile& file_;
  std::array<uint8_t, 512> buffer_{};
  size_t position_ = 0;
  size_t size_ = 0;
};

class BufferedHalFileWriter final : public Print {
 public:
  explicit BufferedHalFileWriter(HalFile& file) : file_(file) {}

  size_t write(uint8_t value) override { return write(&value, 1); }

  size_t write(const uint8_t* input, size_t length) override {
    size_t accepted = 0;
    while (accepted < length && ok_) {
      const size_t count = std::min(buffer_.size() - size_, length - accepted);
      std::memcpy(buffer_.data() + size_, input + accepted, count);
      size_ += count;
      accepted += count;
      if (size_ == buffer_.size()) flushBuffer();
    }
    return accepted;
  }

  bool finish() {
    flushBuffer();
    if (ok_) file_.flush();
    return ok_;
  }

 private:
  void flushBuffer() {
    if (!ok_ || size_ == 0) return;
    ok_ = file_.write(buffer_.data(), size_) == size_;
    size_ = 0;
  }

  HalFile& file_;
  std::array<uint8_t, 512> buffer_{};
  size_t size_ = 0;
  bool ok_ = true;
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
  {
    HalFile file;
    if (!Storage.openFileForWrite("RBS", tmpPath, file)) return false;
    BufferedHalFileWriter output(file);

    constexpr char PREFIX[] = "{\"books\":[";
    if (output.write(reinterpret_cast<const uint8_t*>(PREFIX), sizeof(PREFIX) - 1) != sizeof(PREFIX) - 1) return false;

    JsonDocument record;
    bool first = true;
    for (const auto& book : books) {
      if (!first && output.write(static_cast<uint8_t>(',')) != 1) return false;
      first = false;
      record.clear();
      record["path"] = book.path;
      record["title"] = book.title;
      record["author"] = book.author;
      record["coverBmpPath"] = book.coverBmpPath;
      if (serializeJson(record, output) != measureJson(record)) return false;
    }

    constexpr char SUFFIX[] = "]}";
    if (output.write(reinterpret_cast<const uint8_t*>(SUFFIX), sizeof(SUFFIX) - 1) != sizeof(SUFFIX) - 1) return false;
    if (!output.finish()) return false;
  }

  Storage.remove(path);
  if (!Storage.rename(tmpPath.c_str(), path)) {
    LOG_ERR("RBS", "Failed to replace %s", path);
    return false;
  }
  return true;
}

bool RecentBooksStore::loadFromPath(const char* path) {
  if (!Storage.exists(path)) return false;
  HalFile file;
  if (!Storage.openFileForRead("RBS", path, file)) return false;
  BufferedHalFileReader input(file);
  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, input);
  if (error) {
    LOG_ERR("RBS", "JSON parse error in %s: %s", path, error.c_str());
    return false;
  }
  return fromJson(doc.as<JsonVariantConst>(), LIBRARY_CACHE_MAX_BOOKS);
}
