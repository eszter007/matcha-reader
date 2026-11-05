#pragma once

#include <functional>
#include <string>

class Activity;
class GfxRenderer;
class MappedInputManager;

// Asks, when a book opens, whether to continue at the exact position an Insights sync fetched from
// another device (syncedPosition). Only with Ask Every Time: Smart sync applies it as the book loads.
namespace SyncedPositionPrompt {

// Shows the question when the book has such a position, at `percent` (where it falls in this reader's
// layout, < 0 when there is none), and calls onJump on Confirm; Cancel drops the position. Returns false
// when there was nothing to ask.
bool offer(Activity& host, GfxRenderer& renderer, MappedInputManager& input, const std::string& path, int percent,
           const std::function<void()>& onJump);

}  // namespace SyncedPositionPrompt
