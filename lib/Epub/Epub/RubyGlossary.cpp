#include "RubyGlossary.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>
#include <string_view>

namespace RubyGlossary {

namespace {
constexpr uint8_t GLOSSARY_FILE_VERSION = 1;
constexpr char GLOSSARY_FILE_NAME[] = "/ruby.bin";
// Names and annotated words are short; anything longer is running prose, not a headword.
constexpr size_t MAX_TEXT_BYTES = 32;
// Per-section collection cap: bounds parse-time RAM (worst case ~200 * 64B = 12.8KB of
// short strings, transient until the post-parse merge).
constexpr size_t MAX_PAIRS_PER_SECTION = 200;
// File caps: a typical novel yields a few hundred unique pairs (single-digit KB).
constexpr uint16_t MAX_FILE_RECORDS = 1024;
constexpr size_t MAX_FILE_BYTES = 16 * 1024;

bool hasKanji(const std::string& utf8) {
  size_t i = 0;
  while (i < utf8.size()) {
    const auto c0 = static_cast<unsigned char>(utf8[i]);
    uint32_t cp = 0;
    size_t len = 1;
    if (c0 < 0x80) {
      cp = c0;
    } else if ((c0 & 0xE0) == 0xC0 && i + 1 < utf8.size()) {
      cp = ((c0 & 0x1Fu) << 6) | (static_cast<unsigned char>(utf8[i + 1]) & 0x3Fu);
      len = 2;
    } else if ((c0 & 0xF0) == 0xE0 && i + 2 < utf8.size()) {
      cp = ((c0 & 0x0Fu) << 12) | ((static_cast<unsigned char>(utf8[i + 1]) & 0x3Fu) << 6) |
           (static_cast<unsigned char>(utf8[i + 2]) & 0x3Fu);
      len = 3;
    } else if ((c0 & 0xF8) == 0xF0 && i + 3 < utf8.size()) {
      cp = ((c0 & 0x07u) << 18) | ((static_cast<unsigned char>(utf8[i + 1]) & 0x3Fu) << 12) |
           ((static_cast<unsigned char>(utf8[i + 2]) & 0x3Fu) << 6) | (static_cast<unsigned char>(utf8[i + 3]) & 0x3Fu);
      len = 4;
    }
    // CJK Unified Ideographs (+ Ext A, compatibility, and the SIP planes).
    if ((cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
        (cp >= 0x20000 && cp <= 0x2FA1F)) {
      return true;
    }
    i += len;
  }
  return false;
}

std::string glossaryPath(const std::string& bookCachePath) { return bookCachePath + GLOSSARY_FILE_NAME; }

// Walk every record in an open glossary file, invoking cb(base, ruby). Returns false on a
// malformed file (callers treat that as "no usable glossary").
template <typename Callback>
bool forEachRecord(HalFile& f, uint16_t count, const Callback& cb) {
  char base[MAX_TEXT_BYTES + 1];
  char ruby[MAX_TEXT_BYTES + 1];
  for (uint16_t i = 0; i < count; i++) {
    uint8_t baseLen = 0, rubyLen = 0;
    if (f.read(&baseLen, 1) != 1 || baseLen == 0 || baseLen > MAX_TEXT_BYTES) return false;
    if (f.read(reinterpret_cast<uint8_t*>(base), baseLen) != baseLen) return false;
    if (f.read(&rubyLen, 1) != 1 || rubyLen == 0 || rubyLen > MAX_TEXT_BYTES) return false;
    if (f.read(reinterpret_cast<uint8_t*>(ruby), rubyLen) != rubyLen) return false;
    base[baseLen] = '\0';
    ruby[rubyLen] = '\0';
    cb(std::string_view(base, baseLen), std::string_view(ruby, rubyLen));
  }
  return true;
}

bool openAndReadHeader(const std::string& path, HalFile& f, uint16_t& count) {
  if (!Storage.openFileForRead("RUBY", path, f)) return false;
  uint8_t version = 0;
  if (f.read(&version, 1) != 1 || version != GLOSSARY_FILE_VERSION) return false;
  uint8_t countBytes[2];
  if (f.read(countBytes, 2) != 2) return false;
  count = static_cast<uint16_t>(countBytes[0] | (countBytes[1] << 8));
  return count <= MAX_FILE_RECORDS;
}
}  // namespace

void collect(std::vector<Pair>& pairs, const std::string& base, const std::string& ruby) {
  if (pairs.size() >= MAX_PAIRS_PER_SECTION) return;
  if (base.empty() || ruby.empty() || base == ruby) return;
  if (base.size() > MAX_TEXT_BYTES || ruby.size() > MAX_TEXT_BYTES) return;
  // Only kanji-bearing bases are worth remembering: ruby over kana is decorative and a
  // kana base needs no reading.
  if (!hasKanji(base)) return;
  if (std::any_of(pairs.begin(), pairs.end(), [&](const Pair& p) { return p.first == base && p.second == ruby; })) {
    return;
  }
  pairs.emplace_back(base, ruby);
}

void merge(const std::string& bookCachePath, const std::vector<Pair>& pairs) {
  if (pairs.empty() || bookCachePath.empty()) return;
  const std::string path = glossaryPath(bookCachePath);

  // Load the existing file into a transient heap buffer so the rewrite can carry it over --
  // HalStorage has no append mode (openFileForWrite truncates). Size the buffer to the actual
  // file (not the 16KB cap): this runs right after a section parse, where a large contiguous
  // allocation is exactly what a fragmented heap cannot give, and a failed allocation here
  // silently loses the harvest. First write needs no buffer at all.
  size_t oldBytes = 0;
  uint16_t oldCount = 0;
  std::unique_ptr<uint8_t[]> oldBuf;
  std::vector<Pair> pending = pairs;
  {
    HalFile f;
    uint16_t count = 0;
    if (openAndReadHeader(path, f, count)) {
      const size_t bufCap = std::max<size_t>(std::min(static_cast<size_t>(f.fileSize()), MAX_FILE_BYTES), 1);
      oldBuf = makeUniqueNoThrow<uint8_t[]>(bufCap);
      if (!oldBuf) return;  // best-effort: skip this harvest under memory pressure
      const bool ok = forEachRecord(f, count, [&](std::string_view base, std::string_view ruby) {
        // Drop pending pairs the file already has.
        pending.erase(std::remove_if(pending.begin(), pending.end(),
                                     [&](const Pair& p) { return p.first == base && p.second == ruby; }),
                      pending.end());
        const size_t recLen = 2 + base.size() + ruby.size();
        if (oldBytes + recLen <= bufCap) {
          oldBuf[oldBytes++] = static_cast<uint8_t>(base.size());
          memcpy(oldBuf.get() + oldBytes, base.data(), base.size());
          oldBytes += base.size();
          oldBuf[oldBytes++] = static_cast<uint8_t>(ruby.size());
          memcpy(oldBuf.get() + oldBytes, ruby.data(), ruby.size());
          oldBytes += ruby.size();
          oldCount++;
        }
      });
      if (!ok) {
        // Malformed file: rewrite from scratch with just the new pairs.
        oldBytes = 0;
        oldCount = 0;
        pending = pairs;
      }
    }
  }

  // Respect the file caps; drop overflow silently (the glossary is best-effort).
  auto pendingBytes = [&pending] {
    size_t n = 0;
    for (const auto& p : pending) n += 2 + p.first.size() + p.second.size();
    return n;
  };
  while (!pending.empty() &&
         (oldCount + pending.size() > MAX_FILE_RECORDS || 3 + oldBytes + pendingBytes() > MAX_FILE_BYTES)) {
    pending.pop_back();
  }
  if (pending.empty()) return;

  HalFile out;
  if (!Storage.openFileForWrite("RUBY", path, out)) return;
  const uint16_t newCount = static_cast<uint16_t>(oldCount + pending.size());
  const uint8_t header[3] = {GLOSSARY_FILE_VERSION, static_cast<uint8_t>(newCount & 0xFF),
                             static_cast<uint8_t>(newCount >> 8)};
  out.write(header, 3);
  if (oldBytes > 0) out.write(oldBuf.get(), oldBytes);
  for (const auto& p : pending) {
    const uint8_t baseLen = static_cast<uint8_t>(p.first.size());
    const uint8_t rubyLen = static_cast<uint8_t>(p.second.size());
    out.write(&baseLen, 1);
    out.write(reinterpret_cast<const uint8_t*>(p.first.data()), baseLen);
    out.write(&rubyLen, 1);
    out.write(reinterpret_cast<const uint8_t*>(p.second.data()), rubyLen);
  }
  LOG_DBG("RUBY", "Glossary: +%u pairs (%u total)", static_cast<unsigned>(pending.size()),
          static_cast<unsigned>(newCount));
}

bool lookup(const std::string& bookCachePath, const std::string& base, std::string& outReadings) {
  outReadings.clear();
  if (bookCachePath.empty() || base.empty() || base.size() > MAX_TEXT_BYTES) return false;
  HalFile f;
  uint16_t count = 0;
  if (!openAndReadHeader(glossaryPath(bookCachePath), f, count)) return false;
  // Distinct readings for the same base are rare (e.g. two characters sharing surname
  // kanji) -- collect exactly, then join with '・'.
  std::vector<std::string> readings;
  forEachRecord(f, count, [&](std::string_view recBase, std::string_view ruby) {
    if (recBase != base) return;
    if (std::any_of(readings.begin(), readings.end(), [&](const std::string& r) { return r == ruby; })) return;
    readings.emplace_back(ruby);
  });
  for (const auto& r : readings) {
    if (!outReadings.empty()) outReadings += "\xE3\x83\xBB";  // '・'
    outReadings += r;
  }
  return !outReadings.empty();
}

}  // namespace RubyGlossary
