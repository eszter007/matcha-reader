#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "util/SentenceMining.h"

struct VocabularyContext {
  std::string title;
  std::string author;
  std::string series;
  std::optional<float> seriesIndex;
  std::string chapter;
  uint16_t page = 0;
  uint16_t spine = 0;
  uint32_t startOffset = UINT32_MAX;
};

inline VocabularyContext vocabularyContext(const sentencemining::BookContext& book) {
  return {book.bookTitle, book.bookAuthor, book.series, book.seriesIndex,
          book.chapter,   book.page,       book.spine,  book.startOffset};
}

class VocabularyStore {
 public:
  static constexpr const char* FILE_PATH = "/Vocabulary.txt";

  // A failed export must never prevent a successful dictionary lookup.
  static bool append(const VocabularyContext& context, const std::string& word, const std::string& headword,
                     const std::string& definition, bool htmlDefinition);
};
