#pragma once

#include <string>

#include "OptionPopup.h"

class Activity;
class GfxRenderer;
class MappedInputManager;

// The long-press menu on a book cover, shared by the Library grid and the Home cover grid so the
// gesture means the same thing in both. Stats, the read/unread switch, and delete; the switch
// offers only the direction that would change anything, so a finished book has no "Mark as read"
// and an unopened one has no "Mark as unread".
//
// The host owns the OptionPopup (it has to render it) and passes it in. `onChanged` runs after an
// action that altered the book -- marked, or deleted -- so the host can reload its list; `deleted`
// says which of the two it was, since a deleted book must not be left in the caller's arrays.
namespace BookActionsMenu {

// progressPercent: the badge value, < 0 when unknown (no record yet, i.e. unread).
void show(OptionPopup& popup, Activity& host, GfxRenderer& renderer, MappedInputManager& input,
          const std::string& path, const std::string& title, int progressPercent,
          const std::function<void(bool deleted)>& onChanged);

}  // namespace BookActionsMenu
