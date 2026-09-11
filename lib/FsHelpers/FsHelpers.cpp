#include "FsHelpers.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

namespace FsHelpers {

namespace {
bool isHexDigit(const char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

uint8_t hexValue(const char c) {
  if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
  if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(10 + (c - 'a'));
  return static_cast<uint8_t>(10 + (c - 'A'));
}
}  // namespace

std::string decodeUriEscapes(const std::string& path) {
  std::string decoded;
  decoded.reserve(path.size());

  for (size_t i = 0; i < path.size(); i++) {
    if (path[i] == '%' && i + 2 < path.size() && isHexDigit(path[i + 1]) && isHexDigit(path[i + 2])) {
      const uint8_t value = static_cast<uint8_t>((hexValue(path[i + 1]) << 4) | hexValue(path[i + 2]));
      decoded += static_cast<char>(value);
      i += 2;
      continue;
    }

    decoded += path[i];
  }

  return decoded;
}

bool hasCompleteBmp(const char* moduleName, const char* path) {
  HalFile file;
  if (!Storage.openFileForRead(moduleName, path, file)) return false;
  // 14-byte file header plus a 40-byte BITMAPINFOHEADER: the smallest BMP this code ever writes.
  constexpr int MIN_BMP_BYTES = 14 + 40;
  uint8_t header[MIN_BMP_BYTES];
  // Read byte-wise: the buffer is not guaranteed 4-byte aligned and the C3 faults on unaligned
  // multi-byte loads.
  if (file.read(header, sizeof(header)) != MIN_BMP_BYTES) return false;
  if (header[0] != 'B' || header[1] != 'M') return false;

  const auto le32 = [&header](int offset) -> uint32_t {
    return static_cast<uint32_t>(header[offset]) | (static_cast<uint32_t>(header[offset + 1]) << 8) |
           (static_cast<uint32_t>(header[offset + 2]) << 16) | (static_cast<uint32_t>(header[offset + 3]) << 24);
  };
  const auto le16 = [&header](int offset) -> uint16_t {
    return static_cast<uint16_t>(header[offset] | (header[offset + 1] << 8));
  };

  const uint32_t declared = le32(2);
  const uint32_t pixelOffset = le32(10);
  const int32_t width = static_cast<int32_t>(le32(18));
  const int32_t rawHeight = static_cast<int32_t>(le32(22));
  const uint16_t bpp = le16(28);
  const uint32_t compression = le32(30);
  if (declared < MIN_BMP_BYTES || pixelOffset < MIN_BMP_BYTES) return false;

  // Mirror Bitmap::parseHeaders(): a complete file the renderer cannot decode is not usable, and
  // caching it as generated would stop the cover ever being rebuilt.
  if (!(bpp == 1 || bpp == 2 || bpp == 4 || bpp == 8 || bpp == 24 || bpp == 32)) return false;
  if (!(compression == 0 || (bpp == 32 && compression == 3))) return false;

  // Bound the dimensions before multiplying: these come from a file on the card, and an
  // unbounded width * height * bpp overflows and lets a short file look complete.
  constexpr int32_t MAX_BMP_DIMENSION = 20000;
  if (width <= 0 || width > MAX_BMP_DIMENSION) return false;
  // Widen before negating: -INT32_MIN is signed overflow, and rawHeight comes from the card.
  const int64_t height = rawHeight < 0 ? -static_cast<int64_t>(rawHeight) : static_cast<int64_t>(rawHeight);
  if (height <= 0 || height > MAX_BMP_DIMENSION) return false;

  const uint64_t rowBytes = ((static_cast<uint64_t>(width) * bpp + 31) / 32) * 4;
  const uint64_t required = static_cast<uint64_t>(pixelOffset) + rowBytes * static_cast<uint64_t>(height);
  const uint64_t actual = static_cast<uint64_t>(file.size());
  return actual >= required && actual >= declared;
}

std::string normalisePath(const std::string& path) {
  std::vector<std::string_view> components;
  components.reserve(8);  // Eight nested folders is more than we might expect

  size_t start = 0;
  for (size_t i = 0; i <= path.length(); ++i) {
    if (i == path.length() || path[i] == '/') {
      if (i > start) {
        std::string_view component(path.data() + start, i - start);
        if (component == ".") {
          // Drop no-op segments so "/." canonicalises to root rather than a distinct path.
        } else if (component == "..") {
          if (!components.empty()) {
            components.pop_back();
          }
        } else {
          components.push_back(component);
        }
      }
      start = i + 1;
    }
  }

  if (components.empty()) {
    return "";
  }

  size_t total_len = 0;
  for (const auto& c : components) {
    total_len += c.length() + 1;
  }

  std::string result;
  result.reserve(total_len - 1);

  for (size_t i = 0; i < components.size(); ++i) {
    if (i > 0) {
      result += '/';
    }
    result.append(components[i].data(), components[i].length());
  }

  return result;
}

bool naturalLess(const std::string& str1, const std::string& str2) {
  // Naive natural sort: numeric-aware, case-insensitive
  const char* s1 = str1.c_str();
  const char* s2 = str2.c_str();

  // ctype functions require unsigned char values: passing a negative char (UTF-8
  // bytes above 0x7f with signed char) is undefined behavior
  const auto isDigit = [](const char c) { return isdigit(static_cast<unsigned char>(c)) != 0; };

  // Iterate while both strings have characters
  while (*s1 && *s2) {
    // Check if both are at the start of a number
    if (isDigit(*s1) && isDigit(*s2)) {
      // Skip leading zeros and track them
      while (*s1 == '0') s1++;
      while (*s2 == '0') s2++;

      // Count digits to compare lengths first
      int len1 = 0, len2 = 0;
      while (isDigit(s1[len1])) len1++;
      while (isDigit(s2[len2])) len2++;

      // Different length so return smaller integer value
      if (len1 != len2) return len1 < len2;

      // Same length so compare digit by digit
      for (int i = 0; i < len1; i++) {
        if (s1[i] != s2[i]) return s1[i] < s2[i];
      }

      // Numbers equal so advance pointers
      s1 += len1;
      s2 += len2;
    } else {
      // Regular case-insensitive character comparison
      const int c1 = tolower(static_cast<unsigned char>(*s1));
      const int c2 = tolower(static_cast<unsigned char>(*s2));
      if (c1 != c2) return c1 < c2;
      s1++;
      s2++;
    }
  }

  // One string is prefix of other
  return *s1 == '\0' && *s2 != '\0';
}

void sortFileList(std::vector<std::string>& strs) {
  std::sort(begin(strs), end(strs), [](const std::string& str1, const std::string& str2) {
    // Directories first
    bool isDir1 = str1.back() == '/';
    bool isDir2 = str2.back() == '/';
    if (isDir1 != isDir2) return isDir1;

    return naturalLess(str1, str2);
  });
}

bool checkFileExtension(std::string_view fileName, const char* extension) {
  const size_t extLen = strlen(extension);
  if (fileName.length() < extLen) {
    return false;
  }

  const size_t offset = fileName.length() - extLen;
  for (size_t i = 0; i < extLen; i++) {
    if (tolower(static_cast<unsigned char>(fileName[offset + i])) !=
        tolower(static_cast<unsigned char>(extension[i]))) {
      return false;
    }
  }
  return true;
}

bool hasJpgExtension(std::string_view fileName) {
  return checkFileExtension(fileName, ".jpg") || checkFileExtension(fileName, ".jpeg");
}

bool hasPngExtension(std::string_view fileName) { return checkFileExtension(fileName, ".png"); }

bool hasBmpExtension(std::string_view fileName) { return checkFileExtension(fileName, ".bmp"); }

bool hasGifExtension(std::string_view fileName) { return checkFileExtension(fileName, ".gif"); }

bool hasEpubExtension(std::string_view fileName) { return checkFileExtension(fileName, ".epub"); }

bool hasXtcExtension(std::string_view fileName) {
  return checkFileExtension(fileName, ".xtc") || checkFileExtension(fileName, ".xtch");
}

bool hasTxtExtension(std::string_view fileName) { return checkFileExtension(fileName, ".txt"); }

bool hasMarkdownExtension(std::string_view fileName) { return checkFileExtension(fileName, ".md"); }

bool hasCssExtension(std::string_view fileName) { return checkFileExtension(fileName, ".css"); }

std::string extractFolderPath(const std::string& filePath) {
  const auto lastSlash = filePath.find_last_of('/');
  if (lastSlash == std::string::npos || lastSlash == 0) {
    return "/";
  }
  return filePath.substr(0, lastSlash);
}

bool isSafePathComponent(std::string_view name) {
  return !name.empty() && name.find_first_of("/\\") == std::string_view::npos && name != "." && name != "..";
}

void sanitizePathComponentForFat32(const char* input, char* output, size_t maxLen) {
  if (maxLen == 0) {
    return;
  }

  size_t i = 0;
  for (; i < maxLen - 1 && input[i] != '\0'; i++) {
    const char c = input[i];
    if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' ||
        c == ' ' || (c > 0x00 && c <= 0x1f)) {
      output[i] = '-';
    } else {
      output[i] = c;
    }
  }
  output[i] = '\0';
}

}  // namespace FsHelpers
