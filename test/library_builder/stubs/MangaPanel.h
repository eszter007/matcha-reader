#pragma once

#include <string>

#include "Epub.h"

// Shares bookMetadata with the Epub stub: a manga folder or .matcha reports what the test put there.
namespace manga {
class MangaBook {
  std::string path;
  std::string title;
  std::string author;

 public:
  explicit MangaBook(std::string path) : path(std::move(path)) {}

  bool loadMeta() {
    ++fake::parses;
    const auto& metadata = bookMetadata[path];
    if (!metadata.success) return false;
    title = metadata.title;
    author = metadata.author;
    return true;
  }
  const std::string& getMetaTitle() const { return title; }
  const std::string& getAuthor() const { return author; }
};
}  // namespace manga
