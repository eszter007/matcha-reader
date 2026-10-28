#include "SyncedPositionPrompt.h"

#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>

#include "activities/Activity.h"
#include "activities/util/ConfirmationActivity.h"
#include "util/RemoteProgress.h"

namespace SyncedPositionPrompt {

bool offer(Activity& host, GfxRenderer& renderer, MappedInputManager& input, const std::string& path,
           const int localPercent, const std::function<void(int percent)>& onJump) {
  const int synced = remoteProgress::percent(path);
  // Only a clear lead: rounding between the two page counts must not ask on every open.
  if (synced < 0 || synced >= 100 || synced < localPercent + 2) return false;
  char body[96];
  snprintf(body, sizeof(body), tr(STR_JUMP_TO_SYNCED_PERCENT), synced);
  auto confirmation = makeUniqueNoThrow<ConfirmationActivity>(renderer, input, tr(STR_KOREADER_SYNC), body);
  if (!confirmation) {
    LOG_ERR("SYNC", "OOM: synced position dialog");
    return false;
  }
  host.startActivityForResult(std::move(confirmation), [onJump, synced](const ActivityResult& result) {
    if (!result.isCancelled) onJump(synced);
  });
  return true;
}

}  // namespace SyncedPositionPrompt
