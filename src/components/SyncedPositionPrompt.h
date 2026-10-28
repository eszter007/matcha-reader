#pragma once

#include <functional>
#include <string>

class Activity;
class GfxRenderer;
class MappedInputManager;

// Asks, when a book opens, whether to continue where the sync server says it was read furthest.
// The Library already shows that percentage; without this the reader opened at its own saved page.
namespace SyncedPositionPrompt {

// Shows the question when the server is clearly ahead of localPercent (< 0 when the book has no
// local progress) and calls onJump with the server's percent on Confirm. Returns false when there
// was nothing to ask.
bool offer(Activity& host, GfxRenderer& renderer, MappedInputManager& input, const std::string& path,
           int localPercent, const std::function<void(int percent)>& onJump);

}  // namespace SyncedPositionPrompt
