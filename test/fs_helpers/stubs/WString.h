#pragma once

// Minimal native stand-in for the Arduino String class, just enough to let
// FsHelpers.h's `const String&` overloads compile in the host test build.
#include <cstddef>  // size_t: <string> is not required to provide it
#include <string>

class String {
 public:
  String() = default;
  String(const char* s) : value_(s ? s : "") {}
  String(const std::string& s) : value_(s) {}

  const char* c_str() const { return value_.c_str(); }
  size_t length() const { return value_.length(); }

 private:
  std::string value_;
};
