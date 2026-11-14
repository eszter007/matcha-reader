"""Run with python3 test/test_flashcards.py (requires a host C++ compiler)."""

from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

harness = r"""
#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>
#include "Fsrs.h"
#include "FlashcardCsv.h"
#include "SentenceMiningCsv.h"

using namespace flashcards;

static int parseAll(const std::string& text, CsvRecordParser& p, std::string* words, uint32_t* starts) {
  int n = 0;
  for (char c : text) {
    if (p.feed(c)) { words[n] = p.field(WORD); starts[n] = p.recordStart(); n++; }
  }
  if (p.finish()) { words[n] = p.field(WORD); starts[n] = p.recordStart(); n++; }
  return n;
}

int main() {
  // Round trip through the real writer: header lines, quoting, commas, quotes and line breaks.
  sentencemining::Card a{"微妙", "びみょう", "その答えは<b>微妙</b>だね", "delicate<br>iffy, \"hard\" to say", "Book, One", "", "2026-11-11", "JMdict", "微妙[びみょう]"};
  sentencemining::Card b{"flâner", "", "Il aimait <b>flâner</b>\nle long", "to stroll", "L'Étranger", "Camus", "", "fr", ""};
  const std::string file = sentencemining::headerLines() + sentencemining::formatRow(a) + sentencemining::formatRow(b);
  CsvRecordParser p(0xFFFF);
  std::string words[4];
  uint32_t starts[4];
  assert(parseAll(file, p, words, starts) == 2);
  assert(words[0] == "微妙" && words[1] == "flâner");
  // recordStart points at the row: re-parsing from there yields the same card.
  CsvRecordParser reread(0xFFFF);
  const std::string tail = file.substr(starts[1]);
  bool got = false;
  for (char c : tail) if (reread.feed(c)) { got = true; break; }
  assert(got && reread.field(WORD) == "flâner" && reread.field(SENTENCE) == "Il aimait <b>flâner</b>\nle long");
  assert(reread.field(BOOK) == "L'Étranger");
  // Full fields of the first card.
  CsvRecordParser full(0xFFFF);
  for (char c : file) if (full.feed(c)) break;
  assert(full.field(DEFINITION) == "delicate<br>iffy, \"hard\" to say");
  assert(full.field(BOOK) == "Book, One");
  assert(full.field(FURIGANA) == "微妙[びみょう]" && full.field(TAGS).rfind("matcha", 0) == 0);
  assert(guidToId(full.field(GUID)) != 0);
  assert(guidToId(full.field(GUID)) == guidToId(sentencemining::makeGuid(a.word, a.sentence)));
  // Skipped columns are not stored.
  CsvRecordParser skim(1u << GUID);
  for (char c : file) if (skim.feed(c)) break;
  assert(!skim.field(GUID).empty() && skim.field(WORD).empty() && skim.field(DEFINITION).empty());
  // A last row without a newline still counts.
  CsvRecordParser last(0xFFFF);
  std::string noNl = "#x\nm0000000000000001,w,,,,,,,,\nm0000000000000002,z";
  int n = parseAll(noNl, last, words, starts);
  assert(n == 2 && words[1] == "z");
  assert(guidToId("m00000000000000ff") == 255 && guidToId("bad") == 0);
  assert(stripExportHtml("a<b>b</b>&amp;<br>c", "[", "]", "/") == "a[b]&/c");
  assert(dayNumber("1970-01-01") == 0 && dayNumber("2026-11-11") == 20768 && dayNumber("nope") == -1);

  // FSRS: R(S,S) = 0.9, so at 90% retention the interval equals the stability.
  assert(std::fabs(fsrs::retrievability(10.0f, 10.0f) - 0.9f) < 1e-4f);
  fsrs::Params params;
  assert(fsrs::intervalDays(10.0f, params) == 10);
  params.retention = 0.8f;
  assert(fsrs::intervalDays(10.0f, params) > 10);  // lower retention, longer gaps
  params.retention = 0.9f;
  params.maxInterval = 30;
  assert(fsrs::intervalDays(1000.0f, params) == 30);
  params.maxInterval = 36500;
  // New card: first ratings take the default initial stabilities.
  assert(std::fabs(fsrs::initial(fsrs::Rating::Good).stability - 3.173f) < 1e-4f);
  // Successful review grows stability; a lapse shrinks it; difficulty moves the right way.
  fsrs::Memory m = fsrs::initial(fsrs::Rating::Good);
  fsrs::Memory good = fsrs::next(m, 3.0f, fsrs::Rating::Good);
  fsrs::Memory again = fsrs::next(m, 3.0f, fsrs::Rating::Again);
  fsrs::Memory easy = fsrs::next(m, 3.0f, fsrs::Rating::Easy);
  assert(good.stability > m.stability && again.stability < m.stability);
  assert(easy.stability > good.stability);
  assert(again.difficulty > m.difficulty && easy.difficulty < m.difficulty);
  // Button previews are ordered.
  uint16_t iv[4];
  fsrs::previewIntervals(m, true, 0, params, iv);
  assert(iv[0] == 0 && iv[1] <= iv[2] && iv[2] < iv[3]);
  fsrs::previewIntervals(good, false, 10.0f, params, iv);
  assert(iv[1] <= iv[2] && iv[2] < iv[3]);
  std::printf("new: hard %u good %u easy %u\n", iv[1], iv[2], iv[3]);
  return 0;
}
"""

with tempfile.TemporaryDirectory() as directory:
    cpp = Path(directory) / "flashcards.cpp"
    exe = Path(directory) / "flashcards"
    cpp.write_text(harness)
    subprocess.run(
        ["c++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
         "-I", str(ROOT / "lib/Flashcards"), "-I", str(ROOT / "lib/SentenceMining"),
         str(cpp), str(ROOT / "lib/Flashcards/Fsrs.cpp"), str(ROOT / "lib/Flashcards/FlashcardCsv.cpp"),
         str(ROOT / "lib/SentenceMining/SentenceMiningCsv.cpp"), "-o", str(exe)],
        check=True,
    )
    subprocess.run([str(exe)], check=True)
print("Flashcard regression checks passed")
