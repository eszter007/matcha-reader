#pragma once

#include <cstddef>
#include <string>

class Epub {
 public:
  template <typename Output>
  bool readItemContentsToStream(const std::string&, Output&, size_t, bool = false) const {
    return false;
  }

  // Settable directly by tests (there is no real EPUB metadata to parse here).
  std::string language;
  const std::string& getLanguage() const { return language; }
};
