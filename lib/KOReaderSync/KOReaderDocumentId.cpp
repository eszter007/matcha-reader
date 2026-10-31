#include "KOReaderDocumentId.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <MD5Builder.h>
#include <strings.h>

#include <string_view>

namespace {
// Extract filename from path (everything after last '/')
// A .matcha volume is keyed like the folder it was packed from, so both forms share progress.
std::string getFilename(const std::string& path) {
  const size_t pos = path.rfind('/');
  std::string name = pos == std::string::npos ? path : path.substr(pos + 1);
  name.resize(FsHelpers::stripMatchaExtension(name).size());
  return name;
}
std::string md5Of(const std::string_view name, const std::string_view suffix) {
  MD5Builder md5;
  md5.begin();
  md5.add(reinterpret_cast<const uint8_t*>(name.data()), name.size());
  if (!suffix.empty()) md5.add(reinterpret_cast<const uint8_t*>(suffix.data()), suffix.size());
  md5.calculate();
  return md5.toString().c_str();
}

constexpr std::string_view TWIN_EXTENSIONS[KOReaderDocumentId::MAX_TWINS] = {".epub", ".cbz", ".pdf", ".xtc", ".xtch"};

bool endsWithNoCase(const std::string_view s, const std::string_view suffix) {
  return s.size() > suffix.size() &&
         strncasecmp(s.data() + s.size() - suffix.size(), suffix.data(), suffix.size()) == 0;
}
}  // namespace

size_t KOReaderDocumentId::twinFilenameHashes(const std::string& filePath, std::string (&hashes)[MAX_TWINS]) {
  const size_t slash = filePath.rfind('/');
  std::string_view name = std::string_view(filePath).substr(slash == std::string::npos ? 0 : slash + 1);
  for (const auto ext : TWIN_EXTENSIONS) {
    if (endsWithNoCase(name, ext)) {
      hashes[0] = md5Of(name.substr(0, name.size() - ext.size()), {});
      return 1;
    }
  }
  if (FsHelpers::hasMatchaExtension(name)) {
    name = FsHelpers::stripMatchaExtension(name);
  } else if (FsHelpers::hasReflowableBookExtension(name) || FsHelpers::hasImageExtension(name)) {
    return 0;  // a book file, not a manga folder (whose name may contain dots: "Vol. 1")
  }
  if (name.empty()) return 0;
  for (size_t i = 0; i < MAX_TWINS; i++) hashes[i] = md5Of(name, TWIN_EXTENSIONS[i]);
  return MAX_TWINS;
}

std::string KOReaderDocumentId::calculateFromFilename(const std::string& filePath) {
  const std::string filename = getFilename(filePath);
  if (filename.empty()) {
    return "";
  }

  MD5Builder md5;
  md5.begin();
  md5.add(filename.c_str());
  md5.calculate();

  std::string result = md5.toString().c_str();
  LOG_DBG("KODoc", "Filename hash: %s (from '%s')", result.c_str(), filename.c_str());
  return result;
}

size_t KOReaderDocumentId::getOffset(int i) {
  // Offset = 1024 << (2*i)
  // For i = -1: KOReader uses a value of 0
  // For i >= 0: 1024 << (2*i)
  if (i < 0) {
    return 0;
  }
  return CHUNK_SIZE << (2 * i);
}

std::string KOReaderDocumentId::calculate(const std::string& filePath) {
  HalFile file;
  if (!Storage.openFileForRead("KODoc", filePath, file)) {
    LOG_DBG("KODoc", "Failed to open file: %s", filePath.c_str());
    return "";
  }
  // A manga goes by its title: a folder has no content to sample (its 0 bytes would give every folder
  // the same id), and a .matcha must match the folder it was packed from.
  if (file.isDirectory() || FsHelpers::hasMatchaExtension(filePath)) return "";

  const size_t fileSize = file.fileSize();
  LOG_DBG("KODoc", "Calculating hash for file: %s (size: %zu)", filePath.c_str(), fileSize);

  // Initialize MD5 builder
  MD5Builder md5;
  md5.begin();

  // Buffer for reading chunks
  uint8_t buffer[CHUNK_SIZE];
  size_t totalBytesRead = 0;

  // Read from each offset (i = -1 to 10)
  for (int i = -1; i < OFFSET_COUNT - 1; i++) {
    const size_t offset = getOffset(i);

    // Skip if offset is beyond file size
    if (offset >= fileSize) {
      continue;
    }

    // Seek to offset
    if (!file.seekSet(offset)) {
      LOG_DBG("KODoc", "Failed to seek to offset %zu", offset);
      continue;
    }

    // Read up to CHUNK_SIZE bytes
    const size_t bytesToRead = std::min(CHUNK_SIZE, fileSize - offset);
    const size_t bytesRead = file.read(buffer, bytesToRead);

    if (bytesRead > 0) {
      md5.add(buffer, bytesRead);
      totalBytesRead += bytesRead;
    }
  }

  // Calculate final hash
  md5.calculate();
  std::string result = md5.toString().c_str();

  LOG_DBG("KODoc", "Hash calculated: %s (from %zu bytes)", result.c_str(), totalBytesRead);

  return result;
}
