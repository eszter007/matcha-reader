#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Deinflector.h"
#include "DictIndex.h"
#include "WordLookup.h"

// Build a tiny synthetic dictionary for testing.
// Entries: 食べる (taberu, to eat), 読む (yomu, to read), 美しい (utsukushii, beautiful),
//          する (suru, to do), 来る (kuru, to come), 行く (iku, to go),
//          書く (kaku, to write), 猫 (neko, cat)
struct TestEntry {
  const char* headword;
  const char* definition;
  uint8_t priority;
};

static const TestEntry kTestEntries[] = {
    {"\xe4\xb9\x97\xe3\x82\x8b", "【のる】\nto ride; to board", 200},                                    // 乗る
    {"\xe4\xb9\x97\xe3\x82\x8b", "【のる】\nto ride; to board", 200},                                    // duplicate for sort test
    {"\xe4\xb9\xb0\xe3\x81\x86", "【かう】\nto buy", 200},                                                // 買う
    {"\xe6\x9b\xb8\xe3\x81\x8f", "【かく】\nto write", 200},                                              // 書く
    {"\xe7\x8c\xab", "【ねこ】\ncat", 200},                                                                // 猫
    {"\xe7\xbe\x8e\xe3\x81\x97\xe3\x81\x84", "【うつくしい】\nbeautiful", 200},                          // 美しい
    {"\xe8\xa1\x8c\xe3\x81\x8f", "【いく】\nto go", 200},                                                 // 行く
    {"\xe8\xaa\xad\xe3\x82\x80", "【よむ】\nto read", 200},                                               // 読む
    {"\xe9\xa3\x9f\xe3\x81\xb9\xe3\x82\x8b", "【たべる】\nto eat", 200},                                 // 食べる
    {"\xe3\x81\x99\xe3\x82\x8b", "【する】\nto do", 200},                                                 // する
    {"\xe3\x81\x8f\xe3\x82\x8b", "【くる】\nto come", 200},                                               // くる
};

static bool buildTestDictionary(const char* idxPath, const char* datPath) {
  // Sort entries by headword bytes
  struct SortedEntry {
    char headword[32];
    uint32_t offset;
    uint16_t length;
    uint8_t priority;
  };

  std::vector<SortedEntry> sorted;
  std::string datBlob;

  for (const auto& e : kTestEntries) {
    SortedEntry s;
    std::memset(s.headword, 0, sizeof(s.headword));
    const size_t len = std::strlen(e.headword);
    if (len >= sizeof(s.headword)) continue;
    std::memcpy(s.headword, e.headword, len);
    s.offset = static_cast<uint32_t>(datBlob.size());
    s.length = static_cast<uint16_t>(std::strlen(e.definition));
    s.priority = e.priority;
    datBlob.append(e.definition, s.length);
    sorted.push_back(s);
  }

  // Sort by headword
  std::sort(sorted.begin(), sorted.end(),
            [](const SortedEntry& a, const SortedEntry& b) { return std::memcmp(a.headword, b.headword, 32) < 0; });

  // Deduplicate
  auto it = std::unique(sorted.begin(), sorted.end(),
                        [](const SortedEntry& a, const SortedEntry& b) {
                          return std::memcmp(a.headword, b.headword, 32) == 0;
                        });
  sorted.erase(it, sorted.end());

  // Write dat file
  FILE* dat = std::fopen(datPath, "wb");
  if (!dat) return false;
  std::fwrite(datBlob.data(), 1, datBlob.size(), dat);
  std::fclose(dat);

  // Write idx file
  FILE* idx = std::fopen(idxPath, "wb");
  if (!idx) return false;
  for (const auto& s : sorted) {
    std::fwrite(s.headword, 1, 32, idx);
    std::fwrite(&s.offset, 1, 4, idx);
    std::fwrite(&s.length, 1, 2, idx);
    std::fwrite(&s.priority, 1, 1, idx);
    uint8_t pad = 0;
    std::fwrite(&pad, 1, 1, idx);
  }
  std::fclose(idx);

  return true;
}

static int testCount = 0;
static int passCount = 0;

static void check(bool cond, const char* desc) {
  testCount++;
  if (cond) {
    passCount++;
    std::printf("  PASS: %s\n", desc);
  } else {
    std::printf("  FAIL: %s\n", desc);
  }
}

