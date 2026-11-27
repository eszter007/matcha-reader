#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <LibraryBuilder.h>
#include <LibraryIndexFile.h>
#include <Logging.h>
#include <Memory.h>

#include "SyncEngine.h"
#include "activities/RenderLock.h"

bool syncEngine::syncAllBooks(GfxRenderer& renderer, const BookProgressFn onBook, void* const context) {
  // The index and its record stay off the stack; one book is loaded at a time.
  struct LibraryReader {
    library::LibraryIndexFile index;
    library::ClixRecord record{};
  };
  auto reader = makeUniqueNoThrow<LibraryReader>();
  if (!reader) {
    LOG_ERR("SYNC", "OOM: library index");
    return false;
  }
  if (!reader->index.open(library::libraryIndexPath())) {
    LOG_ERR("SYNC", "Cannot open library index");
    return false;
  }
  const uint16_t total = reader->index.bookCount();
  std::string path;
  for (uint16_t i = 0; i < total; i++) {
    if (!reader->index.readRecord(i, reader->record) || !reader->index.readPath(reader->record, path)) continue;
    if (!FsHelpers::hasEpubExtension(path)) continue;
    onBook(context, static_cast<uint16_t>(i + 1), total);
    KOReaderProgress progress;
    bool prepared;
    {
      RenderLock lock;
      GfxRenderer::FrameBufferLoan loan(renderer);
      prepared = savedBookProgress(path, progress);
    }
    // Never opened here, or unreadable: nothing to send for this book.
    if (!prepared) continue;
    // A failure past this point is the server's or the network's; the next books would fail the same way.
    if (!uploadUnlessBehind(path, progress)) return false;
  }
  return true;
}
