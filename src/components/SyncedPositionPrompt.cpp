#include "SyncedPositionPrompt.h"

#include <I18n.h>
#include <KOReaderCredentialStore.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>

#include "activities/Activity.h"
#include "activities/util/ConfirmationActivity.h"
#include "util/SyncedPosition.h"

namespace SyncedPositionPrompt {

bool offer(Activity& host, GfxRenderer& renderer, MappedInputManager& input, const std::string& path, const int percent,
           const std::function<void()>& onJump) {
  // Smart sync settles positions without asking: the reader applied it while loading.
  if (KOREADER_STORE.getSyncBehavior() == KOReaderSyncBehavior::SMART) return false;
  if (percent < 0) return false;
  char body[96];
  snprintf(body, sizeof(body), tr(STR_JUMP_TO_SYNCED_PERCENT), percent);
  auto confirmation = makeUniqueNoThrow<ConfirmationActivity>(renderer, input, tr(STR_KOREADER_SYNC), body);
  if (!confirmation) {
    LOG_ERR("SYNC", "OOM: synced position dialog");
    return false;
  }
  host.startActivityForResult(std::move(confirmation), [onJump, path](const ActivityResult& result) {
    if (result.isCancelled) {
      syncedPosition::clear(path);  // asked once; the next sync of the book decides again
      return;
    }
    onJump();
  });
  return true;
}

}  // namespace SyncedPositionPrompt