int main() {
  // Use paths relative to CWD for the test dictionary
  const char* idxPath = "/dict/jmdict.idx";
  const char* datPath = "/dict/jmdict.dat";

  // Create /dict/ directory and build test dictionary
  if (std::system("mkdir -p /dict") != 0) {
    std::printf("WARNING: mkdir -p /dict failed\n");
  }
  if (!buildTestDictionary(idxPath, datPath)) {
    std::printf("FATAL: Failed to build test dictionary\n");
    return 1;
  }

  // === Test 1: DictIndex exact lookup ===
  std::printf("\n=== Test 1: DictIndex exact lookup ===\n");
  {
    DictEntry entry;
    check(DictIndex::lookupExact("\xe7\x8c\xab", entry), "lookup 猫 (neko)");
    check(entry.definition.find("cat") != std::string::npos, "猫 definition contains 'cat'");

    check(DictIndex::lookupExact("\xe9\xa3\x9f\xe3\x81\xb9\xe3\x82\x8b", entry), "lookup 食べる (taberu)");
    check(entry.definition.find("to eat") != std::string::npos, "食べる definition contains 'to eat'");

    check(DictIndex::lookupExact("\xe8\xaa\xad\xe3\x82\x80", entry), "lookup 読む (yomu)");
    check(entry.definition.find("to read") != std::string::npos, "読む definition contains 'to read'");

    check(!DictIndex::lookupExact("\xe5\xad\x98\xe5\x9c\xa8", entry), "lookup 存在 (sonzai) not found");
  }

  // === Test 2: Deinflector ===
  std::printf("\n=== Test 2: Deinflector ===\n");
  {
    // 食べた (past of 食べる)
    auto candidates = Deinflector::deinflect("\xe9\xa3\x9f\xe3\x81\xb9\xe3\x81\x9f");
    bool found = false;
    for (const auto& c : candidates) {
      if (c.text == "\xe9\xa3\x9f\xe3\x81\xb9\xe3\x82\x8b") {
        found = true;
        break;
      }
    }
    check(found, "食べた deinflects to 食べる");

    // 読んだ (past of 読む — godan ん+だ)
    candidates = Deinflector::deinflect("\xe8\xaa\xad\xe3\x82\x93\xe3\x81\xa0");
    found = false;
    for (const auto& c : candidates) {
      if (c.text == "\xe8\xaa\xad\xe3\x82\x80") {
        found = true;
        break;
      }
    }
    check(found, "読んだ deinflects to 読む");

    // 美しかった (past of 美しい — i-adjective)
    candidates = Deinflector::deinflect("\xe7\xbe\x8e\xe3\x81\x97\xe3\x81\x8b\xe3\x81\xa3\xe3\x81\x9f");
    found = false;
    for (const auto& c : candidates) {
      if (c.text == "\xe7\xbe\x8e\xe3\x81\x97\xe3\x81\x84") {
        found = true;
        break;
      }
    }
    check(found, "美しかった deinflects to 美しい");

    // 書いた (past of 書く — godan k-column)
    candidates = Deinflector::deinflect("\xe6\x9b\xb8\xe3\x81\x84\xe3\x81\x9f");
    found = false;
    for (const auto& c : candidates) {
      if (c.text == "\xe6\x9b\xb8\xe3\x81\x8f") {
        found = true;
        break;
      }
    }
    check(found, "書いた deinflects to 書く");

    // した (past of する — irregular)
    candidates = Deinflector::deinflect("\xe3\x81\x97\xe3\x81\x9f");
    found = false;
    for (const auto& c : candidates) {
      if (c.text == "\xe3\x81\x99\xe3\x82\x8b") {
        found = true;
        break;
      }
    }
    check(found, "した deinflects to する");

    // きた (past of くる — irregular)
    candidates = Deinflector::deinflect("\xe3\x81\x8d\xe3\x81\x9f");
    found = false;
    for (const auto& c : candidates) {
      if (c.text == "\xe3\x81\x8f\xe3\x82\x8b") {
        found = true;
        break;
      }
    }
    check(found, "きた deinflects to くる");

    // 食べない (negative of 食べる)
    candidates = Deinflector::deinflect("\xe9\xa3\x9f\xe3\x81\xb9\xe3\x81\xaa\xe3\x81\x84");
    found = false;
    for (const auto& c : candidates) {
      if (c.text == "\xe9\xa3\x9f\xe3\x81\xb9\xe3\x82\x8b") {
        found = true;
        break;
      }
    }
    check(found, "食べない deinflects to 食べる");

    // 買った (past of 買う — godan u-column)
    candidates = Deinflector::deinflect("\xe4\xb9\xb0\xe3\x81\xa3\xe3\x81\x9f");
    found = false;
    for (const auto& c : candidates) {
      if (c.text == "\xe4\xb9\xb0\xe3\x81\x86") {
        found = true;
        break;
      }
    }
    check(found, "買った deinflects to 買う");
  }

  // === Test 3: WordLookup end-to-end ===
  std::printf("\n=== Test 3: WordLookup end-to-end ===\n");
  {
    // Paragraph: 猫が食べた (neko ga tabeta — the cat ate)
    std::string text = "\xe7\x8c\xab\xe3\x81\x8c\xe9\xa3\x9f\xe3\x81\xb9\xe3\x81\x9f";

    // Look up at offset 0 → should find 猫
    WordLookupResult result;
    check(WordLookup::lookup(text, 0, result), "lookup at offset 0 finds match");
    check(result.entry.headword == "\xe7\x8c\xab", "matched headword is 猫");
    check(result.entry.definition.find("cat") != std::string::npos, "definition contains 'cat'");

    // Look up at offset 6 (食べた) → should deinflect to 食べる
    check(WordLookup::lookup(text, 6, result), "lookup at offset 6 finds match");
    check(result.entry.headword == "\xe9\xa3\x9f\xe3\x81\xb9\xe3\x82\x8b", "matched headword is 食べる");
    check(result.entry.definition.find("to eat") != std::string::npos, "definition contains 'to eat'");

    // Look up at offset 3 (が) → particle, should not match
    check(!WordLookup::lookup(text, 3, result), "が (particle) has no dictionary match");
  }

  // === Test 4: Edge cases ===
  std::printf("\n=== Test 4: Edge cases ===\n");
  {
    // Empty string
    WordLookupResult result;
    check(!WordLookup::lookup("", 0, result), "empty text returns no match");

    // Offset past end
    check(!WordLookup::lookup("abc", 10, result), "offset past end returns no match");

    // Single character lookup
    DictEntry entry;
    check(DictIndex::lookupExact("\xe7\x8c\xab", entry), "single-char headword lookup works");
  }

  // Cleanup
  std::remove(idxPath);
  std::remove(datPath);
  int rmResult = std::system("rmdir /dict 2>/dev/null");
  (void)rmResult;

  std::printf("\n=== Results: %d/%d passed ===\n", passCount, testCount);
  return (passCount == testCount) ? 0 : 1;
}
