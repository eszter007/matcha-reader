#pragma once

#include <Arduino.h>
#include <DictIndex.h>
#include <Logging.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

// The kanji dictionary's pages for a looked-up word: one per Han character that has an entry, after
// the word's own pages. Shared by the EPUB and manga word lookups.
namespace kanjipages {

constexpr int kMaxPages = 4;
// Opens a kanji page inside a merged definition, as "— Grammar: " opens the grammar page.
constexpr char kMarker[] = "\xe2\x80\x94 Kanji: ";
constexpr char kMarkerEnd[] = " \xe2\x80\x94";
// Headroom kept free beside a kept page, as for the grammar merge.
constexpr size_t kHeapHeadroom = 8 * 1024;

struct Page {
  char character[5] = {};
  std::string definition;
};

inline bool isHan(const uint32_t cp) {
  return (cp >= 0x3400 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0x20000 && cp <= 0x3FFFF);
}

// Fills `pages` in headword order and returns how many hold an entry. A page is skipped when the
// heap cannot hold it beside what is already kept.
inline int collect(const std::string& headword, std::array<Page, kMaxPages>& pages) {
  int count = 0;
  size_t kept = 0;
  for (size_t i = 0; i < headword.size() && count < kMaxPages;) {
    const auto lead = static_cast<unsigned char>(headword[i]);
    const size_t len = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
    if (i + len > headword.size()) break;
    uint32_t cp = len == 1 ? lead : lead & (0xFF >> (len + 1));
    for (size_t k = 1; k < len; k++) cp = (cp << 6) | (static_cast<unsigned char>(headword[i + k]) & 0x3F);
    char ch[5] = {};
    std::memcpy(ch, headword.data() + i, len);
    i += len;
    if (!isHan(cp)) continue;
    bool repeat = false;
    for (int k = 0; k < count; k++) repeat = repeat || std::strcmp(pages[k].character, ch) == 0;
    if (repeat) continue;
    DictEntry entry;
    if (!DictIndex::lookupKanji(ch, entry) || entry.definition.empty()) continue;
    if (ESP.getMaxAllocHeap() < kept + entry.definition.size() + kHeapHeadroom) {
      LOG_ERR("KANJI", "Skipping kanji page, heap too low (maxAlloc=%u)", ESP.getMaxAllocHeap());
      break;
    }
    kept += entry.definition.size();
    std::memcpy(pages[count].character, ch, sizeof(ch));
    pages[count].definition = std::move(entry.definition);
    count++;
  }
  return count;
}

// Appends each kanji page to `definition` under its kMarker heading, for a panel that splits one
// merged definition into pages. Nothing is added when the heap cannot hold every page.
inline void appendTo(const std::string& headword, std::string& definition) {
  std::array<Page, kMaxPages> pages;
  const int count = collect(headword, pages);
  if (count == 0) return;
  size_t added = 0;
  for (int i = 0; i < count; i++) {
    added += 2 + sizeof(kMarker) + sizeof(kMarkerEnd) + 5 + pages[i].definition.size();
  }
  // One reserve for every page: appending page by page would copy the whole definition each time.
  if (ESP.getMaxAllocHeap() < definition.size() + added + kHeapHeadroom) {
    LOG_ERR("KANJI", "Skipping kanji pages, heap too low (maxAlloc=%u)", ESP.getMaxAllocHeap());
    return;
  }
  definition.reserve(definition.size() + added);
  for (int i = 0; i < count; i++) {
    definition += "\n\n";
    definition += kMarker;
    definition += pages[i].character;
    definition += kMarkerEnd;
    definition += '\n';
    definition += pages[i].definition;
    std::string().swap(pages[i].definition);  // free each page once copied, not all at the end
  }
}

// When `piece` opens with a kMarker heading, moves the character into `character`, removes the
// heading line and returns true.
inline bool takeHeading(std::string& piece, std::string& character) {
  if (piece.compare(0, sizeof(kMarker) - 1, kMarker) != 0) return false;
  const size_t lineEnd = piece.find('\n');
  const size_t headEnd = lineEnd == std::string::npos ? piece.size() : lineEnd;
  character = piece.substr(sizeof(kMarker) - 1, headEnd - (sizeof(kMarker) - 1));
  const size_t dash = character.find(kMarkerEnd);
  if (dash != std::string::npos) character.erase(dash);
  piece.erase(0, lineEnd == std::string::npos ? piece.size() : lineEnd + 1);
  return true;
}

}  // namespace kanjipages
