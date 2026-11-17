#pragma once

#include <cstdint>
#include <string>

#include "Epub.h"

namespace serialization {

inline void writeString(HalFile& file, const std::string& s) {
  const auto len = static_cast<uint32_t>(s.size());
  file.writeBytes(std::string(reinterpret_cast<const char*>(&len), sizeof(len)) + s);
}

inline bool readString(HalFile& file, std::string& out) {
  uint32_t len = 0;
  if (!file.readBytes(reinterpret_cast<char*>(&len), sizeof(len))) return false;
  out.resize(len);
  return file.readBytes(out.data(), len);
}

}  // namespace serialization
