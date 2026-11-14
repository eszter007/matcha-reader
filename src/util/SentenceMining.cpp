#include "SentenceMining.h"

#include <HalClock.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>
#include <ctime>

#include "TaskWatchdog.h"

namespace sentencemining {

bool upgradeExport(const std::string& path) {
  // The last file found current: saving a word checks its file again only when it changes. An
  // old-layout file copied over it during the same session is not seen until the next boot.
  static std::string currentPath;
  if (path == currentPath) return true;
  const std::string tmp = path + ".tmp";
  // A copy is only written while the original exists and only replaces it once complete, so a
  // .tmp without its original is a finished upgrade whose swap was cut off.
  if (!Storage.exists(path.c_str()) && Storage.exists(tmp.c_str()) && !Storage.rename(tmp.c_str(), path.c_str())) {
    LOG_ERR("MINE", "Could not restore %s", tmp.c_str());
    return false;
  }
  HalFile in;
  if (!Storage.exists(path.c_str())) return true;  // a new file gets the current header
  if (!Storage.openFileForRead("MINE", path.c_str(), in)) return false;
  char head[160];
  const int headLen = in.read(head, sizeof(head));
  if (headLen < 0) return false;
  if (!isLegacyHeader(std::string_view(head, static_cast<size_t>(headLen)))) {
    currentPath = path;
    return true;
  }
  // Heap, not stack: one buffer for the whole file, freed on return. Taken before the copy is
  // created, so low memory leaves nothing behind on the card.
  constexpr size_t CHUNK = 2048;
  auto buf = makeUniqueNoThrow<char[]>(CHUNK);
  if (!buf) {
    LOG_ERR("MINE", "OOM: %u bytes for the CSV upgrade", static_cast<unsigned>(CHUNK));
    return false;
  }
  const uint64_t expected = in.fileSize64();
  in.seekSet(0);
  bool ok = true;
  {
    HalFile out;
    if (!Storage.openFileForWrite("MINE", tmp.c_str(), out)) return false;
    std::string migrated = headerLines();
    migrated.reserve(CHUNK + CHUNK / 8 + 256);
    LegacyRowMigrator migrator;
    uint64_t copied = 0;
    int n = 0;
    while (ok && (n = in.read(buf.get(), CHUNK)) > 0) {
      copied += static_cast<uint64_t>(n);
      migrator.feed(std::string_view(buf.get(), static_cast<size_t>(n)), migrated);
      ok = out.write(reinterpret_cast<const uint8_t*>(migrated.data()), migrated.size()) == migrated.size();
      migrated.clear();
      resetTaskWatchdogIfSubscribed();
    }
    // A read error ends the loop like the end of the file does: only a full copy may replace it.
    ok = ok && n == 0 && copied == expected;
  }
  in.close();
  if (!ok || !Storage.replaceFile(tmp.c_str(), path.c_str())) {
    LOG_ERR("MINE", "Could not upgrade %s to the Furigana column", path.c_str());
    if (Storage.exists(path.c_str())) Storage.remove(tmp.c_str());
    return false;
  }
  currentPath = path;
  LOG_INF("MINE", "Upgraded %s to the Furigana column", path.c_str());
  return true;
}

namespace {

// False on any SD failure.
bool append(const Card& card, std::string_view language) {
  const std::string path = exportPath(language);
  if (!Storage.ensureDirectoryExists(EXPORT_DIR)) {
    LOG_ERR("MINE", "Could not create %s", EXPORT_DIR);
    return false;
  }
  // A row in the new layout must not land in a file still in the old one.
  if (!upgradeExport(path)) return false;
  // The header goes in only when the file is new: it tells Anki the layout, and a second copy
  // halfway down would import as a card.
  const bool isNew = !Storage.exists(path.c_str());
  HalFile file;
  if (!Storage.openFileForAppend("MINE", path.c_str(), file)) return false;

  const std::string row = formatRow(card);
  if (isNew || file.size() == 0) {
    const std::string header = headerLines();
    if (file.write(reinterpret_cast<const uint8_t*>(header.data()), header.size()) != header.size()) {
      LOG_ERR("MINE", "Header write failed");
      return false;
    }
  }
  if (file.write(reinterpret_cast<const uint8_t*>(row.data()), row.size()) != row.size()) {
    LOG_ERR("MINE", "Row write failed (%u bytes)", static_cast<unsigned>(row.size()));
    return false;
  }
  file.flush();
  LOG_INF("MINE", "Saved \"%s\" to %s (%u bytes)", card.word.c_str(), path.c_str(), static_cast<unsigned>(row.size()));
  return true;
}

// True when the file for `language` already has a row for `card`. A failed check reads as
// "not saved": a duplicate row is better than a lost save.
bool hasRow(const Card& card, const std::string_view language) {
  const std::string path = exportPath(language);
  if (!Storage.exists(path.c_str())) return false;
  HalFile file;
  if (!Storage.openFileForRead("MINE", path.c_str(), file)) return false;
  // Heap, not stack: big reads keep a long file to few SD round trips. Freed on return.
  constexpr size_t CHUNK = 4096;
  auto buf = makeUniqueNoThrow<char[]>(CHUNK);
  if (!buf) {
    LOG_ERR("MINE", "OOM: %u bytes for the duplicate check", static_cast<unsigned>(CHUNK));
    return false;
  }
  GuidFinder finder(makeGuid(card.word, card.sentence));
  int n;
  while ((n = file.read(buf.get(), CHUNK)) > 0) {
    if (finder.feed(std::string_view(buf.get(), static_cast<size_t>(n)))) return true;
    resetTaskWatchdogIfSubscribed();  // a long file is many reads
  }
  return false;
}

}  // namespace

SaveResult saveOnce(const Card& card, const std::string_view language) {
  if (hasRow(card, language)) {
    LOG_INF("MINE", "\"%s\" is already saved with this sentence", card.word.c_str());
    return SaveResult::AlreadySaved;
  }
  return append(card, language) ? SaveResult::Saved : SaveResult::Failed;
}

std::string today() {
  struct tm local{};
  if (!halClock.hasTime() || !halClock.localTime(local)) return {};
  char buf[12];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
  return buf;
}

}  // namespace sentencemining
