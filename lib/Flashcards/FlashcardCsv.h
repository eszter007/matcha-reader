#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

// Reading the sentence-mining export (SentenceMiningCsv.h) back as flashcards. The file is
// RFC 4180 CSV after a block of '#' header lines for Anki; fields may be quoted and hold
// commas, doubled quotes and line breaks. Fed one byte at a time, so a file of any size streams
// through a fixed buffer. Pure: host-testable.
namespace flashcards {

// Column order of SentenceMiningCsv::formatRow.
// Files from before the Furigana column hold Tags in its place: upgrade them first
// (sentencemining::upgradeExport).
enum Column : uint8_t {
  GUID,
  WORD,
  READING,
  SENTENCE,
  DEFINITION,
  BOOK,
  AUTHOR,
  DATE,
  DICTIONARY,
  FURIGANA,
  TAGS,
  COLUMNS
};

class CsvRecordParser {
 public:
  // keepMask: bit i set keeps column i; others are skipped without being stored. A kept field
  // longer than maxFieldBytes is cut (on a UTF-8 boundary) so one bad row cannot grow the heap.
  explicit CsvRecordParser(uint16_t keepMask, size_t maxFieldBytes = 4096);

  // Feeds one byte. True when it ended a data record (header lines and blank lines never are);
  // read the fields then, before the next feed() reuses them.
  bool feed(char c);
  // End of input: true when a last record without a trailing newline was pending.
  bool finish();

  const std::string& field(Column column) const { return fields_[column]; }
  // Bytes fed before the record that just ended began: where to seek to read it again.
  uint32_t recordStart() const { return recordStart_; }

 private:
  void endField();
  bool endRecord();
  void clearFields();

  std::array<std::string, COLUMNS> fields_;
  uint16_t keepMask_;
  size_t maxFieldBytes_;
  uint32_t offset_ = 0;
  uint32_t recordStart_ = 0;
  uint32_t pendingStart_ = 0;
  uint8_t column_ = 0;
  bool atRecordStart_ = true;
  bool comment_ = false;
  bool inQuotes_ = false;
  bool quoteSeen_ = false;  // a quote inside a quoted field: either a doubled quote or the closing one
  bool anyData_ = false;
};

// "m" + 16 hex digits (makeGuid) as a number; 0 when the text is not one.
uint64_t guidToId(std::string_view guid);

// Replaces <b>, </b> and <br> with the given markers (or nothing) and decodes &amp; &lt; &gt;
// &quot; &#39;. The export's only markup, so a card can show the sentence with its word marked.
std::string stripExportHtml(std::string_view html, std::string_view boldOpen = "", std::string_view boldClose = "",
                            std::string_view lineBreak = "\n");

// Days since 1970-01-01 for "YYYY-MM-DD"; -1 when it is not a date.
int32_t dayNumber(std::string_view isoDate);

}  // namespace flashcards
