#include "SentenceMining.h"

#include <HalClock.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>
#include <ctime>

#include "TaskWatchdog.h"

namespace sentencemining {

namespace {

// False on any SD failure.
bool append(const Card& card, std::string_view language) {
  const std::string path = exportPath(language);
  if (!Storage.ensureDirectoryExists(EXPORT_DIR)) {
    LOG_ERR("MINE", "Could not create %s", EXPORT_DIR);
    return false;
  }
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
