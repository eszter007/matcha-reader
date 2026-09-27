#include "BookActionsMenu.h"

#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include "RecentBooksStore.h"
#include "activities/Activity.h"
#include "activities/home/BookStatsActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "util/BookProgress.h"
#include "util/DeleteUtils.h"

namespace BookActionsMenu {
namespace {

enum class Action : uint8_t { Stats, MarkRead, MarkUnread, Delete };

void promptDelete(Activity& host, GfxRenderer& renderer, MappedInputManager& input, const std::string& path,
                  const std::string& title, const std::function<void(bool)>& onChanged) {
  auto confirmation =
      makeUniqueNoThrow<ConfirmationActivity>(renderer, input, tr(STR_DELETE) + std::string("? "), title);
  if (!confirmation) {
    LOG_ERR("BAM", "OOM: delete confirmation");
    return;
  }
  host.startActivityForResult(std::move(confirmation), [path, onChanged](const ActivityResult& result) {
    if (result.isCancelled) {
      onChanged(false);
      return;
    }
    LOG_DBG("BAM", "deleting %s", path.c_str());
    // Recursive: a manga book is a folder. Clears each removed file's reading cache on the way.
    if (!deletePathRecursive(path)) LOG_ERR("BAM", "cannot delete %s", path.c_str());
    if (RECENT_BOOKS.removeByPath(path)) RECENT_BOOKS.saveToFile();
    onChanged(true);
  });
}

}  // namespace

void show(OptionPopup& popup, Activity& host, GfxRenderer& renderer, MappedInputManager& input, const std::string& path,
          const std::string& title, const int progressPercent, const std::function<void(bool deleted)>& onChanged) {
  const char* labels[4];
  Action actions[4];
  int count = 0;
  const auto add = [&](const char* label, const Action action) {
    labels[count] = label;
    actions[count] = action;
    ++count;
  };

  add(tr(STR_VIEW_STATS), Action::Stats);
  // Only the direction that would change something: a finished book cannot be marked read, and a
  // book with no progress at all is already unread.
  if (progressPercent < 100) add(tr(STR_MARK_AS_READ), Action::MarkRead);
  if (progressPercent > 0) add(tr(STR_MARK_AS_UNREAD), Action::MarkUnread);
  add(tr(STR_DELETE), Action::Delete);

  popup.show(title.c_str(), labels, count, 0,
             [&host, &renderer, &input, path, title, onChanged, actions](const int choice) {
               if (choice < 0 || choice >= 4) return;
               switch (actions[choice]) {
                 case Action::Stats:
                   BookStatsActivity::openFor(host, renderer, input, path, title,
                                              [onChanged](const ActivityResult&) { onChanged(false); });
                   break;
                 case Action::MarkRead:
                   markBookRead(path);
                   onChanged(false);
                   break;
                 case Action::MarkUnread:
                   markBookUnread(path);
                   onChanged(false);
                   break;
                 case Action::Delete:
                   promptDelete(host, renderer, input, path, title, onChanged);
                   break;
               }
             });
}

}  // namespace BookActionsMenu
