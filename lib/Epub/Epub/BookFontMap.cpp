#include "BookFontMap.h"

namespace bookfonts {
namespace {
uint16_t keys[MAX_FAMILIES] = {};
int ids[MAX_FAMILIES] = {};
size_t familyCount = 0;
}  // namespace

void setFamilies(const uint16_t* familyKeys, const int* fontIds, const size_t count) {
  familyCount = 0;
  for (size_t i = 0; i < count && i < MAX_FAMILIES; ++i) {
    keys[i] = familyKeys[i];
    ids[i] = fontIds[i];
    familyCount = i + 1;
  }
}

int fontIdForFamily(const uint16_t familyKey) {
  if (familyKey == 0) return 0;
  for (size_t i = 0; i < familyCount; ++i) {
    if (keys[i] == familyKey) return ids[i];
  }
  return 0;
}

bool isBookFont(const int fontId) {
  if (fontId == 0) return false;
  for (size_t i = 0; i < familyCount; ++i) {
    if (ids[i] == fontId) return true;
  }
  return false;
}

uint8_t slotForFamily(const uint16_t familyKey) {
  if (familyCount == 0) return 0;  // nothing loaded: every family is the reader font, as unset
  for (size_t i = 0; i < familyCount; ++i) {
    if (familyKey != 0 && keys[i] == familyKey) return static_cast<uint8_t>(i + 1);
  }
  return READER_SLOT;
}

int fontIdForSlot(const uint8_t slot) { return slot >= 1 && slot <= familyCount ? ids[slot - 1] : 0; }

}  // namespace bookfonts
