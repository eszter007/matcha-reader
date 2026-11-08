#include "NestedLookupHost.h"

#include <Arduino.h>
#include <GfxRenderer.h>

#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "activities/RenderLock.h"

namespace {
constexpr unsigned long NOTICE_MS = 1500;
}  // namespace

bool NestedLookupHost::isConfirmHold(const MappedInputManager& mappedInput) {
  return mappedInput.getHeldTime() >= ReaderUtils::GO_BACK_OR_HOME_MS;
}

bool NestedLookupHost::startCursor(std::vector<DefinitionText::CharBox>&& found) {
  RenderLock lock;  // drawCursor() reads the boxes
  boxes = std::move(found);
  index = boxes.empty() ? -1 : 0;
  return cursorActive();
}

bool NestedLookupHost::startCjkCursor(const GfxRenderer& renderer, const std::string& text) {
  RenderLock lock;  // collecting measures through the font cache render shares, and reads drawnText
  DefinitionText::collectCjkBoxes(renderer, drawnText, text, boxes);
  index = boxes.empty() ? -1 : 0;
  return cursorActive();
}

void NestedLookupHost::stopCursor() {
  RenderLock lock;
  index = -1;
  std::vector<DefinitionText::CharBox>{}.swap(boxes);
}

NestedLookupHost::CursorInput NestedLookupHost::handleCursorInput(const MappedInputManager& mappedInput) {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    stopCursor();
    return CursorInput::Redraw;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    picked = boxes[index].offset;
    stopCursor();
    return CursorInput::Picked;
  }
  int delta = 0;
  if (ReaderUtils::lookupPressAny(mappedInput, delta) != ReaderUtils::LookupPress::None && delta != 0) {
    RenderLock lock;
    const int count = static_cast<int>(boxes.size());
    index = (index + delta + count) % count;
    return CursorInput::Redraw;
  }
  return CursorInput::None;
}

void NestedLookupHost::drawCursor(const GfxRenderer& renderer) const {
  if (!cursorActive()) return;
  const auto& box = boxes[index];
  renderer.drawRoundedRect(box.x - 3, box.y, box.width + 6, box.height, /*lineWidth=*/2, /*cornerRadius=*/4, true);
}

void NestedLookupHost::showNotice(const StrId message) {
  notice = message;
  noticeUntil = millis() + NOTICE_MS;
}

bool NestedLookupHost::noticeExpired() {
  if (noticeUntil == 0 || millis() < noticeUntil) return false;
  noticeUntil = 0;
  return true;
}

const char* NestedLookupHost::popupText(const char* fallback) const {
  return noticeUntil != 0 ? I18N.get(notice) : fallback;
}

std::optional<ActivityResult> NestedLookupHost::chainResult(const ActivityResult& result, const bool nestedLevel) {
  const bool longBack = std::holds_alternative<LookupChainCloseResult>(result.data);
  if (!longBack && !result.isCancelled) return std::nullopt;
  if (nestedLevel && !result.isCancelled) return ActivityResult{LookupChainCloseResult{}};
  ActivityResult cancelled;
  cancelled.isCancelled = true;
  return cancelled;
}
